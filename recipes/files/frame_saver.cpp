// frame_saver.cpp - C++ implementation of frame saver
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
#include <filesystem>
#include <hiredis/hiredis.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

// Global flag for signal handling
volatile sig_atomic_t g_running = 1;

// Redis helper class (same as frame_resizer)
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

// Logger class (same as frame_resizer)
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

class FrameSaver {
private:
    zmq::context_t context;
    zmq::socket_t sub_socket;
    json config;
    Logger logger;
    RedisClient redis;

    // Statistics
    uint64_t frames_received = 0;
    uint64_t frames_saved = 0;
    uint64_t bytes_written = 0;
    uint64_t errors = 0;
    steady_clock::time_point start_time;

public:
    FrameSaver() : context(1),
                   sub_socket(context, ZMQ_SUB),
                   logger("frame-saver"),
                   redis("frame-saver") {
        start_time = steady_clock::now();

        logger.info("Frame Saver (C++) initializing");
        logger.info("Redis connection initialized");

        loadConfig();
        setupSubscriber();
        ensureOutputDirectory();

        // Setup signal handlers
        signal(SIGINT, [](int sig) {
            Logger temp_logger("frame-saver");
            temp_logger.info("Received signal " + std::to_string(sig) + ", shutting down...");
            signal_handler(sig);
        });
        signal(SIGTERM, [](int sig) {
            Logger temp_logger("frame-saver");
            temp_logger.info("Received signal " + std::to_string(sig) + ", shutting down...");
            signal_handler(sig);
        });
    }

    void loadConfig() {
        // Default configuration
        config = {
            {"subscribe_port", 5556},
            {"subscribe_host", "localhost"},
            {"output_dir", "/var/lib/jvideo/frames"},
            {"format", "jpg"},
            {"quality", 90},
            {"compression", 6},
            {"naming_pattern", "frame_{timestamp}_{frame_id}.{ext}"},
            {"timestamp_format", "%Y%m%d_%H%M%S_%f"},
            {"max_files", 1000},
            {"max_size_mb", 500},
            {"purge_oldest", true},
            {"save_every_n", 1},
            {"min_interval_ms", 0},
            {"max_queue_size", 10},
            {"max_fps", 10},
            {"save_metadata", true},
            {"add_timestamp_overlay", true}
        };

        // Try to load from file
        std::string config_path = "/etc/jvideo/frame-saver.conf";
        const char* env_path = std::getenv("FRAME_SAVER_CONFIG");
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
        const char* output_dir = std::getenv("JVIDEO_OUTPUT_DIR");
        if (output_dir) config["output_dir"] = output_dir;

        const char* format = std::getenv("JVIDEO_FORMAT");
        if (format) config["format"] = format;

        const char* subscribe_port = std::getenv("JVIDEO_SUBSCRIBE_PORT");
        if (subscribe_port) config["subscribe_port"] = std::stoi(subscribe_port);

        const char* save_every_n = std::getenv("JVIDEO_SAVE_EVERY_N");
        if (save_every_n) config["save_every_n"] = std::stoi(save_every_n);
    }

    void setupSubscriber() {
        // Setup subscriber
        std::string sub_addr = "tcp://" + config["subscribe_host"].get<std::string>()
                              + ":" + std::to_string(config["subscribe_port"].get<int>());
        sub_socket.connect(sub_addr);
        sub_socket.set(zmq::sockopt::subscribe, "");
        logger.info("Subscriber connected to " + sub_addr);

        // Give socket time to establish
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Store service info in Redis
        json service_info;
        service_info["service"] = "frame-saver";
        service_info["status"] = "running";
        service_info["input_port"] = config["subscribe_port"];
        service_info["output_dir"] = config["output_dir"];
        service_info["format"] = config["format"];
        service_info["implementation"] = "cpp";
        redis.setServiceInfo(service_info);
    }

    void ensureOutputDirectory() {
        fs::path output_dir = config["output_dir"].get<std::string>();
        try {
            if (!fs::exists(output_dir)) {
                fs::create_directories(output_dir);
            }
            logger.info("Output directory: " + output_dir.string());
        } catch (const std::exception& e) {
            logger.error("Failed to create output directory: " + std::string(e.what()));
        }
    }

