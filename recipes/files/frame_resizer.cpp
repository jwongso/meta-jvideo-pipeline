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
#include <unordered_map>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>

using namespace std::chrono;
namespace fs = std::filesystem;
using json = nlohmann::json;

#ifndef DEFAULT_CONFIG_PATH
#define DEFAULT_CONFIG_PATH "/etc/frame_resizer/config.json"
#endif

#pragma pack(push, 1)
struct ResizerSharedMetrics {
    char service_name[64] = "frame-resizer";
    pid_t service_pid = getpid();

    std::atomic<int> frames_processed{0};
    std::atomic<int> frames_dropped{0};
    std::atomic<int> errors{0};
    std::atomic<int> export_errors{0};

    std::atomic<double> current_fps{0.0};
    std::atomic<double> processing_time_ms{0.0};

    std::atomic<int> input_width{0};
    std::atomic<int> input_height{0};
    std::atomic<int> output_width{160};
    std::atomic<int> output_height{120};

    std::atomic<bool> db_healthy{true};
    char _pad[64] = {0};

    std::atomic<time_t> last_update_time{0};
    std::atomic<time_t> service_start_time{time(nullptr)};

    // Explicitly delete copy operations for atomic safety
    ResizerSharedMetrics(const ResizerSharedMetrics&) = delete;
    ResizerSharedMetrics& operator=(const ResizerSharedMetrics&) = delete;
};
#pragma pack(pop)

class ResizerSharedMemoryManager {
    int shm_fd = -1;
    ResizerSharedMetrics* metrics = nullptr;
    bool is_owner = false;

public:
    explicit ResizerSharedMemoryManager(const std::string& service_name) {
        shm_fd = shm_open("/frame_resizer_metrics", O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            throw std::runtime_error("shm_open failed: " + std::string(strerror(errno)));
        }

        if (ftruncate(shm_fd, sizeof(ResizerSharedMetrics))) {
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

        strncpy(metrics->service_name, service_name.c_str(), 63);
        metrics->service_pid = getpid();
        is_owner = true;
    }

    void updateMetric(const std::string& key, int value) {
        if (!metrics) return;

        if (key == "frames_processed") metrics->frames_processed = value;
        else if (key == "frames_dropped") metrics->frames_dropped = value;
        else if (key == "errors") metrics->errors = value;
        else if (key == "input_width") metrics->input_width = value;
        else if (key == "input_height") metrics->input_height = value;
        else if (key == "output_width") metrics->output_width = value;
        else if (key == "output_height") metrics->output_height = value;

        metrics->last_update_time = time(nullptr);
    }

    void updateMetric(const std::string& key, double value) {
        if (!metrics) return;

        if (key == "current_fps") metrics->current_fps = value;
        else if (key == "processing_time_ms") metrics->processing_time_ms = value;

        metrics->last_update_time = time(nullptr);
    }

    // Add method to safely access metrics
    const ResizerSharedMetrics& getMetrics() const {
        if (!metrics) {
            throw std::runtime_error("Shared memory not initialized");
        }
        return *metrics;
    }

    ~ResizerSharedMemoryManager() {
        if (metrics) munmap(metrics, sizeof(ResizerSharedMetrics));
        if (shm_fd != -1) close(shm_fd);
        if (is_owner) shm_unlink("/frame_resizer_metrics");
    }
};

class ResizerBenchmarkDatabase {
    sqlite3* db = nullptr;
    std::mutex db_mutex;
    std::atomic<bool> export_in_progress{false};
    std::thread export_thread;

    void initializeSchema() {
        const char* sql = R"(
            BEGIN TRANSACTION;
            CREATE TABLE IF NOT EXISTS benchmarks (
                timestamp INTEGER PRIMARY KEY,
                frames_processed INTEGER,
                frames_dropped INTEGER,
                fps REAL,
                processing_time_ms REAL,
                mem_usage_kb INTEGER,
                input_width INTEGER,
                input_height INTEGER,
                output_width INTEGER,
                output_height INTEGER,
                errors INTEGER
            );
            CREATE INDEX IF NOT EXISTS idx_timestamp ON benchmarks(timestamp);
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
    ResizerBenchmarkDatabase() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            throw std::runtime_error("Failed to open database: " + std::string(sqlite3_errmsg(db)));
        }

        sqlite3_exec(db, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA temp_store = MEMORY", nullptr, nullptr, nullptr);

        initializeSchema();
    }

