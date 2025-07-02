// frame_resizer.cpp - C++ implementation of frame resizer with Redis support
#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <signal.h>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <sys/stat.h>
#include <hiredis/hiredis.h>

using json = nlohmann::json;
using namespace std::chrono;

// Global flag for signal handling
volatile sig_atomic_t g_running = 1;

// Redis helper class
class RedisClient {
private:
    redisContext* context;
    bool connected;
    std::string service_name;

public:
    RedisClient(const std::string& name) : context(nullptr), connected(false), service_name(name) {
        connect();
    }

    void connect() {
        struct timeval timeout = { 2, 0 }; // 2 seconds timeout
        context = redisConnectWithTimeout("127.0.0.1", 6379, timeout);

        if (context == nullptr || context->err) {
            if (context) {
                std::cerr << "[Redis] Connection error: " << context->errstr << std::endl;
                redisFree(context);
                context = nullptr;
            }
            connected = false;
        } else {
            connected = true;
            std::cout << "[Redis] Connected successfully" << std::endl;
        }
    }

    void updateMetric(const std::string& key, const std::string& value) {
        if (!connected || !context) return;

        std::string metrics_key = "metrics:" + service_name;
        redisReply* reply = (redisReply*)redisCommand(context,
            "HSET %s %s %s", metrics_key.c_str(), key.c_str(), value.c_str());

        if (reply) {
            freeReplyObject(reply);
        } else if (context->err) {
            std::cerr << "[Redis] Error updating metric: " << context->errstr << std::endl;
            // Try to reconnect
            redisFree(context);
            context = nullptr;
            connected = false;
            connect();
        }
    }

    void updateMetric(const std::string& key, int value) {
        updateMetric(key, std::to_string(value));
    }

    void updateMetric(const std::string& key, uint64_t value) {
        updateMetric(key, std::to_string(value));
    }

    void updateMetric(const std::string& key, double value) {
        updateMetric(key, std::to_string(value));
    }

    void setServiceInfo(const json& info) {
        if (!connected || !context) return;

        std::string service_key = "service:" + service_name;
        for (auto& [key, value] : info.items()) {
            std::string val_str = value.is_string() ? value.get<std::string>() : value.dump();
            redisReply* reply = (redisReply*)redisCommand(context,
                "HSET %s %s %s", service_key.c_str(), key.c_str(), val_str.c_str());
            if (reply) freeReplyObject(reply);
        }
    }

    ~RedisClient() {
        if (context) {
            redisFree(context);
        }
    }
};

// Logger class (same as frame_publisher)
class Logger {
private:
    std::ofstream log_file;
    std::string service_name;
    bool console_output;

    std::string getCurrentTime() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        ss << "," << std::setfill('0') << std::setw(3) << ms.count();

        return ss.str();
    }

public:
    Logger(const std::string& name, bool console = true)
        : service_name(name), console_output(console) {
        mkdir("/var/log/jvideo", 0755);

        std::string log_path = "/var/log/jvideo/" + service_name + ".log";
        log_file.open(log_path, std::ios::app);

        if (!log_file.is_open()) {
            std::cerr << "Failed to open log file: " << log_path << std::endl;
        }
    }

    void log(const std::string& level, const std::string& message) {
        std::string timestamp = getCurrentTime();
        std::string log_line = timestamp + " - " + service_name + " - " + level + " - " + message;

        if (log_file.is_open()) {
            log_file << log_line << std::endl;
            log_file.flush();
        }

        if (console_output) {
            std::cout << "[CPP] " << message << std::endl;
        }
    }

    void info(const std::string& message) { log("INFO", message); }
    void warning(const std::string& message) { log("WARNING", message); }
    void error(const std::string& message) { log("ERROR", message); }
    void debug(const std::string& message) { log("DEBUG", message); }

    ~Logger() {
        if (log_file.is_open()) {
            log_file.close();
        }
    }
};

void signal_handler(int signal) {
    g_running = 0;
}

class FrameResizer {
private:
    zmq::context_t context;
    zmq::socket_t sub_socket;
    zmq::socket_t pub_socket;
    json config;
    Logger logger;
    RedisClient redis;

