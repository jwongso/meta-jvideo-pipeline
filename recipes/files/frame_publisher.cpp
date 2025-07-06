#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <signal.h>
#include <memory>
#include <iomanip>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sqlite3.h>
#include <mutex>
#include <errno.h>
#include <sys/types.h>
#include <ctime>
#include <atomic>

using json = nlohmann::json;
using namespace std::chrono;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

// Forward declaration for global pointer
class MP4FramePublisher;
std::unique_ptr<MP4FramePublisher> g_publisher;

// Shared metrics structure - NO atomics in shared memory!
struct PublisherSharedMetrics {
    char service_name[64];
    pid_t service_pid;

    // Regular integers for shared memory
    uint64_t frames_published;
    uint64_t total_frames;
    uint64_t errors;

    double current_fps;
    double video_fps;

    int video_width;
    int video_height;

    bool video_healthy;
    char video_path[256];

    time_t last_update_time;
    time_t service_start_time;

    // Padding for alignment
    char _padding[64];
};

void createDirectoryPath(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string subdir = path.substr(0, pos);
        if (!subdir.empty()) {
            mkdir(subdir.c_str(), 0755);
        }
    }
}

class PublisherSharedMemoryManager {
    int shm_fd = -1;
    PublisherSharedMetrics* metrics = nullptr;
    bool is_owner = false;
    std::mutex update_mutex;  // Protect updates

public:
    explicit PublisherSharedMemoryManager(const std::string& service_name) {
        const char* shm_name = "/jvideo_publisher_metrics";

        // Try to create/open shared memory
        shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            throw std::runtime_error("shm_open failed: " + std::string(strerror(errno)));
        }

        // Set size
        if (ftruncate(shm_fd, sizeof(PublisherSharedMetrics)) == -1) {
            close(shm_fd);
            throw std::runtime_error("ftruncate failed: " + std::string(strerror(errno)));
        }

        // Map memory
        metrics = static_cast<PublisherSharedMetrics*>(
            mmap(nullptr, sizeof(PublisherSharedMetrics),
                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
        );

        if (metrics == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error("mmap failed: " + std::string(strerror(errno)));
        }

        // Initialize
        std::lock_guard<std::mutex> lock(update_mutex);
        memset(metrics, 0, sizeof(PublisherSharedMetrics));
        strncpy(metrics->service_name, service_name.c_str(), 63);
        metrics->service_pid = getpid();
        metrics->service_start_time = time(nullptr);
        metrics->video_healthy = true;

        is_owner = true;
    }

    // Template for all integer types
    template<typename T>
    typename std::enable_if<std::is_integral<T>::value>::type
    updateMetric(const std::string& key, T value) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);

        if (key == "frames_published") metrics->frames_published = static_cast<uint64_t>(value);
        else if (key == "total_frames") metrics->total_frames = static_cast<uint64_t>(value);
        else if (key == "errors") metrics->errors = static_cast<uint64_t>(value);
        else if (key == "video_width") metrics->video_width = static_cast<int>(value);
        else if (key == "video_height") metrics->video_height = static_cast<int>(value);

        metrics->last_update_time = time(nullptr);
    }

    // Overload for double
    void updateMetric(const std::string& key, double value) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);

        if (key == "current_fps") metrics->current_fps = value;
        else if (key == "video_fps") metrics->video_fps = value;

        metrics->last_update_time = time(nullptr);
    }

    void updateVideoPath(const std::string& path) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);
        strncpy(metrics->video_path, path.c_str(), 255);
        metrics->video_path[255] = '\0';
    }

    PublisherSharedMetrics getMetricsSnapshot() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(update_mutex));
        return *metrics;
    }

    ~PublisherSharedMemoryManager() {
        if (metrics && metrics != MAP_FAILED) {
            munmap(metrics, sizeof(PublisherSharedMetrics));
        }
        if (shm_fd != -1) {
            close(shm_fd);
        }
        if (is_owner) {
            shm_unlink("/jvideo_publisher_metrics");
        }
    }
};

class PublisherBenchmarkDatabase {
    sqlite3* db = nullptr;
    std::mutex db_mutex;
    std::atomic<bool> export_in_progress{false};
    std::thread export_thread;

    void initializeSchema() {
        const char* sql = R"(
            CREATE TABLE IF NOT EXISTS publisher_benchmarks (
                timestamp INTEGER PRIMARY KEY,
                frames_published INTEGER,
                total_frames INTEGER,
                current_fps REAL,
                video_fps REAL,
                errors INTEGER,
                uptime_seconds INTEGER,
                memory_usage_kb INTEGER
            );
            CREATE INDEX IF NOT EXISTS idx_timestamp ON publisher_benchmarks(timestamp);
        )";

