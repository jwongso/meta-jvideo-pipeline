#include <zmq.hpp>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sys/mman.h>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

// Forward declaration
class FrameSaver;
std::unique_ptr<FrameSaver> g_saver;

// Shared metrics structure - NO atomics in shared memory!
struct SaverSharedMetrics {
    char service_name[64];
    pid_t service_pid;

    uint64_t frames_saved;
    uint64_t frames_dropped;
    uint64_t errors;
    uint64_t io_errors;

    double current_fps;
    double save_time_ms;
    double disk_usage_mb;

    int frame_width;
    int frame_height;
    int frame_channels;

    bool disk_healthy;
    char output_dir[256];
    char format[16];

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

class SaverSharedMemoryManager {
    int shm_fd = -1;
    SaverSharedMetrics* metrics = nullptr;
    bool is_owner = false;
    std::mutex update_mutex;

public:
    explicit SaverSharedMemoryManager(const std::string& service_name) {
        const char* shm_name = "/jvideo_saver_metrics";

        shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            throw std::runtime_error("shm_open failed: " + std::string(strerror(errno)));
        }

        if (ftruncate(shm_fd, sizeof(SaverSharedMetrics)) == -1) {
            close(shm_fd);
            throw std::runtime_error("ftruncate failed: " + std::string(strerror(errno)));
        }

        metrics = static_cast<SaverSharedMetrics*>(
            mmap(nullptr, sizeof(SaverSharedMetrics),
                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
        );

        if (metrics == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error("mmap failed: " + std::string(strerror(errno)));
        }

        std::lock_guard<std::mutex> lock(update_mutex);
        memset(metrics, 0, sizeof(SaverSharedMetrics));
        strncpy(metrics->service_name, service_name.c_str(), 63);
        metrics->service_pid = getpid();
        metrics->service_start_time = time(nullptr);
        metrics->disk_healthy = true;
        strcpy(metrics->format, "jpg");

        is_owner = true;
    }

    // Template for all numeric types
    template<typename T>
    void updateMetric(const std::string& key, T value) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);

        if (key == "frames_saved") metrics->frames_saved = static_cast<uint64_t>(value);
        else if (key == "frames_dropped") metrics->frames_dropped = static_cast<uint64_t>(value);
        else if (key == "errors") metrics->errors = static_cast<uint64_t>(value);
        else if (key == "io_errors") metrics->io_errors = static_cast<uint64_t>(value);
        else if (key == "current_fps") metrics->current_fps = static_cast<double>(value);
        else if (key == "save_time_ms") metrics->save_time_ms = static_cast<double>(value);
        else if (key == "disk_usage_mb") metrics->disk_usage_mb = static_cast<double>(value);
        else if (key == "frame_width") metrics->frame_width = static_cast<int>(value);
        else if (key == "frame_height") metrics->frame_height = static_cast<int>(value);
        else if (key == "frame_channels") metrics->frame_channels = static_cast<int>(value);
        else if (key == "disk_healthy") metrics->disk_healthy = (value > 0);

        metrics->last_update_time = time(nullptr);
    }

    void updateOutputDir(const std::string& dir) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);
        strncpy(metrics->output_dir, dir.c_str(), 255);
        metrics->output_dir[255] = '\0';
    }

    void updateFormat(const std::string& fmt) {
        if (!metrics) return;

        std::lock_guard<std::mutex> lock(update_mutex);
        strncpy(metrics->format, fmt.c_str(), 15);
        metrics->format[15] = '\0';
    }

    SaverSharedMetrics getMetricsSnapshot() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(update_mutex));
        return *metrics;
    }

    ~SaverSharedMemoryManager() {
        if (metrics && metrics != MAP_FAILED) {
            munmap(metrics, sizeof(SaverSharedMetrics));
        }
        if (shm_fd != -1) {
            close(shm_fd);
        }
        if (is_owner) {
            shm_unlink("/jvideo_saver_metrics");
        }
    }
};

class SaverBenchmarkDatabase {
    sqlite3* db = nullptr;
    std::mutex db_mutex;
    std::atomic<bool> export_in_progress{false};
    std::thread export_thread;

    void initializeSchema() {
        const char* sql = R"(
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
            CREATE INDEX IF NOT EXISTS idx_timestamp ON saver_benchmarks(timestamp);
        )";

        char* err_msg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::string error = err_msg ? err_msg : "Unknown error";
            sqlite3_free(err_msg);
            throw std::runtime_error("Schema initialization failed: " + error);
        }
    }

