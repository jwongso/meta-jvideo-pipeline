#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <signal.h>
#include <memory>
#include <hiredis/hiredis.h>

using json = nlohmann::json;
using namespace std::chrono;

// Global flag for graceful shutdown
volatile sig_atomic_t g_running = 1;

// Simple RAII Redis wrapper
class RedisClient {
private:
    std::unique_ptr<redisContext, decltype(&redisFree)> context;
    std::string service_name;

public:
    RedisClient(const std::string& name)
        : context(nullptr, redisFree), service_name(name) {

        struct timeval timeout = { 2, 0 };
        redisContext* ctx = redisConnectWithTimeout("127.0.0.1", 6379, timeout);

        if (ctx && !ctx->err) {
            context.reset(ctx);
            std::cout << "[Redis] Connected\n";
        } else {
            if (ctx) redisFree(ctx);
            std::cerr << "[Redis] Connection failed\n";
        }
    }

    void updateMetric(const std::string& key, int value) {
        if (!context) return;

        std::string metrics_key = "metrics:" + service_name;
        redisReply* reply = (redisReply*)redisCommand(context.get(),
            "HSET %s %s %d", metrics_key.c_str(), key.c_str(), value);

        if (reply) {
            freeReplyObject(reply);
        } else if (context->err) {
            std::cerr << "[Redis] Error updating metric: " << context->errstr << std::endl;
        }
    }

    void updateMetric(const std::string& key, double value) {
        if (!context) return;

        std::string metrics_key = "metrics:" + service_name;
        redisReply* reply = (redisReply*)redisCommand(context.get(),
            "HSET %s %s %.2f", metrics_key.c_str(), key.c_str(), value);

        if (reply) {
            freeReplyObject(reply);
        } else if (context->err) {
            std::cerr << "[Redis] Error updating metric: " << context->errstr << std::endl;
        }
    }
};

class MP4FramePublisher {
private:
    static constexpr const char* DEFAULT_VIDEO_PATH = "/opt/jvideo/input.mp4";
    static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/jvideo/frame-publisher.conf";

    zmq::context_t zmq_context;
    zmq::socket_t zmq_socket;
    cv::VideoCapture video_cap;
    RedisClient redis;
    json config;

    // Metrics
    int frames_published = 0;
    int errors = 0;
    steady_clock::time_point start_time;

    void loadConfig() {
        // Default configuration
        config = {
            {"video_input_path", DEFAULT_VIDEO_PATH},
            {"publish_port", 5555}
        };

        // Try to load from config file
        std::string config_path = DEFAULT_CONFIG_PATH;
        const char* env_path = std::getenv("FRAME_PUBLISHER_CONFIG");
        if (env_path) {
            config_path = env_path;
        }

        std::ifstream config_file(config_path);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;

                // Only merge the keys we care about
                if (user_config.contains("video_input_path")) {
                    config["video_input_path"] = user_config["video_input_path"];
                }
                if (user_config.contains("publish_port")) {
                    config["publish_port"] = user_config["publish_port"];
                }

                std::cout << "[Publisher] Loaded config from " << config_path << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Publisher] Failed to parse config: " << e.what()
                         << ", using defaults\n";
            }
        }
    }