        char* err_msg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::string error = err_msg ? err_msg : "Unknown error";
            sqlite3_free(err_msg);
            throw std::runtime_error("Schema initialization failed: " + error);
        }
    }

    long getMemoryUsage() const {
        std::ifstream status_file("/proc/self/status");
        std::string line;

        while (std::getline(status_file, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                std::istringstream iss(line);
                std::string label, value, unit;
                iss >> label >> value >> unit;
                return std::stol(value);
            }
        }
        return -1;
    }

public:
    PublisherBenchmarkDatabase() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            throw std::runtime_error("Failed to open in-memory database");
        }

        // Configure for performance
        sqlite3_exec(db, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", nullptr, nullptr, nullptr);

        initializeSchema();
    }

    void storeBenchmark(const PublisherSharedMetrics& metrics) {
        std::lock_guard<std::mutex> lock(db_mutex);

        const char* sql = R"(
            INSERT INTO publisher_benchmarks
            (timestamp, frames_published, total_frames, current_fps, video_fps, errors, uptime_seconds, memory_usage_kb)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return;
        }

        time_t current_time = time(nullptr);
        int uptime = static_cast<int>(current_time - metrics.service_start_time);

        sqlite3_bind_int64(stmt, 1, current_time);
        sqlite3_bind_int64(stmt, 2, metrics.frames_published);
        sqlite3_bind_int64(stmt, 3, metrics.total_frames);
        sqlite3_bind_double(stmt, 4, metrics.current_fps);
        sqlite3_bind_double(stmt, 5, metrics.video_fps);
        sqlite3_bind_int64(stmt, 6, metrics.errors);
        sqlite3_bind_int(stmt, 7, uptime);
        sqlite3_bind_int64(stmt, 8, getMemoryUsage());

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void asyncExport(const std::string& path) {
        if (export_in_progress.load()) return;

        export_in_progress = true;

        if (export_thread.joinable()) {
            export_thread.join();
        }

        export_thread = std::thread([this, path]() {
            try {
                // Create directory path if needed
                size_t pos = path.find_last_of("/");
                if (pos != std::string::npos) {
                    std::string dir = path.substr(0, pos);
                    createDirectoryPath(dir);
                }

                std::lock_guard<std::mutex> lock(db_mutex);

                sqlite3* file_db;
                if (sqlite3_open(path.c_str(), &file_db) != SQLITE_OK) {
                    std::cerr << "[Service] Failed to open DB: " << path << std::endl;
                    export_in_progress = false;
                    return;
                }

                // Initialize the schema in the file database too
                const char* sql = nullptr;

                // Use appropriate schema based on service type
                if (path.find("publisher") != std::string::npos) {
                    sql = R"(
                        CREATE TABLE IF NOT EXISTS publisher_benchmarks (
                            timestamp INTEGER PRIMARY KEY,
                            frames_published INTEGER,
                            total_frames INTEGER,
                            current_fps REAL,
                            video_fps REAL,
                            errors INTEGER,
                            uptime_seconds INTEGER,
                            memory_usage_kb INTEGER
                        );
                    )";
                } else if (path.find("resizer") != std::string::npos) {
                    sql = R"(
                        CREATE TABLE IF NOT EXISTS resizer_benchmarks (
                            timestamp INTEGER PRIMARY KEY,
                            frames_processed INTEGER,
                            frames_dropped INTEGER,
                            current_fps REAL,
                            processing_time_ms REAL,
                            memory_usage_kb INTEGER,
                            input_width INTEGER,
                            input_height INTEGER,
                            output_width INTEGER,
                            output_height INTEGER,
                            errors INTEGER
                        );
                    )";
                } else if (path.find("saver") != std::string::npos) {
                    sql = R"(
                        CREATE TABLE IF NOT EXISTS saver_benchmarks (
                            timestamp INTEGER PRIMARY KEY,
                            frames_saved INTEGER,
                            current_fps REAL,
                            save_time_ms REAL,
                            disk_usage_mb REAL,
                            frame_width INTEGER,
                            frame_height INTEGER,
                            frame_channels INTEGER,
                            io_errors INTEGER
                        );
                    )";
                }

                if (sql) {
                    char* err_msg = nullptr;
                    sqlite3_exec(file_db, sql, nullptr, nullptr, &err_msg);
                    if (err_msg) sqlite3_free(err_msg);
                }

                sqlite3_backup* backup = sqlite3_backup_init(file_db, "main", db, "main");
                if (backup) {
                    sqlite3_backup_step(backup, -1);
                    sqlite3_backup_finish(backup);
                    std::cout << "[Service] Exported benchmarks to " << path << std::endl;
                } else {
                    std::cerr << "[Service] Backup init failed for " << path << std::endl;
                }
                sqlite3_close(file_db);

            } catch (const std::exception& e) {
                std::cerr << "[Service] Export failed: " << e.what() << std::endl;
            }
            export_in_progress = false;
        });
    }

    ~PublisherBenchmarkDatabase() {
        if (export_thread.joinable()) {
            export_thread.join();
        }
        if (db) {
            sqlite3_close(db);
        }
    }
};

