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

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

#pragma pack(push, 1)
struct SaverSharedMetrics {
    char service_name[64] = "frame-saver";
    pid_t service_pid = getpid();

    std::atomic<int> frames_saved{0};
    std::atomic<int> frames_dropped{0};
    std::atomic<int> errors{0};
    std::atomic<int> io_errors{0};

    std::atomic<double> current_fps{0.0};
    std::atomic<double> save_time_ms{0.0};
    std::atomic<double> disk_usage_mb{0.0};

    std::atomic<int> frame_width{0};
    std::atomic<int> frame_height{0};
    std::atomic<int> frame_channels{0};

    std::atomic<bool> disk_healthy{true};
    char output_dir[256] = {0};
    char format[16] = {0};
    char _pad[64] = {0};

    std::atomic<time_t> last_update_time{0};
    std::atomic<time_t> service_start_time{time(nullptr)};
};
#pragma pack(pop)

class SaverSharedMemoryManager {
    int shm_fd = -1;
    SaverSharedMetrics* metrics = nullptr;
    bool is_owner = false;

public:
    explicit SaverSharedMemoryManager(const std::string& service_name) {
        shm_fd = shm_open("/frame_saver_metrics", O_CREAT | O_RDWR, 0666);
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

        strncpy(metrics->service_name, service_name.c_str(), 63);
        metrics->service_pid = getpid();
        is_owner = true;
    }

    void updateMetric(const std::string& key, double value) {
        if (!metrics) return;

        if (key == "frames_saved") metrics->frames_saved = static_cast<int>(value);
        else if (key == "frames_dropped") metrics->frames_dropped = static_cast<int>(value);
        else if (key == "errors") metrics->errors = static_cast<int>(value);
        else if (key == "io_errors") metrics->io_errors = static_cast<int>(value);
        else if (key == "current_fps") metrics->current_fps = value;
        else if (key == "save_time_ms") metrics->save_time_ms = value;
        else if (key == "disk_usage_mb") metrics->disk_usage_mb = value;
        else if (key == "frame_width") metrics->frame_width = static_cast<int>(value);
        else if (key == "frame_height") metrics->frame_height = static_cast<int>(value);
        else if (key == "frame_channels") metrics->frame_channels = static_cast<int>(value);
        else if (key == "disk_healthy") metrics->disk_healthy = (value > 0);

        metrics->last_update_time = time(nullptr);
    }

    void updateOutputDir(const std::string& dir) {
        if (metrics) {
            strncpy(metrics->output_dir, dir.c_str(), 255);
        }
    }

    void updateFormat(const std::string& fmt) {
        if (metrics) {
            strncpy(metrics->format, fmt.c_str(), 15);
        }
    }

    ~SaverSharedMemoryManager() {
        if (metrics && metrics != MAP_FAILED) {
            munmap(metrics, sizeof(SaverSharedMetrics));
        }
        if (shm_fd != -1) {
            close(shm_fd);
        }
        if (is_owner) {
            shm_unlink("/frame_saver_metrics");
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
                fps REAL,
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
            throw std::runtime_error("Failed to open in-memory database: " + std::string(sqlite3_errmsg(db)));
        }
        initializeSchema();
    }

