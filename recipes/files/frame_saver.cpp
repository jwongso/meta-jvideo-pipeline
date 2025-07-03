#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

class FrameSaver {
private:
    zmq::context_t context;
    zmq::socket_t sub_socket;
    json config;

public:
    FrameSaver() : context(1), sub_socket(context, ZMQ_SUB) {
        loadConfig();
        setupSubscriber();
        ensureOutputDirectory();
    }

    ~FrameSaver() {
        sub_socket.close();
        context.close();
    }

    void loadConfig() {
        config = {
            {"subscribe_port", 5556},
            {"subscribe_host", "localhost"},
            {"output_dir", "/var/lib/jvideo/frames"},
            {"format", "jpg"}
        };

        std::ifstream config_file("/etc/jvideo/frame-saver.conf");
        if (config_file.is_open()) {
            try {
                config.merge_patch(json::parse(config_file));
            } catch (...) {}
        }
    }

    void setupSubscriber() {
        sub_socket.connect("tcp://" + config["subscribe_host"].get<std::string>() +
                          ":" + std::to_string(config["subscribe_port"].get<int>()));
        sub_socket.set(zmq::sockopt::subscribe, "");
    }

    void ensureOutputDirectory() {
        fs::create_directories(config["output_dir"].get<std::string>());
    }

    std::string generateFilename() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S")
           << "." << config["format"].get<std::string>();
        return (fs::path(config["output_dir"].get<std::string>()) / ss.str()).string();
    }

    void run() {
        while (true) {
            zmq::message_t metadata_msg, frame_msg;
            if (!sub_socket.recv(metadata_msg, zmq::recv_flags::dontwait)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            sub_socket.recv(frame_msg);
            json metadata = json::parse(metadata_msg.to_string());
            cv::Mat frame(
                metadata["height"].get<int>(),
                metadata["width"].get<int>(),
                metadata["channels"].get<int>() == 3 ? CV_8UC3 : CV_8UC1,
                frame_msg.data()
            );
            cv::imwrite(generateFilename(), frame);
        }
    }
};

int main() {
    FrameSaver saver;
    saver.run();
    return 0;
}