class MP4FramePublisher {
private:
    static constexpr const char* DEFAULT_VIDEO_PATH = "/usr/share/jvideo/media/matterhorn.mp4";
    static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/jvideo/frame-publisher.conf";

    zmq::context_t zmq_context;
    zmq::socket_t zmq_socket;
    cv::VideoCapture video_cap;

    std::unique_ptr<PublisherSharedMemoryManager> shm;
    std::unique_ptr<PublisherBenchmarkDatabase> db;

    json config;
    std::atomic<bool> running{true};

    uint64_t frames_published = 0;
    uint64_t errors = 0;
    steady_clock::time_point start_time;
    steady_clock::time_point last_fps_update;

    void loadConfig() {
        // Default configuration
        config = {
            {"video_input_path", DEFAULT_VIDEO_PATH},
            {"publish_port", 5555},
            {"benchmark_db_path", "/var/lib/jvideo/db/publisher_benchmarks.db"},
            {"benchmark_export_interval", 10},  // Every 10 frames
            {"frame_delay_ms", 0},
            {"jpeg_quality", 85},
            {"loop_video", false}
        };

        // Try to load config file
        std::ifstream config_file(DEFAULT_CONFIG_PATH);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;

                config["video_input_path"] = user_config.value("video_input_path", DEFAULT_VIDEO_PATH);
                config["publish_port"] = user_config.value("publish_port", 5555);
                config["benchmark_db_path"] = user_config.value("benchmark_db_path", "/var/lib/jvideo/db/publisher_benchmarks.db");
                config["benchmark_export_interval"] = user_config.value("benchmark_export_interval", 10);
                config["frame_delay_ms"] = user_config.value("frame_delay_ms", 0);
                config["jpeg_quality"] = user_config.value("jpeg_quality", 85);
                config["loop_video"] = user_config.value("loop_video", false);

                std::cout << "[Publisher] Loaded config from " << DEFAULT_CONFIG_PATH << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Publisher] Config parse error: " << e.what() << ", using defaults" << std::endl;
            }
        } else {
            std::cout << "[Publisher] Config file not found at " << DEFAULT_CONFIG_PATH << ", using defaults" << std::endl;
        }
    }