    void recordBenchmark(int frames_saved, double fps, double save_time_ms,
                        double disk_usage_mb, int width, int height, int channels, int io_errors) {
        std::lock_guard<std::mutex> lock(db_mutex);

        const char* sql = R"(
            INSERT INTO saver_benchmarks
            (timestamp, frames_saved, fps, save_time_ms, disk_usage_mb, frame_width, frame_height, frame_channels, io_errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
            sqlite3_bind_int(stmt, 2, frames_saved);
            sqlite3_bind_double(stmt, 3, fps);
            sqlite3_bind_double(stmt, 4, save_time_ms);
            sqlite3_bind_double(stmt, 5, disk_usage_mb);
            sqlite3_bind_int(stmt, 6, width);
            sqlite3_bind_int(stmt, 7, height);
            sqlite3_bind_int(stmt, 8, channels);
            sqlite3_bind_int(stmt, 9, io_errors);

            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
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

                if (sqlite3_open(path.c_str(), &file_db) == SQLITE_OK) {
                    sqlite3_backup* backup = sqlite3_backup_init(file_db, "main", db, "main");
                    if (backup) {
                        sqlite3_backup_step(backup, -1);
                        sqlite3_backup_finish(backup);
                    }
                    sqlite3_close(file_db);
                } else {
                    std::cerr << "Failed to open export database: " << path << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Export failed: " << e.what() << std::endl;
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
    zmq::context_t context;
    zmq::socket_t sub_socket;
    json config;

    std::atomic<bool> running{true};
    int frames_saved = 0;
    int frames_dropped = 0;
    int errors = 0;
    int io_errors = 0;

    std::unique_ptr<SaverSharedMemoryManager> shm;
    std::unique_ptr<SaverBenchmarkDatabase> db;

    steady_clock::time_point last_fps_calculation;
    int frames_since_last_fps = 0;

    double calculateDiskUsage() {
        try {
            auto space_info = fs::space(config["output_dir"].get<std::string>());
            return static_cast<double>(space_info.capacity - space_info.available) / (1024.0 * 1024.0);
        } catch (...) {
            return 0.0;
        }
    }

public:
    FrameSaver() : context(1), sub_socket(context, ZMQ_SUB) {
        loadConfig();
        setupSubscriber();
        ensureOutputDirectory();

        shm = std::make_unique<SaverSharedMemoryManager>("frame-saver");
        db = std::make_unique<SaverBenchmarkDatabase>();

        // Initialize shared memory with config values
        shm->updateOutputDir(config["output_dir"].get<std::string>());
        shm->updateFormat(config["format"].get<std::string>());

        last_fps_calculation = steady_clock::now();
    }

    ~FrameSaver() {
        running = false;
        if (db) {
            db->asyncExport(config["benchmark_db_path"].get<std::string>());
        }
        sub_socket.close();
        context.close();
    }

    void loadConfig() {
        config = {
            {"subscribe_port", 5556},
            {"subscribe_host", "localhost"},
            {"output_dir", "/var/lib/jvideo/frames"},
            {"format", "jpg"},
            {"benchmark_db_path", "/var/log/jvideo/frame_saver_benchmarks.db"},
            {"benchmark_export_interval", 100}
        };

        std::ifstream config_file("/etc/jvideo/frame-saver.conf");
        if (config_file.is_open()) {
            try {
                json file_config;
                config_file >> file_config;

                // Safely merge known config keys
                if (file_config.contains("subscribe_port")) {
                    config["subscribe_port"] = file_config["subscribe_port"];
                }
                if (file_config.contains("subscribe_host")) {
                    config["subscribe_host"] = file_config["subscribe_host"];
                }
                if (file_config.contains("output_dir")) {
                    config["output_dir"] = file_config["output_dir"];
                }
                if (file_config.contains("format")) {
                    config["format"] = file_config["format"];
                }
                if (file_config.contains("benchmark_db_path")) {
                    config["benchmark_db_path"] = file_config["benchmark_db_path"];
                }
                if (file_config.contains("benchmark_export_interval")) {
                    config["benchmark_export_interval"] = file_config["benchmark_export_interval"];
                }

                std::cout << "[Saver] Loaded config from /etc/jvideo/frame-saver.conf" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Saver] Config parse error: " << e.what() << ", using defaults" << std::endl;
            }
        }
    }

    void setupSubscriber() {
        sub_socket.connect("tcp://" + config["subscribe_host"].get<std::string>() +
                          ":" + std::to_string(config["subscribe_port"].get<int>()));
        sub_socket.set(zmq::sockopt::subscribe, "");
    }

    void ensureOutputDirectory() {
        try {
            fs::create_directories(config["output_dir"].get<std::string>());
        } catch (const std::exception& e) {
            std::cerr << "Failed to create output directory: " << e.what() << std::endl;
            throw;
        }
    }

    std::string generateFilename() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);

        // Add microseconds for uniqueness
        auto microseconds = duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()
        ).count() % 1000000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S")
           << "_" << std::setfill('0') << std::setw(6) << microseconds
           << "." << config["format"].get<std::string>();
        return (fs::path(config["output_dir"].get<std::string>()) / ss.str()).string();
    }

    void run() {
        while (running) {
            zmq::message_t frame_msg;

            // Receive frame data (expecting encoded image data, not raw pixels + metadata)
            if (!sub_socket.recv(frame_msg, zmq::recv_flags::dontwait)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            try {
                auto save_start = steady_clock::now();

                // Decode the received image data
                std::vector<uchar> buffer(
                    static_cast<uchar*>(frame_msg.data()),
                    static_cast<uchar*>(frame_msg.data()) + frame_msg.size()
                );

                cv::Mat frame = cv::imdecode(buffer, cv::IMREAD_COLOR);
                if (frame.empty()) {
                    frames_dropped++;
                    shm->updateMetric("frames_dropped", frames_dropped);
                    std::cerr << "Failed to decode received frame" << std::endl;
                    continue;
                }

                int height = frame.rows;
                int width = frame.cols;
                int channels = frame.channels();

                std::string filename = generateFilename();
                bool save_success = cv::imwrite(filename, frame);

                auto save_end = steady_clock::now();
                double save_time = duration_cast<microseconds>(save_end - save_start).count() / 1000.0;

                if (save_success) {
                    frames_saved++;
                    frames_since_last_fps++;

                    // Update shared memory metrics
                    shm->updateMetric("frames_saved", frames_saved);
                    shm->updateMetric("save_time_ms", save_time);
                    shm->updateMetric("frame_width", width);
                    shm->updateMetric("frame_height", height);
                    shm->updateMetric("frame_channels", channels);
                    shm->updateMetric("disk_healthy", 1);

                    // Calculate FPS every 100 frames or every 5 seconds
                    auto now = steady_clock::now();
                    auto time_diff = duration_cast<milliseconds>(now - last_fps_calculation);

                    if (frames_since_last_fps >= 100 || time_diff.count() >= 5000) {
                        if (time_diff.count() > 0) {
                            double fps = (frames_since_last_fps * 1000.0) / time_diff.count();
                            shm->updateMetric("current_fps", fps);

                            // Record benchmark data
                            double disk_usage = calculateDiskUsage();
                            shm->updateMetric("disk_usage_mb", disk_usage);

                            db->recordBenchmark(frames_saved, fps, save_time, disk_usage,
                                              width, height, channels, io_errors);
                        }

                        last_fps_calculation = now;
                        frames_since_last_fps = 0;
                    }

                    // Periodic database export
                    if (frames_saved % config["benchmark_export_interval"].get<int>() == 0) {
                        db->asyncExport(config["benchmark_db_path"].get<std::string>());
                    }

                } else {
                    io_errors++;
                    shm->updateMetric("io_errors", io_errors);
                    shm->updateMetric("disk_healthy", 0);
                    std::cerr << "Failed to save frame: " << filename << std::endl;
                }

            } catch (const std::exception& e) {
                errors++;
                shm->updateMetric("errors", errors);
                std::cerr << "Frame processing error: " << e.what() << std::endl;
            }
        }

        // Final export on shutdown
        db->asyncExport(config["benchmark_db_path"].get<std::string>());
    }

    void stop() {
        running = false;
    }
};

volatile std::sig_atomic_t g_running = 1;

void signal_handler(int) {
    g_running = 0;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        FrameSaver saver;

        std::thread saver_thread([&saver]() {
            saver.run();
        });

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        saver.stop();
        if (saver_thread.joinable()) {
            saver_thread.join();
        }

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