    std::string generateFilename(const json& metadata) {
        // Get current time
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        // Format timestamp
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), config["timestamp_format"].get<std::string>().c_str());
        std::string timestamp = ss.str();

        // Get frame ID
        std::string frame_id;
        if (metadata.contains("frame_id")) {
            frame_id = std::to_string(metadata["frame_id"].get<int>());
        } else {
            // Generate random ID
            frame_id = std::to_string(std::rand() % 10000);
        }

        // Format filename
        std::string pattern = config["naming_pattern"];
        std::string ext = config["format"];

        // Replace placeholders
        size_t pos = 0;
        while ((pos = pattern.find("{timestamp}", pos)) != std::string::npos) {
            pattern.replace(pos, 11, timestamp);
            pos += timestamp.length();
        }

        pos = 0;
        while ((pos = pattern.find("{frame_id}", pos)) != std::string::npos) {
            pattern.replace(pos, 10, frame_id);
            pos += frame_id.length();
        }

        pos = 0;
        while ((pos = pattern.find("{ext}", pos)) != std::string::npos) {
            pattern.replace(pos, 5, ext);
            pos += ext.length();
        }

        return (fs::path(config["output_dir"].get<std::string>()) / pattern).string();
    }

    bool saveFrame(const cv::Mat& frame, const json& metadata) {
        std::string filename = generateFilename(metadata);

        try {
            // Add timestamp overlay if configured
            cv::Mat output_frame = frame.clone();
            if (config["add_timestamp_overlay"]) {
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                std::stringstream ss;
                ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

                cv::putText(output_frame, ss.str(),
                           cv::Point(10, output_frame.rows - 10), cv::FONT_HERSHEY_SIMPLEX,
                           0.5, cv::Scalar(255, 255, 255), 1);

                if (metadata.contains("frame_id")) {
                    cv::putText(output_frame, "Frame #" + std::to_string(metadata["frame_id"].get<int>()),
                               cv::Point(10, 20), cv::FONT_HERSHEY_SIMPLEX,
                               0.5, cv::Scalar(255, 255, 255), 1);
                }
            }

            // Save frame based on format
            std::vector<int> params;
            if (config["format"] == "jpg") {
                params.push_back(cv::IMWRITE_JPEG_QUALITY);
                params.push_back(config["quality"].get<int>());
                cv::imwrite(filename, output_frame, params);
            } else if (config["format"] == "png") {
                params.push_back(cv::IMWRITE_PNG_COMPRESSION);
                params.push_back(config["compression"].get<int>());
                cv::imwrite(filename, output_frame, params);
            } else if (config["format"] == "raw") {
                std::ofstream file(filename, std::ios::binary);
                if (!file.is_open()) {
                    throw std::runtime_error("Failed to open file for writing");
                }
                file.write(reinterpret_cast<const char*>(output_frame.data),
                          output_frame.total() * output_frame.elemSize());
                file.close();
            } else {
                // Default to jpg
                params.push_back(cv::IMWRITE_JPEG_QUALITY);
                params.push_back(config["quality"].get<int>());
                cv::imwrite(filename, output_frame, params);
            }

            // Get file size
            uintmax_t file_size = fs::file_size(filename);
            bytes_written += file_size;

            // Save metadata if configured
            if (config["save_metadata"]) {
                std::string meta_filename = filename.substr(0, filename.find_last_of('.')) + ".json";
                std::ofstream meta_file(meta_filename);
                if (meta_file.is_open()) {
                    json save_metadata = metadata;
                    auto now = std::chrono::system_clock::now();
                    auto time_t = std::chrono::system_clock::to_time_t(now);
                    std::stringstream ss;
                    ss << std::put_time(std::localtime(&time_t), "%FT%T");

                    save_metadata["saved_at"] = ss.str();
                    save_metadata["saved_by"] = "frame-saver-cpp";
                    save_metadata["file_size"] = file_size;
                    save_metadata["file_path"] = filename;

                    meta_file << save_metadata.dump(2);
                    meta_file.close();
                }
            }

            frames_saved++;
            redis.updateMetric("frames_saved", frames_saved);
            redis.updateMetric("bytes_written", bytes_written);

            if (frames_saved % 100 == 0) {
                std::stringstream ss;
                ss << "Saved frame #" << frames_saved << " to " << filename
                   << " (" << std::fixed << std::setprecision(1) << file_size/1024.0 << " KB)";
                logger.info(ss.str());
            }

            return true;

        } catch (const std::exception& e) {
            logger.error("Error saving frame to " + filename + ": " + std::string(e.what()));
            errors++;
            redis.updateMetric("errors", errors);
            return false;
        }
    }

    void manageStorage() {
        if (!config["purge_oldest"].get<bool>()) {
            return;
        }

        fs::path output_dir = config["output_dir"].get<std::string>();
        std::string format = config["format"];

        try {
            // Check file count limit
            if (config["max_files"].get<int>() > 0) {
                std::vector<fs::path> files;
                for (const auto& entry : fs::directory_iterator(output_dir)) {
                    if (entry.is_regular_file() && entry.path().extension() == "." + format) {
                        files.push_back(entry.path());
                    }
                }

                if (files.size() > config["max_files"].get<size_t>()) {
                    // Sort by creation time
                    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
                        return fs::last_write_time(a) < fs::last_write_time(b);
                    });

                    // Delete oldest files
                    size_t files_to_delete = files.size() - config["max_files"].get<size_t>();
                    for (size_t i = 0; i < files_to_delete && i < files.size(); i++) {
                        try {
                            fs::remove(files[i]);
                            // Also delete metadata if exists
                            fs::path meta_file = files[i];
                            meta_file.replace_extension(".json");
                            if (fs::exists(meta_file)) {
                                fs::remove(meta_file);
                            }
                        } catch (const std::exception& e) {
                            logger.warning("Failed to delete " + files[i].string() + ": " + e.what());
                        }
                    }

                    logger.info("Purged " + std::to_string(files_to_delete) + " oldest files to maintain limit");
                }
            }

            // Check size limit
            if (config["max_size_mb"].get<int>() > 0) {
                uintmax_t total_size = 0;
                std::vector<fs::path> files;

                for (const auto& entry : fs::directory_iterator(output_dir)) {
                    if (entry.is_regular_file() && entry.path().extension() == "." + format) {
                        total_size += entry.file_size();
                        files.push_back(entry.path());
                    }
                }

                double total_size_mb = total_size / (1024.0 * 1024.0);
                if (total_size_mb > config["max_size_mb"].get<double>()) {
                    // Sort by creation time
                    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
                        return fs::last_write_time(a) < fs::last_write_time(b);
                    });

                    for (const auto& file : files) {
                        if (total_size_mb <= config["max_size_mb"].get<double>()) {
                            break;
                        }

                        try {
                            uintmax_t file_size = fs::file_size(file);
                            fs::remove(file);

                            // Also delete metadata if exists
                            fs::path meta_file = file;
                            meta_file.replace_extension(".json");
                            if (fs::exists(meta_file)) {
                                fs::remove(meta_file);
                            }

                            total_size_mb -= file_size / (1024.0 * 1024.0);
                        } catch (const std::exception& e) {
                            logger.warning("Failed to delete " + file.string() + ": " + e.what());
                        }
                    }

                    logger.info("Purged oldest files to maintain size limit of " +
                               std::to_string(config["max_size_mb"].get<int>()) + " MB");
                }
            }
        } catch (const std::exception& e) {
            logger.error("Error managing storage: " + std::string(e.what()));
        }
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

            // Check if we should save this frame based on config
            if (frames_received % config["save_every_n"].get<int>() != 0) {
                return true;
            }

            // Parse metadata
            std::string meta_str(static_cast<char*>(metadata_msg.data()), metadata_msg.size());
            json metadata = json::parse(meta_str);

            // Decode frame
            int width = metadata["width"];
            int height = metadata["height"];
            int channels = metadata["channels"];

            cv::Mat frame(height, width, channels == 3 ? CV_8UC3 : CV_8UC1, frame_msg.data());

            // Save frame
            saveFrame(frame, metadata);

            // Manage storage limits
            if (frames_saved % 50 == 0) {
                manageStorage();
            }

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
        logger.info("Starting frame saver");

        std::stringstream ss;
        ss << "Input: port " << config["subscribe_port"]
           << ", Output: " << config["output_dir"]
           << " (" << config["format"] << " format)";
        logger.info(ss.str());

        ss.str("");
        ss << "Save every " << config["save_every_n"]
           << " frames, max " << config["max_fps"] << " FPS";
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
                double fps = frames_saved / elapsed;
                double avg_size = frames_saved > 0 ? bytes_written / frames_saved : 0;

                std::stringstream log_ss;
                log_ss << "Saved " << frames_saved << "/" << frames_received << " frames, "
                       << "FPS: " << std::fixed << std::setprecision(1) << fps << ", "
                       << "Avg size: " << std::setprecision(1) << avg_size/1024.0 << " KB, "
                       << "Total: " << std::setprecision(2) << bytes_written/(1024.0*1024.0) << " MB";
                logger.info(log_ss.str());

                // Update Redis metrics
                redis.updateMetric("fps", fps);
                redis.updateMetric("avg_file_size_kb", avg_size/1024.0);
                redis.updateMetric("total_size_mb", bytes_written/(1024.0*1024.0));
                redis.updateMetric("last_update", std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count()));

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

        logger.info("Frame saver stopped");

        std::stringstream final_ss;
        final_ss << "Total saved: " << frames_saved << "/" << frames_received << " frames, "
                 << std::fixed << std::setprecision(2) << bytes_written/(1024.0*1024.0) << " MB";
        logger.info(final_ss.str());
    }
};

int main(int argc, char* argv[]) {
    Logger logger("frame-saver");
    logger.info("Juni's Frame Saver (C++ Implementation)");

    // Parse command line arguments
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--help") {
            std::cout << "Frame Saver Service (C++)" << std::endl;
            std::cout << "Environment variables:" << std::endl;
            std::cout << "  JVIDEO_OUTPUT_DIR      - Output directory (default: /var/lib/jvideo/frames)" << std::endl;
            std::cout << "  JVIDEO_FORMAT          - Image format: jpg, png, raw (default: jpg)" << std::endl;
            std::cout << "  JVIDEO_SUBSCRIBE_PORT  - Input port (default: 5556)" << std::endl;
            std::cout << "  JVIDEO_SAVE_EVERY_N    - Save every Nth frame (default: 1)" << std::endl;
            return 0;
        }
    }

    try {
        FrameSaver saver;
        saver.run();
    } catch (const std::exception& e) {
        logger.error("Fatal error: " + std::string(e.what()));
        return 1;
    }

    return 0;
}
