#include "pipeline_stage.h"
#include <opencv2/opencv.hpp>
#include <zmq.hpp>
#include "frame_tracking.h"

class FrameResizer : public ResizerStage {
private:
    zmq::context_t zmq_context_;
    zmq::socket_t sub_socket_;
    zmq::socket_t pub_socket_;

    uint64_t frames_dropped_ = 0;
    int output_width_ = 160;
    int output_height_ = 120;
    int jpeg_quality_ = 85;
    double last_processing_time_ms_ = 0.0;

public:
    FrameResizer()
        : ResizerStage("frame-resizer", "/etc/jvideo/frame-resizer.conf"),
          zmq_context_(1),
          sub_socket_(zmq_context_, ZMQ_SUB),
          pub_socket_(zmq_context_, ZMQ_PUB) {
    }

    ~FrameResizer() {
        sub_socket_.close();
        pub_socket_.close();
        zmq_context_.close();
    }

protected:
    json getDefaultConfig() const override {
        auto config = ResizerStage::getDefaultConfig();
        config["subscribe_port"] = 5555;
        config["publish_port"] = 5556;
        config["output_width"] = 160;
        config["output_height"] = 120;
        config["jpeg_quality"] = 85;
        return config;
    }

    void onStart() override {
        // Configure sockets
        sub_socket_.set(zmq::sockopt::rcvhwm, 100);
        sub_socket_.set(zmq::sockopt::subscribe, "");
        pub_socket_.set(zmq::sockopt::sndhwm, 100);
        pub_socket_.set(zmq::sockopt::linger, 1000);

        // Connect subscriber
        std::string sub_addr = "tcp://localhost:" + std::to_string(config_["subscribe_port"].get<int>());
        sub_socket_.connect(sub_addr);
        std::cout << "[Resizer] Connected to publisher at " << sub_addr << std::endl;

        // Bind publisher
        std::string pub_addr = "tcp://*:" + std::to_string(config_["publish_port"].get<int>());
        pub_socket_.bind(pub_addr);
        std::cout << "[Resizer] Publishing on " << pub_addr << std::endl;

        // Cache config values
        output_width_ = config_["output_width"];
        output_height_ = config_["output_height"];
        jpeg_quality_ = config_["jpeg_quality"];

        // Update initial metrics
        auto& metrics = metrics_mgr_->metrics();
        metrics.output_width = output_width_;
        metrics.output_height = output_height_;
        metrics.service_healthy = true;
        metrics_mgr_->commit();
    }

    bool processFrame() override {
        zmq::message_t meta_msg, frame_msg;

        // Try to receive metadata (non-blocking)
        if (!sub_socket_.recv(meta_msg, zmq::recv_flags::dontwait)) {
            return false; // No frame available
        }

        // Check if there's more (frame data)
        if (!meta_msg.more()) {
            frames_dropped_++;
            return true;
        }

        // Receive frame data
        if (!sub_socket_.recv(frame_msg, zmq::recv_flags::none)) {
            frames_dropped_++;
            return true;
        }

        auto proc_start = steady_clock::now();

        // Parse metadata
        std::string meta_str(static_cast<char*>(meta_msg.data()), meta_msg.size());
        json metadata = json::parse(meta_str);

        // Update tracking timestamp
        double resize_time = duration<double>(proc_start.time_since_epoch()).count();
        if (metadata.contains("tracking")) {
            auto& tracking = metadata["tracking"];

            // These are in seconds (as float/double)
            double source_ts = tracking["source_ts"];
            double publish_ts = tracking["publish_ts"];

            // Current time in seconds
            double current_time = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            // Calculate latencies in milliseconds
            double publish_latency_ms = (publish_ts - source_ts) * 1000.0;
            double resize_latency_ms = (current_time - publish_ts) * 1000.0;

            // Add current timestamp for next service
            tracking["resize_ts"] = current_time;  // In seconds
        }

        // Decode frame
        std::vector<uchar> buffer(
            static_cast<uchar*>(frame_msg.data()),
            static_cast<uchar*>(frame_msg.data()) + frame_msg.size()
        );

        cv::Mat frame = cv::imdecode(buffer, cv::IMREAD_COLOR);
        if (frame.empty()) {
            frames_dropped_++;
            return true;
        }

        // Update input dimensions in metrics
        auto& metrics = metrics_mgr_->metrics();
        metrics.input_width = frame.cols;
        metrics.input_height = frame.rows;

        // Resize frame
        cv::Mat resized;
        cv::Size output_size(output_width_, output_height_);
        cv::resize(frame, resized, output_size, 0, 0, cv::INTER_LINEAR);

        // Encode resized frame
        std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality_};
        std::vector<uchar> encoded;
        cv::imencode(".jpg", resized, encoded, jpeg_params);

        // Calculate processing time
        auto proc_end = steady_clock::now();
        last_processing_time_ms_ = duration<double, std::milli>(proc_end - proc_start).count();

        // Update metadata with resize info
        metadata["resized_width"] = resized.cols;
        metadata["resized_height"] = resized.rows;
        metadata["resizer_timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();
        metadata["resize_time_ms"] = last_processing_time_ms_;

        // Send resized frame (metadata + data)
        std::string new_meta_str = metadata.dump();
        zmq::message_t new_meta_msg(new_meta_str.data(), new_meta_str.size());
        pub_socket_.send(new_meta_msg, zmq::send_flags::sndmore);

        zmq::message_t new_frame_msg(encoded.data(), encoded.size());
        pub_socket_.send(new_frame_msg, zmq::send_flags::none);

        frames_processed_++;

        // Update metrics
        updateMetrics();

        return true;
    }

    void updateServiceSpecificMetrics(ResizerMetrics& metrics) override {
        metrics.frames_processed = frames_processed_;
        metrics.frames_dropped = frames_dropped_;
        metrics.processing_time_ms = last_processing_time_ms_;
        // Input/output dimensions are updated in processFrame()
    }
};

int main() {
    std::cout << "[Resizer] Frame Resizer starting..." << std::endl;
    std::cout << "[Resizer] PID: " << getpid() << std::endl;

    try {
        FrameResizer resizer;
        resizer.run();
    } catch (const std::exception& e) {
        std::cerr << "[Resizer] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Resizer] Shutdown complete" << std::endl;
    return 0;
}
