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
volatile sig_atomic_t g_running = 1;

// Signal handler function
void signal_handler(int sig) {
    g_running = 0;
}

#pragma pack(push, 1)
struct PublisherSharedMetrics {
    char service_name[64] = "mp4-frame-publisher";
    pid_t service_pid = getpid();

    std::atomic<int> frames_published{0};
    std::atomic<int> total_frames{0};
    std::atomic<int> errors{0};

    std::atomic<double> current_fps{0.0};
    std::atomic<double> video_fps{0.0};

    std::atomic<int> video_width{0};
    std::atomic<int> video_height{0};

    std::atomic<bool> video_healthy{true};
    char video_path[256] = {0};
    char _pad[64] = {0};

    std::atomic<time_t> last_update_time{0};
    std::atomic<time_t> service_start_time{time(nullptr)};

    // Explicitly delete copy constructor/assignment for atomic safety
    PublisherSharedMetrics(const PublisherSharedMetrics&) = delete;
    PublisherSharedMetrics& operator=(const PublisherSharedMetrics&) = delete;
};
#pragma pack(pop)

class PublisherSharedMemoryManager {
    int shm_fd = -1;
    PublisherSharedMetrics* metrics = nullptr;
    bool is_owner = false;

public:
    explicit PublisherSharedMemoryManager(const std::string& service_name) {
        shm_fd = shm_open("/mp4_frame_publisher_metrics", O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            throw std::runtime_error("shm_open failed: " + std::string(strerror(errno)));
        }

        if (ftruncate(shm_fd, sizeof(PublisherSharedMetrics)) == -1) {
            close(shm_fd);
            throw std::runtime_error("ftruncate failed: " + std::string(strerror(errno)));
        }

        metrics = static_cast<PublisherSharedMetrics*>(
            mmap(nullptr, sizeof(PublisherSharedMetrics),
                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
        );

        if (metrics == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error("mmap failed: " + std::string(strerror(errno)));
        }

        strncpy(metrics->service_name, service_name.c_str(), 63);
        metrics->service_pid = getpid();
        is_owner = true;
    }

    void updateMetric(const std::string& key, int value) {
        if (!metrics) return;

        if (key == "frames_published") metrics->frames_published = value;
        else if (key == "total_frames") metrics->total_frames = value;
        else if (key == "errors") metrics->errors = value;
        else if (key == "video_width") metrics->video_width = value;
        else if (key == "video_height") metrics->video_height = value;

        metrics->last_update_time = time(nullptr);
    }

    void updateMetric(const std::string& key, double value) {
        if (!metrics) return;

        if (key == "current_fps") metrics->current_fps = value;
        else if (key == "video_fps") metrics->video_fps = value;

        metrics->last_update_time = time(nullptr);
    }

    void updateVideoPath(const std::string& path) {
        if (metrics) {
            strncpy(metrics->video_path, path.c_str(), 255);
        }
    }

    // Return by reference to avoid copying atomics
    const PublisherSharedMetrics& getMetricsSnapshot() const {
        if (!metrics) {
            throw std::runtime_error("Shared memory not initialized");
        }
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
            shm_unlink("/mp4_frame_publisher_metrics");
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
            BEGIN TRANSACTION;
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
            COMMIT;
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
            throw std::runtime_error("Failed to open in-memory database: " + std::string(sqlite3_errmsg(db)));
        }

        // Configure for performance
        sqlite3_exec(db, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA temp_store = MEMORY", nullptr, nullptr, nullptr);

        initializeSchema();
    }

    void storeBenchmark(const PublisherSharedMetrics& metrics) {
        std::lock_guard<std::mutex> lock(db_mutex);

        sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

        const char* sql = R"(
            INSERT INTO publisher_benchmarks
            (timestamp, frames_published, total_frames, current_fps, video_fps, errors, uptime_seconds, memory_usage_kb)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw std::runtime_error("Failed to prepare statement: " + std::string(sqlite3_errmsg(db)));
        }

        try {
            time_t current_time = time(nullptr);
            int uptime = static_cast<int>(current_time - metrics.service_start_time);

            sqlite3_bind_int64(stmt, 1, current_time);
            sqlite3_bind_int(stmt, 2, metrics.frames_published.load());
            sqlite3_bind_int(stmt, 3, metrics.total_frames.load());
            sqlite3_bind_double(stmt, 4, metrics.current_fps.load());
            sqlite3_bind_double(stmt, 5, metrics.video_fps.load());
            sqlite3_bind_int(stmt, 6, metrics.errors.load());
            sqlite3_bind_int(stmt, 7, uptime);
            sqlite3_bind_int64(stmt, 8, getMemoryUsage());

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                throw std::runtime_error("Insert failed: " + std::string(sqlite3_errmsg(db)));
            }

            sqlite3_finalize(stmt);
            sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        } catch (...) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

