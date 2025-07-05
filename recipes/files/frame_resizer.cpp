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

// Define DEFAULT_CONFIG_PATH if not defined elsewhere
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
};
#pragma pack(pop)

class ResizerSharedMemoryManager {
    int shm_fd = -1;
    ResizerSharedMetrics* metrics = nullptr;
    bool is_owner = false;

public:
    explicit ResizerSharedMemoryManager(const std::string& service_name) {
        shm_fd = shm_open("/frame_resizer_metrics", O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) throw std::runtime_error("shm_open failed");

        if (ftruncate(shm_fd, sizeof(ResizerSharedMetrics))) {
            close(shm_fd);
            throw std::runtime_error("ftruncate failed");
        }

        metrics = static_cast<ResizerSharedMetrics*>(
            mmap(nullptr, sizeof(ResizerSharedMetrics),
            PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
        );
        if (metrics == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error("mmap failed");
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

    ~ResizerSharedMemoryManager() {
        if (metrics) munmap(metrics, sizeof(ResizerSharedMetrics));
        if (shm_fd != -1) close(shm_fd);
        if (is_owner) shm_unlink("/frame_resizer_metrics");
    }
};

// Benchmark Database for Frame Resizer
class ResizerBenchmarkDatabase {
    sqlite3* db = nullptr;
    std::mutex db_mutex;
    std::atomic<bool> export_in_progress{false};
    std::thread export_thread;

    void initializeSchema() {
        const char* sql = R"(
            CREATE TABLE IF NOT EXISTS benchmarks (
                timestamp INTEGER PRIMARY KEY,
                fps REAL,
                processing_time_ms REAL,
                mem_usage_kb INTEGER
            )
        )";
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }

public:
    ResizerBenchmarkDatabase() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
        initializeSchema();
    }

    void asyncExport(const std::string& path) {
        if (export_in_progress) return;
        export_in_progress = true;

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
                }
            } catch (...) {
                std::cerr << "Export failed\n";
            }
            export_in_progress = false;
        });
        export_thread.detach();
    }

    ~ResizerBenchmarkDatabase() {
        if (export_thread.joinable()) export_thread.join();
        if (db) sqlite3_close(db);
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
        // Default configuration
        config = {
            {"subscribe_port", 5555},
            {"publish_port", 5556},
            {"output_width", 160},
            {"output_height", 120},
            {"interpolation", "linear"},
            {"benchmark_db_path", "/tmp/frame_resizer_benchmarks.db"},
            {"benchmark_export_interval", 100}  // Export every 10000 frames
        };

        // Try to load from config file
        std::string config_path = DEFAULT_CONFIG_PATH;
        const char* env_path = std::getenv("FRAME_RESIZER_CONFIG");
        if (env_path) {
            config_path = env_path;
        }

        std::ifstream config_file(config_path);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;

                // Only merge the keys we care about
                if (user_config.contains("subscribe_port")) {
                    config["subscribe_port"] = user_config["subscribe_port"];
                }
                if (user_config.contains("publish_port")) {
                    config["publish_port"] = user_config["publish_port"];
                }
                if (user_config.contains("output_width")) {
                    config["output_width"] = user_config["output_width"];
                }
                if (user_config.contains("output_height")) {
                    config["output_height"] = user_config["output_height"];
                }
                if (user_config.contains("interpolation")) {
                    config["interpolation"] = user_config["interpolation"];
                }
                if (user_config.contains("benchmark_db_path")) {
                    config["benchmark_db_path"] = user_config["benchmark_db_path"];
                }
                if (user_config.contains("benchmark_export_interval")) {
                    config["benchmark_export_interval"] = user_config["benchmark_export_interval"];
                }

                std::cout << "[Resizer] Loaded config from " << config_path << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Resizer] Failed to parse config: " << e.what()
                         << ", using defaults\n";
            }
        }
    }

public:
    FrameResizer() : subscriber(ctx, ZMQ_SUB), publisher(ctx, ZMQ_PUB) {
        loadConfig();

        // Configure subscriber and publisher with config values
        std::string subscribe_addr = "tcp://localhost:" + std::to_string(config["subscribe_port"].get<int>());
        std::string publish_addr = "tcp://*:" + std::to_string(config["publish_port"].get<int>());

        subscriber.connect(subscribe_addr);
        subscriber.set(zmq::sockopt::subscribe, ""); // Subscribe to all messages
        publisher.bind(publish_addr);

        shm = std::make_unique<ResizerSharedMemoryManager>("resizer");
        db = std::make_unique<ResizerBenchmarkDatabase>();

        // Update output dimensions in shared memory
        shm->updateMetric("output_width", config["output_width"].get<int>());
        shm->updateMetric("output_height", config["output_height"].get<int>());
    }

    void processFrame() {
        zmq::message_t msg;
        if (!subscriber.recv(msg, zmq::recv_flags::dontwait)) {
            return; // No message available
        }

        try {
            auto start = steady_clock::now();
            cv::Mat frame = cv::imdecode(cv::Mat(1, msg.size(), CV_8UC1, msg.data()), cv::IMREAD_COLOR);

            if (frame.empty()) {
                frames_dropped++;
                shm->updateMetric("frames_dropped", frames_dropped);
                return;
            }

            // Update input dimensions
            shm->updateMetric("input_width", frame.cols);
            shm->updateMetric("input_height", frame.rows);

            cv::Mat resized;
            cv::Size output_size(config["output_width"].get<int>(), config["output_height"].get<int>());
            cv::resize(frame, resized, output_size);

            // Encode the resized frame
            std::vector<uchar> buffer;
            cv::imencode(".jpg", resized, buffer);

            // Send processed frame
            zmq::message_t out_msg(buffer.data(), buffer.size());
            publisher.send(out_msg, zmq::send_flags::none);

            // Update metrics
            frames_processed++;
            double proc_time = duration_cast<milliseconds>(steady_clock::now() - start).count();

            shm->updateMetric("frames_processed", frames_processed);
            shm->updateMetric("processing_time_ms", proc_time);

            // Calculate FPS every 100 frames
            if (frames_processed % 100 == 0) {
                static auto last_time = steady_clock::now();
                auto now = steady_clock::now();
                double fps = 100000.0 / duration_cast<microseconds>(now - last_time).count();
                shm->updateMetric("current_fps", fps);
                last_time = now;
            }

            // Async export
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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        db->asyncExport(config["benchmark_db_path"].get<std::string>());  // Final export
    }

    void stop() { running = false; }

    ~FrameResizer() {
        stop();
        if (db) db->asyncExport(config["benchmark_db_path"].get<std::string>());
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
