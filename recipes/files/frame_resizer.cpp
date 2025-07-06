#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sys/mman.h>
#include <thread>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <sys/stat.h>

using namespace std::chrono;
namespace fs = std::filesystem;
using json = nlohmann::json;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

// Forward declaration
class FrameResizer;
std::unique_ptr<FrameResizer> g_resizer;

// Shared metrics structure - NO atomics in shared memory!
struct ResizerSharedMetrics {
    char service_name[64];
    pid_t service_pid;

    uint64_t frames_processed;
    uint64_t frames_dropped;
    uint64_t errors;

    double current_fps;
    double processing_time_ms;

    int input_width;
    int input_height;
    int output_width;
    int output_height;

    bool service_healthy;

    time_t last_update_time;
    time_t service_start_time;

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

class ResizerSharedMemoryManager {
    int shm_fd = -1;
    ResizerSharedMetrics* metrics = nullptr;
    bool is_owner = false;
    std::mutex update_mutex;

public:
    explicit ResizerSharedMemoryManager(const std::string& service_name) {
        const char* shm_name = "/jvideo_resizer_metrics";

        shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            throw std::runtime_error("shm_open failed: " + std::string(strerror(errno)));
        }

        if (ftruncate(shm_fd, sizeof(ResizerSharedMetrics)) == -1) {
            close(shm_fd);
            throw std::runtime_error("ftruncate failed: " + std::string(strerror(errno)));
        }

        metrics = static_cast<ResizerSharedMetrics*>(
            mmap(nullptr, sizeof(ResizerSharedMetrics),
                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
        );

        if (metrics == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error("mmap failed: " + std::string(strerror(errno)));
        }

        std::lock_guard<std::mutex> lock(update_mutex);
        memset(metrics, 0, sizeof(ResizerSharedMetrics));
        strncpy(metrics->service_name, service_name.c_str(), 63);
        metrics->service_pid = getpid();
        metrics->service_start_time = time(nullptr);
        metrics->service_healthy = true;

        is_owner = true;
    }

    // Template for all integer types
    template<typename T>
    typename std::enable_if<std::is_integral<T>::value>::type
    updateMetric(const std::string& key, T value) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);

        if (key == "frames_processed") metrics->frames_processed = static_cast<uint64_t>(value);
        else if (key == "frames_dropped") metrics->frames_dropped = static_cast<uint64_t>(value);
        else if (key == "errors") metrics->errors = static_cast<uint64_t>(value);
        else if (key == "input_width") metrics->input_width = static_cast<int>(value);
        else if (key == "input_height") metrics->input_height = static_cast<int>(value);
        else if (key == "output_width") metrics->output_width = static_cast<int>(value);
        else if (key == "output_height") metrics->output_height = static_cast<int>(value);

        metrics->last_update_time = time(nullptr);
    }

    // Overload for double
    void updateMetric(const std::string& key, double value) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);

        if (key == "current_fps") metrics->current_fps = value;
        else if (key == "processing_time_ms") metrics->processing_time_ms = value;

        metrics->last_update_time = time(nullptr);
    }

    ResizerSharedMetrics getMetricsSnapshot() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(update_mutex));
        return *metrics;
    }

    ~ResizerSharedMemoryManager() {
        if (metrics && metrics != MAP_FAILED) {
            munmap(metrics, sizeof(ResizerSharedMetrics));
        }
        if (shm_fd != -1) {
            close(shm_fd);
        }
        if (is_owner) {
            shm_unlink("/jvideo_resizer_metrics");
        }
    }
};

class ResizerBenchmarkDatabase {
    sqlite3* db = nullptr;
    std::mutex db_mutex;
    std::atomic<bool> export_in_progress{false};
    std::thread export_thread;

    void initializeSchema() {
        const char* sql = R"(
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
            CREATE INDEX IF NOT EXISTS idx_timestamp ON resizer_benchmarks(timestamp);
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
    ResizerBenchmarkDatabase() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            throw std::runtime_error("Failed to open database");
        }

        sqlite3_exec(db, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", nullptr, nullptr, nullptr);

        initializeSchema();
    }