public:
    MP4FramePublisher() : zmq_context(1), zmq_socket(zmq_context, ZMQ_PUB) {
        start_time = steady_clock::now();
        last_fps_update = start_time;

        // Initialize components
        shm = std::make_unique<PublisherSharedMemoryManager>("frame-publisher");
        db = std::make_unique<PublisherBenchmarkDatabase>();

        loadConfig();

        // Force initial database creation
        std::string db_path = config["benchmark_db_path"].get<std::string>();
        size_t pos = db_path.find_last_of("/");
        if (pos != std::string::npos) {
            std::string dir = db_path.substr(0, pos);
            createDirectoryPath(dir);
        }
        // Create empty database file immediately
        db->asyncExport(db_path);

        // Configure ZMQ socket
        zmq_socket.set(zmq::sockopt::sndhwm, 100);
        zmq_socket.set(zmq::sockopt::linger, 1000);

        // Bind socket
        int port = config["publish_port"];
        std::string bind_addr = "tcp://*:" + std::to_string(port);
        zmq_socket.bind(bind_addr);
        std::cout << "[Publisher] ZMQ socket bound to " << bind_addr << std::endl;

        // Give subscribers time to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    bool openVideo(const std::string& video_path = "") {
        std::string final_path = video_path.empty() ?
            config.value("video_input_path", DEFAULT_VIDEO_PATH) : video_path;

        std::cout << "[Publisher] Opening video: " << final_path << std::endl;

        // Check file exists
        std::ifstream test_file(final_path);
        if (!test_file.good()) {
            std::cerr << "[Publisher] File not found: " << final_path << std::endl;
            shm->updateVideoPath("FILE_NOT_FOUND");
            return false;
        }
        test_file.close();

        // Try to open with OpenCV
        if (!video_cap.open(final_path)) {
            std::cerr << "[Publisher] Failed to open video: " << final_path << std::endl;
            shm->updateVideoPath("OPEN_FAILED");
            return false;
        }

        // Get video properties
        int frame_count = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_COUNT));
        double fps = video_cap.get(cv::CAP_PROP_FPS);
        int width = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_WIDTH));
        int height = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_HEIGHT));

        // Fix: Check for invalid frame count
        if (frame_count < 0 || frame_count > 1000000) {  // Sanity check
            std::cout << "[Publisher] Invalid frame count detected: " << frame_count << ", setting to 0" << std::endl;
            frame_count = 0;  // Unknown
        }

        std::cout << "[Publisher] Video opened successfully:" << std::endl;
        std::cout << "  Resolution: " << width << "x" << height << std::endl;
        std::cout << "  Frames: " << (frame_count > 0 ? std::to_string(frame_count) : "unknown") << std::endl;
        std::cout << "  FPS: " << fps << std::endl;

        // Update shared memory with safe values
        shm->updateMetric("total_frames", frame_count > 0 ? frame_count : 0);
        shm->updateMetric("video_fps", fps);
        shm->updateMetric("video_width", width);
        shm->updateMetric("video_height", height);
        shm->updateVideoPath(final_path);

        return true;
    }

    void run() {
        if (!video_cap.isOpened()) {
            std::cerr << "[Publisher] Video not opened" << std::endl;
            return;
        }

        // Update video info immediately in shared memory
        int total_frames = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_COUNT));
        double video_fps = video_cap.get(cv::CAP_PROP_FPS);
        int width = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_WIDTH));
        int height = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_HEIGHT));

        shm->updateMetric("total_frames", total_frames);
        shm->updateMetric("video_fps", video_fps);
        shm->updateMetric("video_width", width);
        shm->updateMetric("video_height", height);

        cv::Mat frame;
        int frame_id = 0;

        // Safe config value extraction with defaults
        int jpeg_quality = 85;
        int frame_delay_ms = 33;
        int export_interval = 1000;

        try {
            if (config.contains("jpeg_quality") && config["jpeg_quality"].is_number()) {
                jpeg_quality = config["jpeg_quality"].get<int>();
            }
            if (config.contains("frame_delay_ms") && config["frame_delay_ms"].is_number()) {
                frame_delay_ms = config["frame_delay_ms"].get<int>();
            }
            if (config.contains("benchmark_export_interval") && config["benchmark_export_interval"].is_number()) {
                export_interval = config["benchmark_export_interval"].get<int>();
            }
        } catch (const std::exception& e) {
            std::cerr << "[Publisher] Error reading config in run(): " << e.what() << std::endl;
        }

        std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality};

        std::cout << "[Publisher] Starting frame publishing..." << std::endl;
        std::cout << "[Publisher] JPEG quality: " << jpeg_quality << ", Frame delay: " << frame_delay_ms << "ms" << std::endl;

        const bool loop_video = config.value("loop_video", false);

        while (running && g_running) {
            // Read frame
            if (!video_cap.read(frame) || frame.empty()) {
                if (loop_video) {
                    std::cout << "[Publisher] End of video, restarting..." << std::endl;
                    video_cap.set(cv::CAP_PROP_POS_FRAMES, 0);
                    continue;
                } else {
                    std::cout << "[Publisher] End of video reached. Exiting..." << std::endl;
                    break;
                }
            }

            if (frame.empty()) {
                continue;
            }

            try {
                // Create metadata - all these should be safe as we're setting values, not reading
                json metadata;
                metadata["frame_id"] = frame_id;
                metadata["timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();
                metadata["width"] = frame.cols;
                metadata["height"] = frame.rows;
                metadata["channels"] = frame.channels();
                metadata["source"] = "mp4";

                // Encode frame as JPEG
                std::vector<uchar> buffer;
                cv::imencode(".jpg", frame, buffer, jpeg_params);

                // Send metadata first
                std::string meta_str = metadata.dump();
                zmq::message_t meta_msg(meta_str.data(), meta_str.size());
                zmq_socket.send(meta_msg, zmq::send_flags::sndmore);

                // Send encoded frame
                zmq::message_t frame_msg(buffer.data(), buffer.size());
                zmq_socket.send(frame_msg, zmq::send_flags::none);

                frames_published++;
                frame_id++;

                // Store initial benchmark immediately
                if (frames_published == 1) {
                    auto metrics = shm->getMetricsSnapshot();
                    db->storeBenchmark(metrics);
                    std::cout << "[Publisher] Stored initial benchmark" << std::endl;
                }

                // Update metrics and store benchmark periodically
                if (frames_published <= 10 || frames_published % 10 == 0) {
                    auto now = steady_clock::now();
                    double elapsed = duration<double>(now - last_fps_update).count();
                    if (elapsed > 0 && frames_published > 0) {
                        double fps = (frames_published <= 10 ? frames_published : 10.0) / elapsed;
                        shm->updateMetric("current_fps", fps);
                        if (frames_published >= 10) {
                            last_fps_update = now;
                        }
                    }

                    shm->updateMetric("frames_published", frames_published);
                    shm->updateMetric("errors", errors);

                    // CRITICAL: Store benchmark to in-memory database
                    auto metrics = shm->getMetricsSnapshot();
                    db->storeBenchmark(metrics);
                }

                // Export database periodically
                if (frames_published % export_interval == 0) {
                    std::string db_path = "/var/lib/jvideo/db/publisher_benchmarks.db";
                    try {
                        if (config.contains("benchmark_db_path") && config["benchmark_db_path"].is_string()) {
                            db_path = config["benchmark_db_path"].get<std::string>();
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[Publisher] Error getting benchmark_db_path: " << e.what() << std::endl;
                    }

                    // Log the export
                    std::cout << "[Publisher] Exporting database at frame " << frames_published << std::endl;
                    db->asyncExport(db_path);
                }

            } catch (const zmq::error_t& e) {
                std::cerr << "[Publisher] ZMQ error: " << e.what() << std::endl;
                errors++;
            } catch (const json::exception& e) {
                std::cerr << "[Publisher] JSON error: " << e.what() << std::endl;
                errors++;
            } catch (const std::exception& e) {
                std::cerr << "[Publisher] Error: " << e.what() << std::endl;
                errors++;
            }
        }

        // Final updates - safe access to benchmark_db_path
        shm->updateMetric("frames_published", frames_published);
        shm->updateMetric("errors", errors);

        auto metrics = shm->getMetricsSnapshot();
        db->storeBenchmark(metrics);

        std::string final_db_path = "/var/lib/jvideo/db/publisher_benchmarks.db";
        try {
            if (config.contains("benchmark_db_path") && config["benchmark_db_path"].is_string()) {
                final_db_path = config["benchmark_db_path"].get<std::string>();
            }
        } catch (const std::exception& e) {
            std::cerr << "[Publisher] Error getting final benchmark_db_path: " << e.what() << std::endl;
        }
        db->asyncExport(final_db_path);

        auto total_time = duration<double>(steady_clock::now() - start_time).count();
        std::cout << "[Publisher] Finished. Published " << frames_published
                << " frames in " << std::fixed << std::setprecision(1)
                << total_time << " seconds" << std::endl;
    }

    void stop() {
        running = false;
    }

    ~MP4FramePublisher() {
        running = false;
        if (video_cap.isOpened()) {
            video_cap.release();
        }
        zmq_socket.close();
        zmq_context.close();
    }
};

// Signal handler - must be after class definition
void signal_handler(int sig) {
    std::cout << "\n[Publisher] Received signal " << sig << ", shutting down..." << std::endl;
    g_running = false;
    if (g_publisher) {
        g_publisher->stop();
    }
}

int main(int argc, char* argv[]) {
    std::cout << "[Publisher] MP4 Frame Publisher starting..." << std::endl;
    std::cout << "[Publisher] PID: " << getpid() << std::endl;

    // Test JSON library
    try {
        json test;
        test["test"] = "value";
        std::cout << "[Publisher] JSON library test passed" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Publisher] JSON library test failed: " << e.what() << std::endl;
        return 1;
    }

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        g_publisher = std::make_unique<MP4FramePublisher>();

        std::string video_path = (argc > 1) ? argv[1] : "";

        if (!g_publisher->openVideo(video_path)) {
            std::cerr << "[Publisher] Failed to open video file" << std::endl;
            return 1;
        }

        g_publisher->run();

    } catch (const std::exception& e) {
        std::cerr << "[Publisher] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Publisher] Shutdown complete" << std::endl;
    return 0;
}
