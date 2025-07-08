#include "pipeline_stage.h"
#include <opencv2/opencv.hpp>
#include <zmq.hpp>
#include <filesystem>
#include <deque>
#include <numeric>
#include "frame_tracking.h"

namespace fs = std::filesystem;

class FrameSaver : public SaverStage {
private:
    zmq::context_t zmq_context_;
    zmq::socket_t sub_socket_;

    uint64_t frames_dropped_ = 0;
    uint64_t io_errors_ = 0;
    double last_save_time_ms_ = 0.0;

    std::string output_dir_;
    std::string format_ = "jpg";

    // Tracking metrics
    std::deque<double> total_latencies_;
    std::deque<double> publish_latencies_;
    std::deque<double> resize_latencies_;
    std::deque<double> save_latencies_;
    double min_latency_ms_ = std::numeric_limits<double>::max();
    double max_latency_ms_ = 0.0;
    uint64_t tracked_frames_ = 0;

public:
    FrameSaver()
        : SaverStage("frame-saver", "/etc/jvideo/frame-saver.conf"),
          zmq_context_(1),
          sub_socket_(zmq_context_, ZMQ_SUB) {
    }

    ~FrameSaver() {
        sub_socket_.close();
        zmq_context_.close();
    }

protected:
    json getDefaultConfig() const override {
        auto config = SaverStage::getDefaultConfig();
        config["subscribe_port"] = 5556;
        config["subscribe_host"] = "localhost";
        config["output_dir"] = "/var/lib/jvideo/frames";
        config["format"] = "jpg";
        return config;
    }

    void onStart() override {
        // Configure socket
        sub_socket_.set(zmq::sockopt::rcvhwm, 100);
        sub_socket_.set(zmq::sockopt::subscribe, "");

        // Connect to resizer
        std::string sub_addr = "tcp://" + config_["subscribe_host"].get<std::string>() +
                               ":" + std::to_string(config_["subscribe_port"].get<int>());
        sub_socket_.connect(sub_addr);
        std::cout << "[Saver] Connected to resizer at " << sub_addr << std::endl;

        // Get config values
        output_dir_ = config_["output_dir"];
        format_ = config_["format"];

        // Create output directory
        try {
            fs::create_directories(output_dir_);
            std::cout << "[Saver] Output directory: " << output_dir_ << std::endl;
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to create output directory: " + std::string(e.what()));
        }

        // Update initial metrics
        auto& metrics = metrics_mgr_->metrics();
        metrics.output_dir = output_dir_;
        metrics.format = format_;
        metrics.disk_healthy = true;
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

        auto save_start = steady_clock::now();

        // Parse metadata
        std::string meta_str(static_cast<char*>(meta_msg.data()), meta_msg.size());
        json metadata = json::parse(meta_str);

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

        // Generate filename
        int frame_id = metadata.value("frame_id", static_cast<int>(frames_processed_));
        std::string filename = generateFilename(frame_id);

        // Save frame
        bool save_success = cv::imwrite(filename, frame);

        auto save_end = steady_clock::now();
        last_save_time_ms_ = duration<double, std::milli>(save_end - save_start).count();

        if (save_success) {
            frames_processed_++;

            // Complete tracking if available
            if (metadata.contains("tracking")) {
                processTracking(metadata, save_end);
            }

            // Update frame properties in metrics
            auto& metrics = metrics_mgr_->metrics();
            metrics.frame_width = frame.cols;
            metrics.frame_height = frame.rows;
            metrics.frame_channels = frame.channels();

            // Update metrics
            updateMetrics();

        } else {
            io_errors_++;
            auto& metrics = metrics_mgr_->metrics();
            metrics.io_errors = io_errors_;
            metrics.disk_healthy = false;
            metrics_mgr_->commit();
            std::cerr << "[Saver] Failed to save frame: " << filename << std::endl;
        }

        return true;
    }