    void storeBenchmark(const ResizerSharedMetrics& metrics) {
        std::lock_guard<std::mutex> lock(db_mutex);

        const char* sql = R"(
            INSERT INTO resizer_benchmarks
            (timestamp, frames_processed, frames_dropped, current_fps, processing_time_ms,
             memory_usage_kb, input_width, input_height, output_width, output_height, errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return;
        }

        sqlite3_bind_int64(stmt, 1, time(nullptr));
        sqlite3_bind_int64(stmt, 2, metrics.frames_processed);
        sqlite3_bind_int64(stmt, 3, metrics.frames_dropped);
        sqlite3_bind_double(stmt, 4, metrics.current_fps);
        sqlite3_bind_double(stmt, 5, metrics.processing_time_ms);
        sqlite3_bind_int64(stmt, 6, getMemoryUsage());
        sqlite3_bind_int(stmt, 7, metrics.input_width);
        sqlite3_bind_int(stmt, 8, metrics.input_height);
        sqlite3_bind_int(stmt, 9, metrics.output_width);
        sqlite3_bind_int(stmt, 10, metrics.output_height);
        sqlite3_bind_int64(stmt, 11, metrics.errors);

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

    ~ResizerBenchmarkDatabase() {
        if (export_thread.joinable()) {
            export_thread.join();
        }
        if (db) {
            sqlite3_close(db);
        }
    }
};

class FrameResizer {
private:
    static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/jvideo/frame-resizer.conf";

    zmq::context_t zmq_context;
    zmq::socket_t sub_socket;
    zmq::socket_t pub_socket;

    std::unique_ptr<ResizerSharedMemoryManager> shm;
    std::unique_ptr<ResizerBenchmarkDatabase> db;

    json config;
    std::atomic<bool> running{true};

    uint64_t frames_processed = 0;
    uint64_t frames_dropped = 0;
    uint64_t errors = 0;

    steady_clock::time_point start_time;
    steady_clock::time_point last_fps_update;

    void loadConfig() {
        config = {
            {"subscribe_port", 5555},
            {"publish_port", 5556},
            {"output_width", 160},
            {"output_height", 120},
            {"jpeg_quality", 85},
            {"benchmark_db_path", "/var/lib/jvideo/db/resizer_benchmarks.db"},
            {"benchmark_export_interval", 1000}
        };

        std::ifstream config_file(DEFAULT_CONFIG_PATH);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;
                config.merge_patch(user_config);
                std::cout << "[Resizer] Loaded config from " << DEFAULT_CONFIG_PATH << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Resizer] Config parse error: " << e.what() << ", using defaults" << std::endl;
            }
        }
    }