public:
    SaverBenchmarkDatabase() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            throw std::runtime_error("Failed to open in-memory database");
        }

        sqlite3_exec(db, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", nullptr, nullptr, nullptr);

        initializeSchema();
    }

    void storeBenchmark(const SaverSharedMetrics& metrics) {
        std::lock_guard<std::mutex> lock(db_mutex);

        const char* sql = R"(
            INSERT INTO saver_benchmarks
            (timestamp, frames_saved, current_fps, save_time_ms, disk_usage_mb,
             frame_width, frame_height, frame_channels, io_errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return;
        }

        sqlite3_bind_int64(stmt, 1, time(nullptr));
        sqlite3_bind_int64(stmt, 2, metrics.frames_saved);
        sqlite3_bind_double(stmt, 3, metrics.current_fps);
        sqlite3_bind_double(stmt, 4, metrics.save_time_ms);
        sqlite3_bind_double(stmt, 5, metrics.disk_usage_mb);
        sqlite3_bind_int(stmt, 6, metrics.frame_width);
        sqlite3_bind_int(stmt, 7, metrics.frame_height);
        sqlite3_bind_int(stmt, 8, metrics.frame_channels);
        sqlite3_bind_int64(stmt, 9, metrics.io_errors);

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

    ~SaverBenchmarkDatabase() {
        if (export_thread.joinable()) {
            export_thread.join();
        }
        if (db) {
            sqlite3_close(db);
        }
    }
};

class FrameSaver {
private:
    static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/jvideo/frame-saver.conf";

    zmq::context_t zmq_context;
    zmq::socket_t sub_socket;

    std::unique_ptr<SaverSharedMemoryManager> shm;
    std::unique_ptr<SaverBenchmarkDatabase> db;

    json config;
    std::atomic<bool> running{true};

    uint64_t frames_saved = 0;
    uint64_t frames_dropped = 0;
    uint64_t errors = 0;
    uint64_t io_errors = 0;

    steady_clock::time_point start_time;
    steady_clock::time_point last_fps_update;

    void loadConfig() {
        config = {
            {"subscribe_port", 5556},
            {"subscribe_host", "localhost"},
            {"output_dir", "/var/lib/jvideo/frames"},
            {"format", "jpg"},
            {"benchmark_db_path", "/var/lib/jvideo/db/saver_benchmarks.db"},
            {"benchmark_export_interval", 1000}
        };

        std::ifstream config_file(DEFAULT_CONFIG_PATH);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;
                config.merge_patch(user_config);
                std::cout << "[Saver] Loaded config from " << DEFAULT_CONFIG_PATH << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Saver] Config parse error: " << e.what() << ", using defaults" << std::endl;
            }
        }
    }

    double calculateDiskUsage() {
        try {
            auto space_info = fs::space(config["output_dir"].get<std::string>());
            double used_mb = static_cast<double>(space_info.capacity - space_info.available) / (1024.0 * 1024.0);
            return used_mb;
        } catch (...) {
            return 0.0;
        }
    }

    std::string generateFilename(int frame_id) {
        auto now = system_clock::now();
        auto time_t_now = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");
        ss << "_" << std::setfill('0') << std::setw(3) << ms.count();
        ss << "_frame" << std::setfill('0') << std::setw(6) << frame_id;
        ss << "." << config["format"].get<std::string>();

        return (fs::path(config["output_dir"].get<std::string>()) / ss.str()).string();
    }

