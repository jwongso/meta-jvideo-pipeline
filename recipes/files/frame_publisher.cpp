#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <signal.h>
#include <cmath>
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

// Logger class (same as before)
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

enum class SourceType {
    CAMERA,
    SYNTHETIC
};

class FramePublisher {
private:
    zmq::context_t context;
    zmq::socket_t socket;
    json config;
    SourceType source_type;
    cv::VideoCapture cap;
    Logger logger;
    RedisClient redis;

    int frame_count = 0;
    steady_clock::time_point start_time;
    int width;
    int height;
    int fps;

    // Metrics
    int frames_processed = 0;
    int errors = 0;

public:
    FramePublisher() : context(1), socket(context, ZMQ_PUB),
                       logger("frame-publisher"), redis("frame-publisher") {
        start_time = steady_clock::now();

        logger.info("ZeroMQ context created");
        logger.info("Redis connection initialized");

        loadConfig();

        // Setup ZMQ socket
        int port = config.value("publish_port", 5555);
        socket.bind("tcp://*:" + std::to_string(port));
        logger.info("Publisher bound to tcp://*:" + std::to_string(port));

        // Give subscribers time to connect
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    void loadConfig() {
        // Default configuration
        config = {
            {"source_type", "synthetic"},
            {"publish_port", 5555},
            {"camera_device", 0},
            {"camera_backend", "auto"},
            {"width", 640},
            {"height", 480},
            {"fps", 30},
            {"synthetic_pattern", "moving_shapes"},
            {"synthetic_motion_speed", 1.0},
            {"add_timestamp", true},
            {"add_frame_number", true},
            {"add_source_label", true}
        };

        // Try to load from file
        std::string config_path = "/etc/jvideo/frame-publisher.conf";
        const char* env_path = std::getenv("FRAME_PUBLISHER_CONFIG");
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

        // Parse source type
        std::string source_str = config["source_type"];
        source_type = (source_str == "camera") ? SourceType::CAMERA : SourceType::SYNTHETIC;

        // Get dimensions
        width = config["width"];
        height = config["height"];
        fps = config["fps"];
    }

    bool initCamera() {
        int device = config["camera_device"];
        logger.info("Initializing camera device " + std::to_string(device));

        // Try to open camera
        cap.open(device);

        if (!cap.isOpened()) {
            logger.error("Failed to open camera device " + std::to_string(device));

            // Try alternative devices
            for (int alt : {0, 1}) {
                if (alt != device) {
                    logger.info("Trying device: " + std::to_string(alt));
                    cap.open(alt);
                    if (cap.isOpened()) {
                        device = alt;
                        break;
                    }
                }
            }

            if (!cap.isOpened()) {
                logger.error("No camera found, falling back to synthetic");
                source_type = SourceType::SYNTHETIC;
                return initSynthetic();
            }
        }

        // Configure camera
        cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
        cap.set(cv::CAP_PROP_FPS, fps);
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

        // Get actual settings
        int actual_width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        int actual_height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        double actual_fps = cap.get(cv::CAP_PROP_FPS);

        std::stringstream ss;
        ss << "Camera initialized: " << actual_width << "x" << actual_height
           << " @ " << actual_fps << "fps";
        logger.info(ss.str());

        // Update with actual values
        width = actual_width;
        height = actual_height;
        if (actual_fps > 0) fps = static_cast<int>(actual_fps);

        return true;
    }

    bool initSynthetic() {
        std::stringstream ss;
        ss << "Synthetic source initialized: " << width << "x" << height
           << " @ " << fps << "fps, pattern: " << config["synthetic_pattern"];
        logger.info(ss.str());

        // Store source info in Redis
        json source_info;
        source_info["source_type"] = "synthetic";
        source_info["width"] = width;
        source_info["height"] = height;
        source_info["fps"] = fps;
        source_info["pattern"] = config["synthetic_pattern"];
        source_info["implementation"] = "cpp";
        redis.setServiceInfo(source_info);

        return true;
    }

    cv::Mat generateSyntheticFrame() {
        cv::Mat frame(height, width, CV_8UC3);

        auto now = steady_clock::now();
        double t = duration<double>(now - start_time).count() * config["synthetic_motion_speed"].get<double>();

        std::string pattern = config["synthetic_pattern"];

        if (pattern == "moving_shapes") {
            // Background gradient
            for (int y = 0; y < height; y++) {
                int val = 20 + 10 * std::sin(y / 50.0 + t * 0.5);
                cv::Vec3b color(val/3, val/2, val);
                for (int x = 0; x < width; x++) {
                    frame.at<cv::Vec3b>(y, x) = color;
                }
            }

            // Moving circles
            for (int i = 0; i < 3; i++) {
                double phase = 2 * M_PI * i / 3;
                int cx = width * (0.5 + 0.3 * std::sin(t + phase));
                int cy = height * (0.5 + 0.3 * std::cos(t * 0.7 + phase));

                cv::Scalar color(
                    127 + 127 * std::sin(t + phase),
                    127 + 127 * std::sin(t + phase + 2*M_PI/3),
                    127 + 127 * std::sin(t + phase + 4*M_PI/3)
                );
                cv::circle(frame, cv::Point(cx, cy), 30, color, -1);
            }

            // Moving rectangle
            int rx = width * (0.5 + 0.4 * std::cos(t * 0.8));
            int ry = height / 3;
            cv::rectangle(frame,
                         cv::Point(rx - 40, ry - 20),
                         cv::Point(rx + 40, ry + 20),
                         cv::Scalar(0, 200, 255), -1);
        }
        else if (pattern == "color_bars") {
            // SMPTE-style color bars
            std::vector<cv::Scalar> colors = {
                cv::Scalar(192, 192, 192),  // Gray
                cv::Scalar(0, 192, 192),    // Yellow (BGR)
                cv::Scalar(192, 192, 0),    // Cyan
                cv::Scalar(0, 192, 0),      // Green
                cv::Scalar(192, 0, 192),    // Magenta
                cv::Scalar(0, 0, 192),      // Red
                cv::Scalar(192, 0, 0),      // Blue
            };

            int bar_width = width / colors.size();
            for (size_t i = 0; i < colors.size(); i++) {
                int x_start = i * bar_width;
                int x_end = (i == colors.size() - 1) ? width : (i + 1) * bar_width;
                cv::rectangle(frame,
                             cv::Point(x_start, 0),
                             cv::Point(x_end, height),
                             colors[i], -1);
            }

            // Animated line
            int line_y = height * (0.5 + 0.4 * std::sin(t));
            cv::line(frame, cv::Point(0, line_y), cv::Point(width, line_y),
                     cv::Scalar(255, 255, 255), 2);
        }
        else if (pattern == "checkerboard") {
            frame = cv::Scalar(0, 0, 0);
            int square_size = 40;
            int offset = static_cast<int>(t * 10) % (square_size * 2);

            for (int y = 0; y < height; y += square_size) {
                for (int x = 0; x < width; x += square_size) {
                    if (((x + offset) / square_size + y / square_size) % 2 == 0) {
                        cv::rectangle(frame,
                                     cv::Point(x, y),
                                     cv::Point(std::min(x + square_size, width),
                                              std::min(y + square_size, height)),
                                     cv::Scalar(255, 255, 255), -1);
                    }
                }
            }
        }

        return frame;
    }

    void addOverlays(cv::Mat& frame) {
        if (config["add_source_label"]) {
            std::string label = source_type == SourceType::CAMERA ? "CAMERA" : "SYNTHETIC";
            if (source_type == SourceType::SYNTHETIC) {
                label += " (" + config["synthetic_pattern"].get<std::string>() + ")";
            }
            cv::putText(frame, label,
                       cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX,
                       0.7, cv::Scalar(255, 255, 255), 2);
        }

        if (config["add_frame_number"]) {
            cv::putText(frame, "Frame: " + std::to_string(frame_count),
                       cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX,
                       0.6, cv::Scalar(255, 255, 255), 1);
        }

        if (config["add_timestamp"]) {
            auto now = system_clock::now();
            auto time_t = system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
            cv::putText(frame, ss.str(),
                       cv::Point(width - 100, 30), cv::FONT_HERSHEY_SIMPLEX,
                       0.6, cv::Scalar(255, 255, 255), 1);
        }

        // FPS indicator
        auto elapsed = duration<double>(steady_clock::now() - start_time).count();
        if (elapsed > 0) {
            double actual_fps = frame_count / elapsed;
            cv::putText(frame, "FPS: " + std::to_string(static_cast<int>(actual_fps)),
                       cv::Point(width - 100, 60), cv::FONT_HERSHEY_SIMPLEX,
                       0.6, cv::Scalar(255, 255, 255), 1);
        }

        // C++ indicator
        cv::putText(frame, "[C++]",
                   cv::Point(width - 50, height - 10), cv::FONT_HERSHEY_SIMPLEX,
                   0.5, cv::Scalar(128, 128, 128), 1);
    }

    void run() {
        // Initialize source
        bool initialized = false;
        if (source_type == SourceType::CAMERA) {
            initialized = initCamera();
        } else {
            initialized = initSynthetic();
        }

        if (!initialized) {
            logger.error("Failed to initialize source");
            return;
        }

        std::stringstream ss;
        ss << "Starting frame publishing from "
           << (source_type == SourceType::CAMERA ? "camera" : "synthetic");
        logger.info(ss.str());

        ss.str("");
        ss << "Target: " << width << "x" << height << " @ " << fps << "fps";
        logger.info(ss.str());

        auto frame_duration = std::chrono::milliseconds(1000 / fps);
        auto last_log_time = steady_clock::now();

        // Setup signal handlers
        signal(SIGINT, [](int sig) {
            Logger temp_logger("frame-publisher");
            temp_logger.info("Received signal " + std::to_string(sig) + ", shutting down...");
            signal_handler(sig);
        });
        signal(SIGTERM, [](int sig) {
            Logger temp_logger("frame-publisher");
            temp_logger.info("Received signal " + std::to_string(sig) + ", shutting down...");
            signal_handler(sig);
        });

        while (g_running) {
            auto frame_start = steady_clock::now();

            cv::Mat frame;

            // Get frame based on source type
            if (source_type == SourceType::CAMERA) {
                cap >> frame;
                if (frame.empty()) {
                    logger.error("Failed to capture frame");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            } else {
                frame = generateSyntheticFrame();
            }

            // Add overlays
            addOverlays(frame);

            // Prepare metadata
            json metadata;
            metadata["frame_id"] = frame_count;
            metadata["timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();
            metadata["width"] = frame.cols;
            metadata["height"] = frame.rows;
            metadata["channels"] = 3;
            metadata["source_type"] = source_type == SourceType::CAMERA ? "camera" : "synthetic";
            metadata["implementation"] = "cpp";

            try {
                // Send metadata
                std::string meta_str = metadata.dump();
                zmq::message_t meta_msg(meta_str.begin(), meta_str.end());
                socket.send(meta_msg, zmq::send_flags::sndmore);

                // Send frame data
                zmq::message_t frame_msg(frame.data, frame.total() * frame.elemSize());
                socket.send(frame_msg, zmq::send_flags::none);

                frame_count++;
                frames_processed++;

                // Update Redis metrics
                redis.updateMetric("frames_processed", frames_processed);
                redis.updateMetric("last_frame_id", frame_count);
                redis.updateMetric("errors", errors);

            } catch (const zmq::error_t& e) {
                logger.error("ZMQ error: " + std::string(e.what()));
                errors++;
                redis.updateMetric("errors", errors);
            }

            // Log progress periodically
            auto now = steady_clock::now();
            if (duration_cast<seconds>(now - last_log_time).count() >= 5) {
                auto elapsed = duration<double>(now - start_time).count();
                double actual_fps = frames_processed / elapsed;

                std::stringstream log_ss;
                log_ss << "Metrics - Frames: " << frames_processed
                       << ", FPS: " << std::fixed << std::setprecision(2) << actual_fps
                       << ", Errors: " << errors;
                logger.info(log_ss.str());

                // Update FPS in Redis
                redis.updateMetric("fps", actual_fps);
                redis.updateMetric("uptime", elapsed);

                last_log_time = now;
            }

            // Maintain frame rate
            auto frame_end = steady_clock::now();
            auto elapsed = frame_end - frame_start;
            if (elapsed < frame_duration) {
                std::this_thread::sleep_for(frame_duration - elapsed);
            }
        }

        logger.info("Frame publishing stopped");
        logger.info("Shutting down frame publisher");

        std::stringstream final_ss;
        final_ss << "Total frames processed: " << frames_processed;
        logger.info(final_ss.str());
    }

    ~FramePublisher() {
        if (cap.isOpened()) {
            cap.release();
        }
    }
};

int main(int argc, char* argv[]) {
    // Setup signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    Logger logger("frame-publisher");
    logger.info("Juni's Frame Publisher (C++ Implementation)");

    // Parse command line arguments
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "camera" || arg == "synthetic") {
            setenv("JVIDEO_SOURCE_TYPE", arg.c_str(), 1);
            logger.info("Using source type: " + arg);
        }
    }

    try {
        FramePublisher publisher;
        publisher.run();
    } catch (const std::exception& e) {
        logger.error("Fatal error: " + std::string(e.what()));
        return 1;
    }

    return 0;
}