    void storeBenchmark(const ResizerSharedMetrics& metrics) {
        std::lock_guard<std::mutex> lock(db_mutex);

        sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

        const char* sql = R"(
            INSERT INTO benchmarks
            (timestamp, frames_processed, frames_dropped, fps, processing_time_ms, mem_usage_kb,
             input_width, input_height, output_width, output_height, errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw std::runtime_error("Prepare failed: " + std::string(sqlite3_errmsg(db)));
        }

        try {
            sqlite3_bind_int64(stmt, 1, duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
            sqlite3_bind_int(stmt, 2, metrics.frames_processed.load());
            sqlite3_bind_int(stmt, 3, metrics.frames_dropped.load());
            sqlite3_bind_double(stmt, 4, metrics.current_fps.load());
            sqlite3_bind_double(stmt, 5, metrics.processing_time_ms.load());
            sqlite3_bind_int64(stmt, 6, getMemoryUsage());
            sqlite3_bind_int(stmt, 7, metrics.input_width.load());
            sqlite3_bind_int(stmt, 8, metrics.input_height.load());
            sqlite3_bind_int(stmt, 9, metrics.output_width.load());
            sqlite3_bind_int(stmt, 10, metrics.output_height.load());
            sqlite3_bind_int(stmt, 11, metrics.errors.load());

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

                sqlite3_backup_step(backup, -1);
                sqlite3_backup_finish(backup);
                sqlite3_close(file_db);
            } catch (const std::exception& e) {
                std::cerr << "Export failed: " << e.what() << std::endl;
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
    zmq::context_t ctx{1};
    zmq::socket_t subscriber{ctx, ZMQ_SUB};
    zmq::socket_t publisher{ctx, ZMQ_PUB};
    std::atomic<bool> running{true};

    int frames_processed = 0;
    int frames_dropped = 0;
    int errors = 0;
    json config;

    std::unique_ptr<ResizerSharedMemoryManager> shm;
    std::unique_ptr<ResizerBenchmarkDatabase> db;

    void loadConfig() {
        config = {
            {"subscribe_port", 5555},
            {"publish_port", 5556},
            {"output_width", 160},
            {"output_height", 120},
            {"interpolation", "linear"},
            {"benchmark_db_path", "/tmp/frame_resizer_benchmarks.db"},
            {"benchmark_export_interval", 100}
        };

        std::string config_path = DEFAULT_CONFIG_PATH;
        const char* env_path = std::getenv("FRAME_RESIZER_CONFIG");
        if (env_path) config_path = env_path;

        std::ifstream config_file(config_path);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;
                config.merge_patch(user_config);
                std::cout << "[Resizer] Loaded config from " << config_path << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Resizer] Config error: " << e.what() << "\nUsing defaults\n";
            }
        }
    }

public:
    FrameResizer() : subscriber(ctx, ZMQ_SUB), publisher(ctx, ZMQ_PUB) {
        loadConfig();

        subscriber.connect("tcp://localhost:" + std::to_string(config["subscribe_port"].get<int>()));
        subscriber.set(zmq::sockopt::subscribe, "");
        publisher.bind("tcp://*:" + std::to_string(config["publish_port"].get<int>()));

        shm = std::make_unique<ResizerSharedMemoryManager>("resizer");
        db = std::make_unique<ResizerBenchmarkDatabase>();

        shm->updateMetric("output_width", config["output_width"].get<int>());
        shm->updateMetric("output_height", config["output_height"].get<int>());
    }

    void processFrame() {
        zmq::message_t msg;
        if (!subscriber.recv(msg, zmq::recv_flags::dontwait)) return;

        try {
            auto start = steady_clock::now();
            cv::Mat frame = cv::imdecode(cv::Mat(1, msg.size(), CV_8UC1, msg.data()), cv::IMREAD_COLOR);
            if (frame.empty()) {
                frames_dropped++;
                shm->updateMetric("frames_dropped", frames_dropped);
                return;
            }

            shm->updateMetric("input_width", frame.cols);
            shm->updateMetric("input_height", frame.rows);

            cv::Mat resized;
            cv::Size output_size(config["output_width"].get<int>(), config["output_height"].get<int>());
            cv::resize(frame, resized, output_size);

            std::vector<uchar> buffer;
            cv::imencode(".jpg", resized, buffer);

            publisher.send(zmq::message_t(buffer.data(), buffer.size()), zmq::send_flags::none);

            frames_processed++;
            double proc_time = duration_cast<milliseconds>(steady_clock::now() - start).count();

            shm->updateMetric("frames_processed", frames_processed);
            shm->updateMetric("processing_time_ms", proc_time);

            if (frames_processed % 100 == 0) {
                static auto last_time = steady_clock::now();
                auto now = steady_clock::now();
                double fps = 100000.0 / duration_cast<microseconds>(now - last_time).count();
                shm->updateMetric("current_fps", fps);
                last_time = now;
            }

            if (frames_processed % 1000 == 0) {
                db->storeBenchmark(shm->getMetrics());
            }

            if (frames_processed % config["benchmark_export_interval"].get<int>() == 0) {
                db->asyncExport(config["benchmark_db_path"].get<std::string>());
            }

        } catch (const std::exception& e) {
            errors++;
            shm->updateMetric("errors", errors);
            std::cerr << "Processing error: " << e.what() << std::endl;
        }
    }

    void run() {
        while (running) {
            processFrame();
            std::this_thread::sleep_for(1ms);
        }
        db->asyncExport(config["benchmark_db_path"].get<std::string>());
    }

    void stop() { running = false; }

    ~FrameResizer() {
        stop();
        if (db) db->asyncExport(config["benchmark_db_path"].get<std::string>());
    }
};

volatile std::sig_atomic_t g_running = 1;

void signal_handler(int) { g_running = 0; }

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        FrameResizer resizer;
        while (g_running) {
            resizer.processFrame();
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