public:
    FrameResizer() : zmq_context(1),
                     sub_socket(zmq_context, ZMQ_SUB),
                     pub_socket(zmq_context, ZMQ_PUB) {
        start_time = steady_clock::now();
        last_fps_update = start_time;

        loadConfig();

        // Configure sockets
        sub_socket.set(zmq::sockopt::rcvhwm, 100);
        sub_socket.set(zmq::sockopt::subscribe, "");
        pub_socket.set(zmq::sockopt::sndhwm, 100);
        pub_socket.set(zmq::sockopt::linger, 1000);

        // Connect subscriber
        std::string sub_addr = "tcp://localhost:" + std::to_string(config["subscribe_port"].get<int>());
        sub_socket.connect(sub_addr);
        std::cout << "[Resizer] Connected to publisher at " << sub_addr << std::endl;

        // Bind publisher
        std::string pub_addr = "tcp://*:" + std::to_string(config["publish_port"].get<int>());
        pub_socket.bind(pub_addr);
        std::cout << "[Resizer] Publishing on " << pub_addr << std::endl;

        // Initialize components
        shm = std::make_unique<ResizerSharedMemoryManager>("frame-resizer");
        db = std::make_unique<ResizerBenchmarkDatabase>();

        // Force initial database creation
        std::string db_path = config["benchmark_db_path"].get<std::string>();
        size_t pos = db_path.find_last_of("/");
        if (pos != std::string::npos) {
            std::string dir = db_path.substr(0, pos);
            createDirectoryPath(dir);
        }
        // Create empty database file immediately
        db->asyncExport(db_path);

        // Update initial config in shared memory
        shm->updateMetric("output_width", config["output_width"].get<int>());
        shm->updateMetric("output_height", config["output_height"].get<int>());
    }

    void processFrame() {
        zmq::message_t meta_msg, frame_msg;

        // Try to receive metadata (non-blocking)
        if (!sub_socket.recv(meta_msg, zmq::recv_flags::dontwait)) {
            return;
        }

        // Check if there's more (frame data)
        if (!meta_msg.more()) {
            frames_dropped++;
            return;
        }

        // Receive frame data
        if (!sub_socket.recv(frame_msg, zmq::recv_flags::none)) {
            frames_dropped++;
            return;
        }

        try {
            auto proc_start = steady_clock::now();

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
                frames_dropped++;
                shm->updateMetric("frames_dropped", frames_dropped);
                return;
            }

            // Update input dimensions
            shm->updateMetric("input_width", frame.cols);
            shm->updateMetric("input_height", frame.rows);

            // Resize frame
            cv::Mat resized;
            cv::Size output_size(
                config["output_width"].get<int>(),
                config["output_height"].get<int>()
            );
            cv::resize(frame, resized, output_size, 0, 0, cv::INTER_LINEAR);

            // Encode resized frame
            std::vector<int> jpeg_params = {
                cv::IMWRITE_JPEG_QUALITY,
                config["jpeg_quality"].get<int>()
            };
            std::vector<uchar> encoded;
            cv::imencode(".jpg", resized, encoded, jpeg_params);

            // Update metadata
            metadata["resized_width"] = resized.cols;
            metadata["resized_height"] = resized.rows;
            metadata["resizer_timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();

            // Send resized frame (metadata + data)
            std::string new_meta_str = metadata.dump();
            zmq::message_t new_meta_msg(new_meta_str.data(), new_meta_str.size());
            pub_socket.send(new_meta_msg, zmq::send_flags::sndmore);

            zmq::message_t new_frame_msg(encoded.data(), encoded.size());
            pub_socket.send(new_frame_msg, zmq::send_flags::none);

            frames_processed++;

            // Calculate processing time
            auto proc_end = steady_clock::now();
            double proc_time_ms = duration<double, std::milli>(proc_end - proc_start).count();

            // Update metrics every 10 frames
            if (frames_processed <= 10 || frames_processed % 10 == 0) {
                auto now = steady_clock::now();
                double elapsed = duration<double>(now - last_fps_update).count();
                if (elapsed > 0) {
                    double fps = 10.0 / elapsed;
                    shm->updateMetric("current_fps", fps);
                    last_fps_update = now;
                }

                shm->updateMetric("frames_processed", frames_processed);
                shm->updateMetric("processing_time_ms", proc_time_ms);
                shm->updateMetric("errors", errors);
            }

            // Export database periodically
            if (frames_processed % config["benchmark_export_interval"].get<int>() == 0) {
                auto metrics = shm->getMetricsSnapshot();
                db->storeBenchmark(metrics);

                std::cout << "[Resizer] Processed " << frames_processed
                          << " frames, FPS: " << std::fixed << std::setprecision(1)
                          << metrics.current_fps << std::endl;

                db->asyncExport(config["benchmark_db_path"]);
            }

        } catch (const std::exception& e) {
            errors++;
            shm->updateMetric("errors", errors);
            std::cerr << "[Resizer] Processing error: " << e.what() << std::endl;
        }
    }

    void run() {
        std::cout << "[Resizer] Starting frame processing..." << std::endl;

        while (running && g_running) {
            processFrame();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // Final updates
        shm->updateMetric("frames_processed", frames_processed);
        shm->updateMetric("frames_dropped", frames_dropped);
        shm->updateMetric("errors", errors);

        auto metrics = shm->getMetricsSnapshot();
        db->storeBenchmark(metrics);
        db->asyncExport(config["benchmark_db_path"]);

        auto total_time = duration<double>(steady_clock::now() - start_time).count();
        std::cout << "[Resizer] Finished. Processed " << frames_processed
                  << " frames in " << std::fixed << std::setprecision(1)
                  << total_time << " seconds" << std::endl;
    }

    void stop() {
        running = false;
    }

    ~FrameResizer() {
        running = false;
        sub_socket.close();
        pub_socket.close();
        zmq_context.close();
    }
};

// Signal handler - must be after class definition
void signal_handler(int sig) {
    std::cout << "\n[Resizer] Received signal " << sig << ", shutting down..." << std::endl;
    g_running = false;
    if (g_resizer) {
        g_resizer->stop();
    }
}

int main() {
    std::cout << "[Resizer] Frame Resizer starting..." << std::endl;
    std::cout << "[Resizer] PID: " << getpid() << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        g_resizer = std::make_unique<FrameResizer>();
        g_resizer->run();
    } catch (const std::exception& e) {
        std::cerr << "[Resizer] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Resizer] Shutdown complete" << std::endl;
    return 0;
}