public:
    MP4FramePublisher()
        : zmq_context(1),
          zmq_socket(zmq_context, ZMQ_PUB),
          redis("mp4-frame-publisher") {

        start_time = steady_clock::now();

        // Load configuration
        loadConfig();

        // Setup signal handlers
        signal(SIGINT, [](int sig) { g_running = 0; });
        signal(SIGTERM, [](int sig) { g_running = 0; });

        // Bind ZMQ socket with configured port
        int port = config["publish_port"];
        std::string bind_addr = "tcp://*:" + std::to_string(port);
        zmq_socket.bind(bind_addr);
        std::cout << "[Publisher] ZMQ socket bound to " << bind_addr << std::endl;

        // Give subscribers time to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    bool openVideo(const std::string& video_path = "") {
        // Determine video path: command line > config file > default
        std::string final_path;
        if (!video_path.empty()) {
            final_path = video_path;
            std::cout << "[Publisher] Using command line path: " << final_path << std::endl;
        } else {
            final_path = config["video_input_path"];
            std::cout << "[Publisher] Using configured path: " << final_path << std::endl;
        }

        // Debug: Check file accessibility
        std::ifstream test_file(final_path);
        if (!test_file.good()) {
            std::cerr << "[Publisher] File not accessible: " << final_path << std::endl;
            return false;
        }
        test_file.close();
        std::cout << "[Publisher] File is accessible" << std::endl;

        // Try different backends (compatible with older OpenCV versions)
        std::vector<std::pair<cv::VideoCaptureAPIs, std::string>> backends_to_try = {
            {cv::CAP_ANY, "CAP_ANY"},
            {cv::CAP_FFMPEG, "CAP_FFMPEG"},
            {cv::CAP_GSTREAMER, "CAP_GSTREAMER"},
            {cv::CAP_V4L2, "CAP_V4L2"}
        };

        for (auto& backend_pair : backends_to_try) {
            auto backend = backend_pair.first;
            auto backend_name = backend_pair.second;

            std::cout << "[Publisher] Trying backend: " << backend_name << std::endl;

            if (video_cap.open(final_path, backend)) {
                std::cout << "[Publisher] Successfully opened with backend: " << backend_name << std::endl;

                // Get video properties
                int frame_count = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_COUNT));
                double fps = video_cap.get(cv::CAP_PROP_FPS);
                int width = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_WIDTH));
                int height = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_HEIGHT));

                std::cout << "[Publisher] Video opened: " << width << "x" << height
                        << ", " << frame_count << " frames @ " << fps << " fps\n";

                // Store video info in Redis
                redis.updateMetric("total_frames", frame_count);
                redis.updateMetric("video_fps", fps);
                redis.updateMetric("video_width", width);
                redis.updateMetric("video_height", height);

                return true;
            } else {
                std::cout << "[Publisher] Failed with backend: " << backend_name << std::endl;
            }
        }

        std::cerr << "[Publisher] Failed to open video with any backend: " << final_path << std::endl;
        return false;
    }

    void publishFrames() {
        if (!video_cap.isOpened()) {
            std::cerr << "[Publisher] Video not opened\n";
            return;
        }

        cv::Mat frame;
        int frame_id = 0;
        auto last_metric_update = steady_clock::now();

        std::cout << "[Publisher] Starting frame publishing...\n";

        while (g_running && video_cap.read(frame)) {
            if (frame.empty()) {
                std::cout << "[Publisher] Empty frame, skipping\n";
                continue;
            }

            try {
                // Create metadata
                json metadata;
                metadata["frame_id"] = frame_id;
                metadata["timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();
                metadata["width"] = frame.cols;
                metadata["height"] = frame.rows;
                metadata["channels"] = frame.channels();
                metadata["source"] = "mp4";

                // Send metadata first
                std::string meta_str = metadata.dump();
                zmq::message_t meta_msg(meta_str.data(), meta_str.size());
                zmq_socket.send(meta_msg, zmq::send_flags::sndmore);

                // Send frame data
                size_t frame_size = frame.total() * frame.elemSize();
                zmq::message_t frame_msg(frame.data, frame_size);
                zmq_socket.send(frame_msg, zmq::send_flags::none);

                frames_published++;
                frame_id++;

                // Update metrics every 100 frames
                if (frames_published % 100 == 0) {
                    auto now = steady_clock::now();
                    double elapsed = duration<double>(now - start_time).count();
                    double fps = frames_published / elapsed;

                    redis.updateMetric("frames_published", frames_published);
                    redis.updateMetric("current_fps", fps);
                    redis.updateMetric("errors", errors);

                    std::cout << "[Publisher] Published " << frames_published
                              << " frames, FPS: " << std::fixed << std::setprecision(1) << fps << "\n";
                }

                // Small delay to prevent overwhelming subscribers
                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps

            } catch (const zmq::error_t& e) {
                std::cerr << "[Publisher] ZMQ error: " << e.what() << std::endl;
                errors++;
            } catch (const std::exception& e) {
                std::cerr << "[Publisher] Error: " << e.what() << std::endl;
                errors++;
            }
        }

        // Final metrics update
        auto total_time = duration<double>(steady_clock::now() - start_time).count();
        redis.updateMetric("frames_published", frames_published);
        redis.updateMetric("total_time", total_time);
        redis.updateMetric("errors", errors);

        std::cout << "[Publisher] Finished. Published " << frames_published
                  << " frames in " << std::fixed << std::setprecision(1) << total_time << " seconds\n";
    }

    ~MP4FramePublisher() {
        if (video_cap.isOpened()) {
            video_cap.release();
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << "[Publisher] MP4 Frame Publisher starting...\n";

    try {
        MP4FramePublisher publisher;

        // Use command line argument for video path if provided
        std::string video_path = (argc > 1) ? argv[1] : "";

        if (!publisher.openVideo(video_path)) {
            std::cerr << "[Publisher] Failed to open video file\n";
            return 1;
        }

        publisher.publishFrames();

    } catch (const std::exception& e) {
        std::cerr << "[Publisher] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Publisher] Shutdown complete\n";
    return 0;
}
