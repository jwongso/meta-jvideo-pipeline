#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <signal.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <unordered_map>

class FrameResizer {
    zmq::context_t context{1};
    zmq::socket_t sub_socket;
    zmq::socket_t pub_socket;
    std::atomic<bool> running{true};

    struct Config {
        int subscribe_port{5555};
        int publish_port{5556};
        int output_width{160};
        int output_height{120};
        int interpolation{cv::INTER_LINEAR};
    } config;

    void loadConfig() {
        try {
            std::ifstream config_file("/etc/jvideo/frame-resizer.conf");
            if (config_file) {
                nlohmann::json json_config;
                config_file >> json_config;

                // Validate and apply config
                if (json_config.contains("output_width"))
                    config.output_width = std::max(1, json_config["output_width"].get<int>());
                if (json_config.contains("output_height"))
                    config.output_height = std::max(1, json_config["output_height"].get<int>());
                if (json_config.contains("subscribe_port"))
                    config.subscribe_port = json_config["subscribe_port"].get<int>();
                if (json_config.contains("publish_port"))
                    config.publish_port = json_config["publish_port"].get<int>();

                // Interpolation mapping
                static const std::unordered_map<std::string, int> interpolation_methods = {
                    {"nearest", cv::INTER_NEAREST},
                    {"linear", cv::INTER_LINEAR},
                    {"cubic", cv::INTER_CUBIC},
                    {"area", cv::INTER_AREA},
                    {"lanczos", cv::INTER_LANCZOS4}
                };
                if (json_config.contains("interpolation")) {
                    auto it = interpolation_methods.find(json_config["interpolation"].get<std::string>());
                    if (it != interpolation_methods.end()) {
                        config.interpolation = it->second;
                    }
                }
            }
        } catch (...) {
            std::cerr << "Warning: Invalid config, using defaults\n";
        }
    }

    void setupSockets() {
        sub_socket = zmq::socket_t(context, ZMQ_SUB);
        pub_socket = zmq::socket_t(context, ZMQ_PUB);

        sub_socket.connect("tcp://localhost:" + std::to_string(config.subscribe_port));
        sub_socket.set(zmq::sockopt::subscribe, "");

        pub_socket.bind("tcp://*:" + std::to_string(config.publish_port));
    }

    bool processFrame() {
        zmq::message_t metadata_msg, frame_msg;

        // Non-blocking receive with timeout
        if (!sub_socket.recv(metadata_msg, zmq::recv_flags::dontwait))
            return false;
        if (!sub_socket.recv(frame_msg))
            return false;

        try {
            // Parse metadata
            auto metadata = nlohmann::json::parse(metadata_msg.to_string());

            // Validate frame dimensions
            const int width = metadata["width"];
            const int height = metadata["height"];
            const int channels = metadata["channels"];
            if (width <= 0 || height <= 0 || (channels != 1 && channels != 3)) {
                throw std::runtime_error("Invalid frame metadata");
            }

            // Wrap frame data in OpenCV Mat (zero-copy)
            cv::Mat frame(height, width, channels == 3 ? CV_8UC3 : CV_8UC1, frame_msg.data());

            // Resize
            cv::Mat resized;
            cv::resize(frame, resized,
                       cv::Size(config.output_width, config.output_height),
                       0, 0, config.interpolation);

            // Update metadata
            metadata["width"] = resized.cols;
            metadata["height"] = resized.rows;

            // Send metadata
            std::string meta_str = metadata.dump();
            zmq::message_t new_meta_msg(meta_str.begin(), meta_str.end());
            pub_socket.send(new_meta_msg, zmq::send_flags::sndmore);

            // Send frame (zero-copy)
            zmq::message_t new_frame_msg(resized.data, resized.total() * resized.elemSize());
            pub_socket.send(new_frame_msg, zmq::send_flags::none);

            return true;
        } catch (...) {
            return false;  // Silently drop corrupt frames
        }
    }

public:
    FrameResizer() : sub_socket(context, ZMQ_SUB), pub_socket(context, ZMQ_PUB) {
        loadConfig();
        setupSockets();
    }

    void run() {
        while (running) {
            if (!processFrame()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void stop() { running = false; }
};

// Signal handler
std::atomic<FrameResizer*> g_resizer{nullptr};
void signalHandler(int) {
    if (g_resizer) g_resizer.load()->stop();
}

int main() {
    FrameResizer resizer;
    g_resizer = &resizer;

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    resizer.run();
    return 0;
}