public:
    FrameSaver() : zmq_context(1), sub_socket(zmq_context, ZMQ_SUB) {
        start_time = steady_clock::now();
        last_fps_update = start_time;

        loadConfig();

        // Configure socket
        sub_socket.set(zmq::sockopt::rcvhwm, 100);
        sub_socket.set(zmq::sockopt::subscribe, "");

        // Connect to resizer
        std::string sub_addr = "tcp://" + config["subscribe_host"].get<std::string>() +
                               ":" + std::to_string(config["subscribe_port"].get<int>());
        sub_socket.connect(sub_addr);
        std::cout << "[Saver] Connected to resizer at " << sub_addr << std::endl;

        // Create output directory
        try {
            fs::create_directories(config["output_dir"].get<std::string>());
            std::cout << "[Saver] Output directory: " << config["output_dir"] << std::endl;
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to create output directory: " + std::string(e.what()));
        }

        // Initialize components
        shm = std::make_unique<SaverSharedMemoryManager>("frame-saver");
        db = std::make_unique<SaverBenchmarkDatabase>();

        // Force initial database creation
        std::string db_path = config["benchmark_db_path"].get<std::string>();
        size_t pos = db_path.find_last_of("/");
        if (pos != std::string::npos) {
            std::string dir = db_path.substr(0, pos);
            createDirectoryPath(dir);
        }
        // Create empty database file immediately
        db->asyncExport(db_path);

        // Update shared memory with config
        shm->updateOutputDir(config["output_dir"].get<std::string>());
        shm->updateFormat(config["format"].get<std::string>());
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
                frames_dropped++;
                shm->updateMetric("frames_dropped", frames_dropped);
                return;
            }

            // Generate filename
            int frame_id = metadata.value("frame_id", static_cast<int>(frames_saved));
            std::string filename = generateFilename(frame_id);

            // Save frame
            bool save_success = cv::imwrite(filename, frame);

            auto save_end = steady_clock::now();
            double save_time_ms = duration<double, std::milli>(save_end - save_start).count();

            if (save_success) {
                frames_saved++;

                // Update metrics every 10 frames
                if (frames_saved <= 10 || frames_saved % 10 == 0) {
                    auto now = steady_clock::now();
                    double elapsed = duration<double>(now - last_fps_update).count();
                    if (elapsed > 0) {
                        double fps = 10.0 / elapsed;
                        shm->updateMetric("current_fps", fps);
                        last_fps_update = now;
                    }

                    shm->updateMetric("frames_saved", frames_saved);
                    shm->updateMetric("save_time_ms", save_time_ms);
                    shm->updateMetric("frame_width", frame.cols);
                    shm->updateMetric("frame_height", frame.rows);
                    shm->updateMetric("frame_channels", frame.channels());
                    shm->updateMetric("disk_healthy", 1);
                    shm->updateMetric("errors", errors);
                }

                // Export database periodically
                if (frames_saved % config["benchmark_export_interval"].get<int>() == 0) {
                    double disk_usage = calculateDiskUsage();
                    shm->updateMetric("disk_usage_mb", disk_usage);

                    auto metrics = shm->getMetricsSnapshot();
                    db->storeBenchmark(metrics);

                    std::cout << "[Saver] Saved " << frames_saved
                              << " frames, FPS: " << std::fixed << std::setprecision(1)
                              << metrics.current_fps
                              << ", Disk: " << std::fixed << std::setprecision(1)
                              << disk_usage << " MB" << std::endl;

                    db->asyncExport(config["benchmark_db_path"]);
                }

            } else {
                io_errors++;
                shm->updateMetric("io_errors", io_errors);
                shm->updateMetric("disk_healthy", 0);
                std::cerr << "[Saver] Failed to save frame: " << filename << std::endl;
            }

        } catch (const std::exception& e) {
            errors++;
            shm->updateMetric("errors", errors);
            std::cerr << "[Saver] Processing error: " << e.what() << std::endl;
        }
    }

    void run() {
        std::cout << "[Saver] Starting frame saving..." << std::endl;

        while (running && g_running) {
            processFrame();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // Final updates
        shm->updateMetric("frames_saved", frames_saved);
        shm->updateMetric("frames_dropped", frames_dropped);
        shm->updateMetric("errors", errors);
        shm->updateMetric("io_errors", io_errors);

        auto metrics = shm->getMetricsSnapshot();
        db->storeBenchmark(metrics);
        db->asyncExport(config["benchmark_db_path"]);

        auto total_time = duration<double>(steady_clock::now() - start_time).count();
        std::cout << "[Saver] Finished. Saved " << frames_saved
                  << " frames in " << std::fixed << std::setprecision(1)
                  << total_time << " seconds" << std::endl;
    }

    void stop() {
        running = false;
    }

    ~FrameSaver() {
        running = false;
        sub_socket.close();
        zmq_context.close();
    }
};

// Signal handler - must be after class definition
void signal_handler(int sig) {
    std::cout << "\n[Saver] Received signal " << sig << ", shutting down..." << std::endl;
    g_running = false;
    if (g_saver) {
        g_saver->stop();
    }
}

int main() {
    std::cout << "[Saver] Frame Saver starting..." << std::endl;
    std::cout << "[Saver] PID: " << getpid() << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        g_saver = std::make_unique<FrameSaver>();
        g_saver->run();
    } catch (const std::exception& e) {
        std::cerr << "[Saver] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Saver] Shutdown complete" << std::endl;
    return 0;
}