    void updateServiceSpecificMetrics(SaverMetrics& metrics) override {
        metrics.frames_saved = frames_processed_;
        metrics.frames_dropped = frames_dropped_;
        metrics.io_errors = io_errors_;
        metrics.save_time_ms = last_save_time_ms_;
        metrics.disk_usage_mb = calculateDiskUsage();
        metrics.tracked_frames = tracked_frames_;

        // Update tracking averages if we have data
        if (!total_latencies_.empty()) {
            metrics.avg_total_latency_ms = std::accumulate(total_latencies_.begin(),
                                                          total_latencies_.end(), 0.0) / total_latencies_.size();
            metrics.min_total_latency_ms = min_latency_ms_;
            metrics.max_total_latency_ms = max_latency_ms_;
            metrics.avg_publish_latency_ms = std::accumulate(publish_latencies_.begin(),
                                                            publish_latencies_.end(), 0.0) / publish_latencies_.size();
            metrics.avg_resize_latency_ms = std::accumulate(resize_latencies_.begin(),
                                                           resize_latencies_.end(), 0.0) / resize_latencies_.size();
            metrics.avg_save_latency_ms = std::accumulate(save_latencies_.begin(),
                                                         save_latencies_.end(), 0.0) / save_latencies_.size();
        }
    }

private:
    std::string generateFilename(int frame_id) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");
        ss << "_" << std::setfill('0') << std::setw(3) << ms.count();
        ss << "_frame" << std::setfill('0') << std::setw(6) << frame_id;
        ss << "." << format_;

        return (fs::path(output_dir_) / ss.str()).string();
    }

    double calculateDiskUsage() {
        try {
            auto space_info = fs::space(output_dir_);
            return static_cast<double>(space_info.capacity - space_info.available) / (1024.0 * 1024.0);
        } catch (...) {
            return 0.0;
        }
    }

    void processTracking(const json& metadata, const steady_clock::time_point& save_end) {
        double save_timestamp = duration<double>(save_end.time_since_epoch()).count();

        auto& tracking = metadata["tracking"];
        if (tracking.contains("source_ts") && tracking.contains("publish_ts") && tracking.contains("resize_ts")) {
            double source_ts = tracking["source_ts"].get<double>();
            double publish_ts = tracking["publish_ts"].get<double>();
            double resize_ts = tracking["resize_ts"].get<double>();

            // Calculate latencies in milliseconds
            double total_latency_ms = (save_timestamp - source_ts) * 1000;
            double publish_latency_ms = (publish_ts - source_ts) * 1000;
            double resize_latency_ms = (resize_ts - publish_ts) * 1000;
            double save_latency_ms = (save_timestamp - resize_ts) * 1000;

            // Update tracking metrics
            updateTrackingMetrics(total_latency_ms, publish_latency_ms, resize_latency_ms, save_latency_ms);

            // Log complete tracking for sample frames
            uint64_t seq = tracking.value("sequence", 0ULL);
            if (seq % 100 == 0) {
                std::cout << "[Saver] Frame " << seq << " complete pipeline journey:\n";
                std::cout << "  - Read→Publish: " << std::fixed << std::setprecision(1)
                        << publish_latency_ms << "ms\n";
                std::cout << "  - Publish→Resize: " << resize_latency_ms << "ms\n";
                std::cout << "  - Resize→Save: " << save_latency_ms << "ms\n";
                std::cout << "  - TOTAL: " << total_latency_ms << "ms" << std::endl;
            }
        }
    }

    void updateTrackingMetrics(double total_ms, double publish_ms, double resize_ms, double save_ms) {
        // Keep last 1000 samples
        if (total_latencies_.size() >= 1000) {
            total_latencies_.pop_front();
            publish_latencies_.pop_front();
            resize_latencies_.pop_front();
            save_latencies_.pop_front();
        }

        total_latencies_.push_back(total_ms);
        publish_latencies_.push_back(publish_ms);
        resize_latencies_.push_back(resize_ms);
        save_latencies_.push_back(save_ms);

        min_latency_ms_ = std::min(min_latency_ms_, total_ms);
        max_latency_ms_ = std::max(max_latency_ms_, total_ms);
        tracked_frames_++;
    }
};

int main() {
    std::cout << "[Saver] Frame Saver starting..." << std::endl;
    std::cout << "[Saver] PID: " << getpid() << std::endl;

    try {
        FrameSaver saver;
        saver.run();
    } catch (const std::exception& e) {
        std::cerr << "[Saver] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Saver] Shutdown complete" << std::endl;
    return 0;
}
