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
        LOG_DEBUG(service_name_, "Shutting down FrameSaver");
        sub_socket_.close();
        zmq_context_.close();
        LOG_DEBUG(service_name_, "ZMQ resources cleaned up");
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
        LOG_INFO(service_name_, "Starting FrameSaver initialization");

        // Configure socket
        sub_socket_.set(zmq::sockopt::rcvhwm, 100);
        sub_socket_.set(zmq::sockopt::subscribe, "");

        // Connect to resizer
        std::string sub_addr = "tcp://" + config_["subscribe_host"].get<std::string>() +
                               ":" + std::to_string(config_["subscribe_port"].get<int>());
        try {
            sub_socket_.connect(sub_addr);
            LOG_INFO(service_name_, "Connected to resizer at " + sub_addr);
        } catch (const std::exception& e) {
            LOG_ERROR(service_name_, "Failed to connect to resizer at " + sub_addr + ": " + e.what());
            throw;
        }

        // Get config values
        output_dir_ = config_["output_dir"];
        format_ = config_["format"];

        LOG_INFO(service_name_, "Configuration loaded - Output directory: " + output_dir_ +
                 ", Format: " + format_);

        // Create output directory
        try {
            fs::create_directories(output_dir_);
            LOG_INFO(service_name_, "Output directory created/verified: " + output_dir_);
        } catch (const std::exception& e) {
            LOG_FATAL(service_name_, "Failed to create output directory: " + std::string(e.what()));
            throw std::runtime_error("Failed to create output directory: " + std::string(e.what()));
        }

        // Update initial metrics
        auto& metrics = metrics_mgr_->metrics();
        metrics.output_dir = output_dir_;
        metrics.format = format_;
        metrics.disk_healthy = true;
        metrics_mgr_->commit();

        LOG_DEBUG(service_name_, "Initial metrics updated");
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
            LOG_WARN(service_name_, "Received metadata without frame data, dropping frame");
            return true;
        }

        // Receive frame data
        if (!sub_socket_.recv(frame_msg, zmq::recv_flags::none)) {
            frames_dropped_++;
            LOG_WARN(service_name_, "Failed to receive frame data, dropping frame");
            return true;
        }

        auto save_start = steady_clock::now();

        // Parse metadata
        std::string meta_str(static_cast<char*>(meta_msg.data()), meta_msg.size());
        json metadata;
        try {
            metadata = json::parse(meta_str);
        } catch (const std::exception& e) {
            frames_dropped_++;
            LOG_ERROR(service_name_, "Failed to parse metadata JSON: " + std::string(e.what()));
            return true;
        }

        // Decode frame
        std::vector<uchar> buffer(
            static_cast<uchar*>(frame_msg.data()),
            static_cast<uchar*>(frame_msg.data()) + frame_msg.size()
        );

        cv::Mat frame = cv::imdecode(buffer, cv::IMREAD_COLOR);
        if (frame.empty()) {
            frames_dropped_++;
            LOG_ERROR(service_name_, "Failed to decode frame image");
            return true;
        }

        // Generate filename
        int frame_id = metadata.value("frame_id", static_cast<int>(frames_processed_));
        std::string filename = generateFilename(frame_id);

        LOG_TRACE(service_name_, "Saving frame " + std::to_string(frame_id) + " as " + filename +
                  " (size: " + std::to_string(frame.cols) + "x" + std::to_string(frame.rows) + ")");

        // Save frame
        bool save_success = false;
        try {
            save_success = cv::imwrite(filename, frame);
        } catch (const std::exception& e) {
            LOG_ERROR(service_name_, "Exception during frame save: " + std::string(e.what()));
            save_success = false;
        }

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

            LOG_TRACE(service_name_, "Frame saved successfully in " +
                      std::to_string(last_save_time_ms_) + "ms");

            // Update metrics
            updateMetrics();

        } else {
            io_errors_++;
            auto& metrics = metrics_mgr_->metrics();
            metrics.io_errors = io_errors_;
            metrics.disk_healthy = false;
            metrics_mgr_->commit();
            LOG_ERROR(service_name_, "Failed to save frame: " + filename);
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

        // Log metrics update for debugging
        if (frames_processed_ % 1000 == 0) {
            LOG_TRACE(service_name_, "Metrics updated - saved: " + std::to_string(frames_processed_) +
                      ", dropped: " + std::to_string(frames_dropped_) +
                      ", io_errors: " + std::to_string(io_errors_) +
                      ", save_time: " + std::to_string(last_save_time_ms_) + "ms");
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
            double usage_mb = static_cast<double>(space_info.capacity - space_info.available) / (1024.0 * 1024.0);
            LOG_TRACE(service_name_, "Disk usage calculated: " + std::to_string(usage_mb) + " MB");
            return usage_mb;
        } catch (const std::exception& e) {
            LOG_WARN(service_name_, "Failed to calculate disk usage: " + std::string(e.what()));
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
                LOG_INFO(service_name_, "Frame " + std::to_string(seq) + " pipeline journey complete:");
                LOG_INFO(service_name_, "  Read→Publish: " + std::to_string(publish_latency_ms) + "ms");
                LOG_INFO(service_name_, "  Publish→Resize: " + std::to_string(resize_latency_ms) + "ms");
                LOG_INFO(service_name_, "  Resize→Save: " + std::to_string(save_latency_ms) + "ms");
                LOG_INFO(service_name_, "  TOTAL: " + std::to_string(total_latency_ms) + "ms");
            }
        } else {
            LOG_WARN(service_name_, "Incomplete tracking data in metadata");
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

        LOG_TRACE(service_name_, "Tracking metrics updated - total: " + std::to_string(total_ms) +
                  "ms, tracked frames: " + std::to_string(tracked_frames_));
    }
};

int main() {
    LOG_INFO("frame-saver", "Frame Saver starting...");
    LOG_INFO("frame-saver", "PID: " + std::to_string(getpid()));

    try {
        FrameSaver saver;
        saver.run();
    } catch (const std::exception& e) {
        LOG_FATAL("frame-saver", "Fatal error: " + std::string(e.what()));
        return 1;
    }

    LOG_INFO("frame-saver", "Shutdown complete");
    return 0;
}