    // Statistics
    uint64_t frames_received = 0;
    uint64_t frames_resized = 0;
    uint64_t frames_published = 0;
    uint64_t frames_skipped = 0;
    uint64_t errors = 0;
    steady_clock::time_point start_time;

public:
    FrameResizer() : context(1),
                     sub_socket(context, ZMQ_SUB),
                     pub_socket(context, ZMQ_PUB),
                     logger("frame-resizer"),
                     redis("frame-resizer") {
        start_time = steady_clock::now();

        logger.info("Frame Resizer (C++) initializing");
        logger.info("Redis connection initialized");

        loadConfig();
        setupSockets();

        // Setup signal handlers
        signal(SIGINT, [](int sig) {
            Logger temp_logger("frame-resizer");
            temp_logger.info("Received signal " + std::to_string(sig) + ", shutting down...");
            signal_handler(sig);
        });
        signal(SIGTERM, [](int sig) {
            Logger temp_logger("frame-resizer");
            temp_logger.info("Received signal " + std::to_string(sig) + ", shutting down...");
            signal_handler(sig);
        });
    }

    void loadConfig() {
        // Default configuration
        config = {
            {"subscribe_port", 5555},
            {"subscribe_host", "localhost"},
            {"publish_port", 5556},
            {"output_width", 320},
            {"output_height", 240},
            {"maintain_aspect_ratio", true},
            {"interpolation", "linear"},
            {"quality", 95},
            {"add_resize_info", true},
            {"add_timestamp", true},
            {"skip_frames", 0},
            {"max_fps", 0}
        };

        // Try to load from file
        std::string config_path = "/etc/jvideo/frame-resizer.conf";
        const char* env_path = std::getenv("FRAME_RESIZER_CONFIG");
        if (env_path) {
            config_path = env_path;
        }

        std::ifstream config_file(config_path);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;
                config.merge_patch(user_config);
                logger.info("Loaded config from " + config_path);
            } catch (const std::exception& e) {
                logger.error("Failed to parse config: " + std::string(e.what()));
            }
        }

        // Environment variable overrides
        const char* resize_width = std::getenv("JVIDEO_RESIZE_WIDTH");
        if (resize_width) config["output_width"] = std::stoi(resize_width);

        const char* resize_height = std::getenv("JVIDEO_RESIZE_HEIGHT");
        if (resize_height) config["output_height"] = std::stoi(resize_height);
    }

    void setupSockets() {
        // Setup subscriber
        std::string sub_addr = "tcp://" + config["subscribe_host"].get<std::string>()
                              + ":" + std::to_string(config["subscribe_port"].get<int>());
        sub_socket.connect(sub_addr);
        sub_socket.set(zmq::sockopt::subscribe, "");
        logger.info("Subscriber connected to " + sub_addr);

        // Setup publisher
        int pub_port = config["publish_port"];
        pub_socket.bind("tcp://*:" + std::to_string(pub_port));
        logger.info("Publisher bound to tcp://*:" + std::to_string(pub_port));

        // Give sockets time to establish
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Store service info in Redis
        json service_info;
        service_info["service"] = "frame-resizer";
        service_info["status"] = "running";
        service_info["input_port"] = config["subscribe_port"];
        service_info["output_port"] = config["publish_port"];
        service_info["output_size"] = std::to_string(config["output_width"].get<int>()) + "x" +
                                      std::to_string(config["output_height"].get<int>());
        service_info["implementation"] = "cpp";
        redis.setServiceInfo(service_info);
    }

    int getInterpolationMethod() {
        std::string method = config["interpolation"];
        if (method == "nearest") return cv::INTER_NEAREST;
        if (method == "cubic") return cv::INTER_CUBIC;
        if (method == "area") return cv::INTER_AREA;
        if (method == "lanczos") return cv::INTER_LANCZOS4;
        return cv::INTER_LINEAR;  // default
    }

    cv::Size calculateResizeDimensions(int original_width, int original_height) {
        int target_width = config["output_width"];
        int target_height = config["output_height"];

        if (config["maintain_aspect_ratio"]) {
            double aspect_ratio = static_cast<double>(original_width) / original_height;
            double target_ratio = static_cast<double>(target_width) / target_height;

            if (aspect_ratio > target_ratio) {
                // Original is wider - fit to width
                return cv::Size(target_width, static_cast<int>(target_width / aspect_ratio));
            } else {
                // Original is taller - fit to height
                return cv::Size(static_cast<int>(target_height * aspect_ratio), target_height);
            }
        }

        return cv::Size(target_width, target_height);
    }

    cv::Mat resizeFrame(const cv::Mat& frame, const json& metadata) {
        cv::Size new_size = calculateResizeDimensions(frame.cols, frame.rows);

        cv::Mat resized;
        cv::resize(frame, resized, new_size, 0, 0, getInterpolationMethod());

        // Add padding if maintaining aspect ratio
        if (config["maintain_aspect_ratio"]) {
            int target_width = config["output_width"];
            int target_height = config["output_height"];

            if (resized.cols != target_width || resized.rows != target_height) {
                cv::Mat canvas = cv::Mat::zeros(target_height, target_width, frame.type());

                int x_offset = (target_width - resized.cols) / 2;
                int y_offset = (target_height - resized.rows) / 2;

                resized.copyTo(canvas(cv::Rect(x_offset, y_offset, resized.cols, resized.rows)));
                resized = canvas;
            }
        }

        // Add overlays
        if (config["add_resize_info"]) {
            std::stringstream ss;
            ss << "Resized: " << frame.cols << "x" << frame.rows
               << " -> " << resized.cols << "x" << resized.rows;
            cv::putText(resized, ss.str(),
                       cv::Point(10, resized.rows - 40), cv::FONT_HERSHEY_SIMPLEX,
                       0.5, cv::Scalar(255, 255, 255), 1);
        }

        if (config["add_timestamp"]) {
            auto now = system_clock::now();
            auto time_t = system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
            cv::putText(resized, ss.str(),
                       cv::Point(resized.cols - 80, 20), cv::FONT_HERSHEY_SIMPLEX,
                       0.5, cv::Scalar(255, 255, 255), 1);
        }

        // Add service identifier
        cv::putText(resized, "[RESIZER-CPP]",
                   cv::Point(resized.cols - 100, resized.rows - 10),
                   cv::FONT_HERSHEY_SIMPLEX,
                   0.4, cv::Scalar(128, 128, 128), 1);

        return resized;
    }

    bool processFrame() {
        try {
            zmq::message_t metadata_msg;
            zmq::message_t frame_msg;

            // Try to receive with timeout
            zmq::recv_result_t result = sub_socket.recv(metadata_msg, zmq::recv_flags::dontwait);
            if (!result.has_value()) {
                return false;  // No message available
            }

            // Receive frame data
            result = sub_socket.recv(frame_msg, zmq::recv_flags::none);
            if (!result.has_value()) {
                logger.warning("Failed to receive frame data");
                return false;
            }

            frames_received++;
            redis.updateMetric("frames_received", frames_received);

            // Skip frames if configured
            if (config["skip_frames"].get<int>() > 0) {
                if (frames_received % (config["skip_frames"].get<int>() + 1) != 0) {
                    frames_skipped++;
                    redis.updateMetric("frames_skipped", frames_skipped);
                    return true;
                }
            }

            // Parse metadata
            std::string meta_str(static_cast<char*>(metadata_msg.data()), metadata_msg.size());
            json metadata = json::parse(meta_str);

            // Decode frame
            int width = metadata["width"];
            int height = metadata["height"];
            int channels = metadata["channels"];

            cv::Mat frame(height, width, channels == 3 ? CV_8UC3 : CV_8UC1, frame_msg.data());

            // Resize frame
            cv::Mat resized = resizeFrame(frame, metadata);
            frames_resized++;
            redis.updateMetric("frames_resized", frames_resized);

            // Prepare metadata for resized frame
            json resized_metadata;
            resized_metadata["frame_id"] = metadata.value("frame_id", frames_resized);
            resized_metadata["timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();
            resized_metadata["original_width"] = width;
            resized_metadata["original_height"] = height;
            resized_metadata["width"] = resized.cols;
            resized_metadata["height"] = resized.rows;
            resized_metadata["channels"] = resized.channels();
            resized_metadata["resizer"] = "cpp";
            resized_metadata["interpolation"] = config["interpolation"];
            resized_metadata["source_timestamp"] = metadata.value("timestamp", 0.0);

            // Send resized frame
            std::string meta_out = resized_metadata.dump();
            zmq::message_t meta_out_msg(meta_out.begin(), meta_out.end());
            pub_socket.send(meta_out_msg, zmq::send_flags::sndmore);

            zmq::message_t frame_out_msg(resized.data, resized.total() * resized.elemSize());
            pub_socket.send(frame_out_msg, zmq::send_flags::none);

            frames_published++;
            redis.updateMetric("frames_published", frames_published);

            return true;

        } catch (const zmq::error_t& e) {
            logger.error("ZMQ error: " + std::string(e.what()));
            errors++;
            redis.updateMetric("errors", errors);
            return false;
        } catch (const std::exception& e) {
            logger.error("Error processing frame: " + std::string(e.what()));
            errors++;
            redis.updateMetric("errors", errors);
            return false;
        }
    }

    void run() {
        logger.info("Starting frame resizing");

        std::stringstream ss;
        ss << "Input: port " << config["subscribe_port"]
           << ", Output: " << config["output_width"] << "x" << config["output_height"]
           << " on port " << config["publish_port"];
        logger.info(ss.str());

        auto last_log_time = steady_clock::now();
        auto last_frame_time = steady_clock::now();

        // FPS limiting
        int max_fps = config["max_fps"];
        auto frame_duration = max_fps > 0 ?
            std::chrono::milliseconds(1000 / max_fps) :
            std::chrono::milliseconds(0);

        while (g_running) {
            auto frame_start = steady_clock::now();

            bool processed = processFrame();

            // Log progress periodically
            auto now = steady_clock::now();
            if (duration_cast<seconds>(now - last_log_time).count() >= 5) {
                auto elapsed = duration<double>(now - start_time).count();
                double fps = frames_resized / elapsed;
                double efficiency = frames_received > 0 ?
                    (100.0 * frames_resized / frames_received) : 0.0;

                std::stringstream log_ss;
                log_ss << "Metrics - Received: " << frames_received
                       << ", Resized: " << frames_resized
                       << ", Published: " << frames_published
                       << ", Skipped: " << frames_skipped
                       << ", FPS: " << std::fixed << std::setprecision(2) << fps
                       << ", Efficiency: " << std::setprecision(1) << efficiency << "%"
                       << ", Errors: " << errors;
                logger.info(log_ss.str());

                // Update Redis metrics
                redis.updateMetric("fps", fps);
                redis.updateMetric("efficiency", efficiency);
                redis.updateMetric("uptime", elapsed);
                auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
                redis.updateMetric("last_update", std::to_string(timestamp));

                last_log_time = now;
            }

            // FPS limiting
            if (processed && frame_duration.count() > 0) {
                auto elapsed = steady_clock::now() - frame_start;
                if (elapsed < frame_duration) {
                    std::this_thread::sleep_for(frame_duration - elapsed);
                }
            } else if (!processed) {
                // No frame available, brief sleep
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        logger.info("Frame resizing stopped");

        std::stringstream final_ss;
        final_ss << "Total processed: " << frames_resized << "/" << frames_received << " frames";
        logger.info(final_ss.str());

        // Final metrics update
        redis.updateMetric("status", "stopped");
        redis.updateMetric("final_frames_received", frames_received);
        redis.updateMetric("final_frames_resized", frames_resized);
        redis.updateMetric("final_frames_published", frames_published);
    }
};

int main(int argc, char* argv[]) {
    Logger logger("frame-resizer");
    logger.info("Juni's Frame Resizer (C++ Implementation)");

    // Parse command line arguments
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--help") {
            std::cout << "Frame Resizer Service (C++)" << std::endl;
            std::cout << "Environment variables:" << std::endl;
            std::cout << "  JVIDEO_RESIZE_WIDTH    - Output width (default: 320)" << std::endl;
            std::cout << "  JVIDEO_RESIZE_HEIGHT   - Output height (default: 240)" << std::endl;
            std::cout << "  JVIDEO_SUBSCRIBE_PORT  - Input port (default: 5555)" << std::endl;
            std::cout << "  JVIDEO_PUBLISH_PORT    - Output port (default: 5556)" << std::endl;
            return 0;
        }
    }

    try {
        FrameResizer resizer;
        resizer.run();
    } catch (const std::exception& e) {
        logger.error("Fatal error: " + std::string(e.what()));
        return 1;
    }

    return 0;
}