    void asyncExport(const std::string& path) {
        if (export_in_progress.load()) return;

        export_in_progress = true;

        if (export_thread.joinable()) {
            export_thread.join();
        }

        export_thread = std::thread([this, path]() {
            try {
                std::lock_guard<std::mutex> lock(db_mutex);

                sqlite3* file_db;
                if (sqlite3_open(path.c_str(), &file_db) != SQLITE_OK) {
                    throw std::runtime_error("Failed to open export database: " + std::string(sqlite3_errmsg(file_db)));
                }

                sqlite3_backup* backup = sqlite3_backup_init(file_db, "main", db, "main");
                if (!backup) {
                    sqlite3_close(file_db);
                    throw std::runtime_error("Backup initialization failed");
                }

                sqlite3_backup_step(backup, -1); // Copy entire database
                sqlite3_backup_finish(backup);
                sqlite3_close(file_db);
            } catch (const std::exception& e) {
                std::cerr << "Export failed: " << e.what() << std::endl;
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
    static constexpr const char* DEFAULT_VIDEO_PATH = "/opt/jvideo/input.mp4";
    static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/jvideo/frame-publisher.conf";

    zmq::context_t zmq_context;
    zmq::socket_t zmq_socket;
    cv::VideoCapture video_cap;

    std::unique_ptr<PublisherSharedMemoryManager> shm;
    std::unique_ptr<PublisherBenchmarkDatabase> db;

    json config;
    std::atomic<bool> running{true};

    int frames_published = 0;
    int errors = 0;
    steady_clock::time_point start_time;

    void loadConfig() {
        config = {
            {"video_input_path", DEFAULT_VIDEO_PATH},
            {"publish_port", 5555},
            {"benchmark_db_path", "/var/log/jvideo/frame_publisher_benchmarks.db"},
            {"benchmark_export_interval", 100}
        };

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

                if (user_config.contains("video_input_path")) {
                    config["video_input_path"] = user_config["video_input_path"];
                }
                if (user_config.contains("publish_port")) {
                    config["publish_port"] = user_config["publish_port"];
                }
                if (user_config.contains("benchmark_db_path")) {
                    config["benchmark_db_path"] = user_config["benchmark_db_path"];
                }
                if (user_config.contains("benchmark_export_interval")) {
                    config["benchmark_export_interval"] = user_config["benchmark_export_interval"];
                }

                std::cout << "[Publisher] Loaded config from " << config_path << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Publisher] Failed to parse config: " << e.what()
                         << ", using defaults\n";
            }
        }
    }

public:
    MP4FramePublisher() : zmq_context(1), zmq_socket(zmq_context, ZMQ_PUB) {
        start_time = steady_clock::now();

        shm = std::make_unique<PublisherSharedMemoryManager>("mp4-frame-publisher");
        db = std::make_unique<PublisherBenchmarkDatabase>();

        loadConfig();

        zmq_socket.set(zmq::sockopt::sndhwm, 10);

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        int port = config["publish_port"];
        std::string bind_addr = "tcp://*:" + std::to_string(port);
        zmq_socket.bind(bind_addr);
        std::cout << "[Publisher] ZMQ socket bound to " << bind_addr << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    bool openVideo(const std::string& video_path = "") {
        std::string final_path;
        if (!video_path.empty()) {
            final_path = video_path;
            std::cout << "[Publisher] Using command line path: " << final_path << std::endl;
        } else {
            final_path = config["video_input_path"];
            std::cout << "[Publisher] Using configured path: " << final_path << std::endl;
        }

        std::ifstream test_file(final_path);
        if (!test_file.good()) {
            std::cerr << "[Publisher] File not accessible: " << final_path << std::endl;
            return false;
        }
        test_file.close();

        std::vector<std::pair<int, std::string>> backends_to_try = {
            {cv::CAP_ANY, "CAP_ANY"},
            {cv::CAP_FFMPEG, "CAP_FFMPEG"},
            {cv::CAP_GSTREAMER, "CAP_GSTREAMER"},
            {cv::CAP_V4L2, "CAP_V4L2"}
        };

        for (auto& backend_pair : backends_to_try) {
            int backend = backend_pair.first;
            const std::string& backend_name = backend_pair.second;

            std::cout << "[Publisher] Trying backend: " << backend_name << std::endl;

            if (video_cap.open(final_path, backend)) {
                std::cout << "[Publisher] Successfully opened with backend: " << backend_name << std::endl;

                int frame_count = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_COUNT));
                double fps = video_cap.get(cv::CAP_PROP_FPS);
                int width = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_WIDTH));
                int height = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_HEIGHT));

                std::cout << "[Publisher] Video opened: " << width << "x" << height
                        << ", " << frame_count << " frames @ " << fps << " fps\n";

                shm->updateMetric("total_frames", frame_count);
                shm->updateMetric("video_fps", fps);
                shm->updateMetric("video_width", width);
                shm->updateMetric("video_height", height);
                shm->updateVideoPath(final_path);

                return true;
            } else {
                std::cout << "[Publisher] Failed with backend: " << backend_name << std::endl;
            }
        }

        std::cerr << "[Publisher] Failed to open video with any backend: " << final_path << std::endl;
        return false;
    }

    void run() {
        if (!video_cap.isOpened()) {
            std::cerr << "[Publisher] Video not opened\n";
            return;
        }

        cv::Mat frame;
        int frame_id = 0;
        int export_interval = config["benchmark_export_interval"];

        std::cout << "[Publisher] Starting frame publishing...\n";

        while (g_running && running && video_cap.read(frame)) {
            if (frame.empty()) {
                std::cout << "[Publisher] Empty frame, skipping\n";
                continue;
            }

            try {
                json metadata;
                metadata["frame_id"] = frame_id;
                metadata["timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();
                metadata["width"] = frame.cols;
                metadata["height"] = frame.rows;
                metadata["channels"] = frame.channels();
                metadata["source"] = "mp4";

                std::string meta_str = metadata.dump();
                zmq::message_t meta_msg(meta_str.data(), meta_str.size());
                zmq_socket.send(meta_msg, zmq::send_flags::sndmore);

                size_t frame_size = frame.total() * frame.elemSize();
                zmq::message_t frame_msg(frame_size);
                std::memcpy(frame_msg.data(), frame.data, frame_size);
                zmq_socket.send(frame_msg, zmq::send_flags::none);

                frames_published++;
                frame_id++;

                // Update metrics every 10 frames
                if (frames_published % 10 == 0) {
                    auto now = steady_clock::now();
                    double elapsed = duration<double>(now - start_time).count();
                    double fps = frames_published / elapsed;

                    shm->updateMetric("frames_published", frames_published);
                    shm->updateMetric("current_fps", fps);
                    shm->updateMetric("errors", errors);
                }

                // Store benchmark every 1000 frames
                if (frames_published % 1000 == 0) {
                    auto& metrics_snapshot = shm->getMetricsSnapshot();
                    db->storeBenchmark(metrics_snapshot);

                    std::cout << "[Publisher] Published " << frames_published
                              << " frames, FPS: " << std::fixed << std::setprecision(1)
                              << metrics_snapshot.current_fps << "\n";
                }

                // Export periodically
                if (frames_published % export_interval == 0) {
                    db->asyncExport(config["benchmark_db_path"]);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(33));

            } catch (const zmq::error_t& e) {
                std::cerr << "[Publisher] ZMQ error: " << e.what() << std::endl;
                errors++;
                shm->updateMetric("errors", errors);
            } catch (const std::exception& e) {
                std::cerr << "[Publisher] Error: " << e.what() << std::endl;
                errors++;
                shm->updateMetric("errors", errors);
            }
        }

        // Final updates
        shm->updateMetric("frames_published", frames_published);
        shm->updateMetric("errors", errors);

        auto& final_metrics = shm->getMetricsSnapshot();
        db->storeBenchmark(final_metrics);
        db->asyncExport(config["benchmark_db_path"]);

        auto total_time = duration<double>(steady_clock::now() - start_time).count();
        std::cout << "[Publisher] Finished. Published " << frames_published
                  << " frames in " << std::fixed << std::setprecision(1) << total_time << " seconds\n";
    }

    void stop() {
        running = false;
    }

    ~MP4FramePublisher() {
        if (video_cap.isOpened()) {
            video_cap.release();
        }
        if (db) {
            db->asyncExport(config["benchmark_db_path"]);
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << "[Publisher] MP4 Frame Publisher starting...\n";

    try {
        MP4FramePublisher publisher;

        std::string video_path = (argc > 1) ? argv[1] : "";

        if (!publisher.openVideo(video_path)) {
            std::cerr << "[Publisher] Failed to open video file\n";
            return 1;
        }

        std::thread publisher_thread([&publisher]() {
            publisher.run();
        });

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        publisher.stop();
        if (publisher_thread.joinable()) {
            publisher_thread.join();
        }

    } catch (const std::exception& e) {
        std::cerr << "[Publisher] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Publisher] Shutdown complete\n";
    return 0;
}
