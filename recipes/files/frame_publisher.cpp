#include "pipeline_stage.h"
#include <opencv2/opencv.hpp>
#include <zmq.hpp>
#include "frame_tracking.h"

class MP4FramePublisher : public PublisherStage {
private:
    static constexpr const char* DEFAULT_VIDEO_PATH = "/usr/share/jvideo/media/matterhorn.mp4";

    zmq::context_t zmq_context_;
    zmq::socket_t zmq_socket_;
    cv::VideoCapture video_cap_;

    std::atomic<uint64_t> frame_counter_{0};
    bool loop_video_ = false;
    int jpeg_quality_ = 85;
    int frame_delay_ms_ = 0;

public:
    MP4FramePublisher()
        : PublisherStage("frame-publisher", "/etc/jvideo/frame-publisher.conf"),
          zmq_context_(1),
          zmq_socket_(zmq_context_, ZMQ_PUB) {
    }

    ~MP4FramePublisher() {
        if (video_cap_.isOpened()) {
            video_cap_.release();
        }
        zmq_socket_.close();
        zmq_context_.close();
    }

protected:
    json getDefaultConfig() const override {
        auto config = PublisherStage::getDefaultConfig();
        config["video_input_path"] = DEFAULT_VIDEO_PATH;
        config["publish_port"] = 5555;
        config["jpeg_quality"] = 85;
        config["frame_delay_ms"] = 0;
        config["loop_video"] = false;
        return config;
    }

    void onStart() override {
        // Setup ZMQ
        zmq_socket_.set(zmq::sockopt::sndhwm, 100);
        zmq_socket_.set(zmq::sockopt::linger, 1000);

        int port = config_["publish_port"];
        std::string bind_addr = "tcp://*:" + std::to_string(port);
        zmq_socket_.bind(bind_addr);
        std::cout << "[Publisher] ZMQ socket bound to " << bind_addr << std::endl;

        // Give subscribers time to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Open video
        std::string video_path = config_["video_input_path"];
        if (!openVideo(video_path)) {
            throw std::runtime_error("Failed to open video: " + video_path);
        }

        // Cache config values
        loop_video_ = config_["loop_video"];
        jpeg_quality_ = config_["jpeg_quality"];
        frame_delay_ms_ = config_["frame_delay_ms"];
    }

    bool processFrame() override {
        cv::Mat frame;

        // Track when we start reading
        auto read_start = steady_clock::now();
        double read_timestamp = duration<double>(read_start.time_since_epoch()).count();

        // Read frame
        if (!video_cap_.read(frame) || frame.empty()) {
            if (loop_video_) {
                std::cout << "[Publisher] End of video, restarting..." << std::endl;
                video_cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
                frame_counter_ = 0;
                return true; // Continue processing
            } else {
                std::cout << "[Publisher] End of video reached. Exiting..." << std::endl;
                stop();
                return false;
            }
        }

        // Create tracking info
        FrameTrackingInfo tracking;
        tracking.frame_id = std::chrono::steady_clock::now().time_since_epoch().count();
        tracking.sequence_number = frame_counter_++;
        tracking.source_timestamp = read_timestamp;

        // Encode frame
        std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality_};
        std::vector<uchar> buffer;
        cv::imencode(".jpg", frame, buffer, jpeg_params);

        // Get publish timestamp
        double publish_timestamp = duration<double>(steady_clock::now().time_since_epoch()).count();
        tracking.publish_timestamp = publish_timestamp;

        // Create metadata
        json metadata = {
            {"frame_id", frames_processed_},
            {"timestamp", publish_timestamp},
            {"width", frame.cols},
            {"height", frame.rows},
            {"channels", frame.channels()},
            {"source", "mp4"},
            {"tracking", {
                {"frame_id", tracking.frame_id},
                {"sequence", tracking.sequence_number},
                {"source_ts", tracking.source_timestamp},
                {"publish_ts", tracking.publish_timestamp}
            }}
        };

        // Send data
        std::string meta_str = metadata.dump();
        zmq::message_t meta_msg(meta_str.data(), meta_str.size());
        zmq_socket_.send(meta_msg, zmq::send_flags::sndmore);

        zmq::message_t frame_msg(buffer.data(), buffer.size());
        zmq_socket_.send(frame_msg, zmq::send_flags::none);

        frames_processed_++;

        // Update metrics
        updateMetrics();

        // Frame delay if configured
        if (frame_delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(frame_delay_ms_));
        }

        return true;
    }

    void updateServiceSpecificMetrics(PublisherMetrics& metrics) override {
        metrics.frames_published = frames_processed_;
        // Video properties are set in openVideo()
    }

private:
    bool openVideo(const std::string& video_path) {
        std::cout << "[Publisher] Opening video: " << video_path << std::endl;

        if (!video_cap_.open(video_path)) {
            std::cerr << "[Publisher] Failed to open video: " << video_path << std::endl;
            return false;
        }

        // Get video properties
        int frame_count = static_cast<int>(video_cap_.get(cv::CAP_PROP_FRAME_COUNT));
        double fps = video_cap_.get(cv::CAP_PROP_FPS);
        int width = static_cast<int>(video_cap_.get(cv::CAP_PROP_FRAME_WIDTH));
        int height = static_cast<int>(video_cap_.get(cv::CAP_PROP_FRAME_HEIGHT));

        if (frame_count < 0 || frame_count > 1000000) {
            frame_count = 0;
        }

        std::cout << "[Publisher] Video opened successfully:" << std::endl;
        std::cout << "  Resolution: " << width << "x" << height << std::endl;
        std::cout << "  Frames: " << (frame_count > 0 ? std::to_string(frame_count) : "unknown") << std::endl;
        std::cout << "  FPS: " << fps << std::endl;

        // Update metrics
        auto& metrics = metrics_mgr_->metrics();
        metrics.total_frames = frame_count;
        metrics.video_fps = fps;
        metrics.video_width = width;
        metrics.video_height = height;
        metrics.video_path = video_path;
        metrics.video_healthy = true;
        metrics_mgr_->commit();

        return true;
    }
};

int main(int argc, char* argv[]) {
    std::cout << "[Publisher] MP4 Frame Publisher starting..." << std::endl;
    std::cout << "[Publisher] PID: " << getpid() << std::endl;

    try {
        MP4FramePublisher publisher;
        publisher.run();
    } catch (const std::exception& e) {
        std::cerr << "[Publisher] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Publisher] Shutdown complete" << std::endl;
    return 0;
}
