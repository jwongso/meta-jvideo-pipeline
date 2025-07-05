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
// Replace Redis includes with shared memory and SQLite
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

using json = nlohmann::json;
using namespace std::chrono;

// Global flag for graceful shutdown
volatile sig_atomic_t g_running = 1;

// Signal handler function
void signal_handler(int sig) {
    g_running = 0;
}

/**
 * Shared Memory Metrics Structure
 * This structure is mapped to shared memory and contains real-time metrics
 * that can be accessed by monitoring tools or other services without IPC overhead
 */
struct SharedMetrics {
    // Service identification
    char service_name[64];
    pid_t service_pid;

    // Video properties (set once during initialization)
    int total_frames;
    double video_fps;
    int video_width;
    int video_height;

    // Real-time counters (updated frequently)
    volatile int frames_published;
    volatile int errors;
    volatile double current_fps;

    // Timestamps for monitoring service health
    volatile time_t last_update_time;
    volatile time_t service_start_time;

    // Memory barrier and synchronization
    volatile int update_counter;  // Incremented on each update for change detection

    // Reserved space for future metrics without breaking compatibility
    char reserved[256];
};

/**
 * Shared Memory Manager
 * Handles creation, mapping, and cleanup of POSIX shared memory segment
 * for real-time metrics sharing between processes
 */
class SharedMemoryManager {
private:
    static constexpr const char* SHM_NAME = "/mp4_frame_publisher_metrics";
    static constexpr size_t SHM_SIZE = sizeof(SharedMetrics);

    int shm_fd;                    // File descriptor for shared memory
    SharedMetrics* metrics_ptr;    // Pointer to mapped shared memory
    bool is_owner;                 // Whether this process created the shared memory

public:
    SharedMemoryManager(const std::string& service_name)
        : shm_fd(-1), metrics_ptr(nullptr), is_owner(false) {

        // Try to create new shared memory segment first
        shm_fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);

        if (shm_fd != -1) {
            // Successfully created new segment - we are the owner
            is_owner = true;
            std::cout << "[SHM] Created new shared memory segment\n";

            // Set the size of the shared memory segment
            if (ftruncate(shm_fd, SHM_SIZE) == -1) {
                std::cerr << "[SHM] Failed to set shared memory size: " << strerror(errno) << std::endl;
                close(shm_fd);
                shm_fd = -1;
                return;
            }
        } else if (errno == EEXIST) {
            // Segment already exists, try to open it
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (shm_fd != -1) {
                std::cout << "[SHM] Opened existing shared memory segment\n";
            } else {
                std::cerr << "[SHM] Failed to open existing shared memory: " << strerror(errno) << std::endl;
                return;
            }
        } else {
            std::cerr << "[SHM] Failed to create shared memory: " << strerror(errno) << std::endl;
            return;
        }

        // Map the shared memory into our address space
        metrics_ptr = static_cast<SharedMetrics*>(
            mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
        );

        if (metrics_ptr == MAP_FAILED) {
            std::cerr << "[SHM] Failed to map shared memory: " << strerror(errno) << std::endl;
            close(shm_fd);
            shm_fd = -1;
            metrics_ptr = nullptr;
            return;
        }

        // Initialize the metrics structure if we created it
        if (is_owner) {
            std::memset(metrics_ptr, 0, sizeof(SharedMetrics));
            strncpy(metrics_ptr->service_name, service_name.c_str(), sizeof(metrics_ptr->service_name) - 1);
            metrics_ptr->service_pid = getpid();
            metrics_ptr->service_start_time = time(nullptr);
            metrics_ptr->last_update_time = time(nullptr);
        }

        std::cout << "[SHM] Shared memory initialized successfully\n";
    }

    /**
     * Update real-time metrics in shared memory
     * These updates are atomic for basic types and very fast
     */
    void updateMetric(const std::string& key, int value) {
        if (!metrics_ptr) return;

        if (key == "frames_published") {
            metrics_ptr->frames_published = value;
        } else if (key == "errors") {
            metrics_ptr->errors = value;
        } else if (key == "total_frames") {
            metrics_ptr->total_frames = value;
        } else if (key == "video_width") {
            metrics_ptr->video_width = value;
        } else if (key == "video_height") {
            metrics_ptr->video_height = value;
        }

        // Update synchronization fields
        metrics_ptr->last_update_time = time(nullptr);
        __sync_fetch_and_add(&metrics_ptr->update_counter, 1);
    }

    void updateMetric(const std::string& key, double value) {
        if (!metrics_ptr) return;

        if (key == "current_fps") {
            metrics_ptr->current_fps = value;
        } else if (key == "video_fps") {
            metrics_ptr->video_fps = value;
        }

        // Update synchronization fields
        metrics_ptr->last_update_time = time(nullptr);
        __sync_fetch_and_add(&metrics_ptr->update_counter, 1);
    }

    /**
     * Get current metrics snapshot for benchmark storage
     */
    SharedMetrics getMetricsSnapshot() const {
        if (!metrics_ptr) {
            return SharedMetrics{};
        }
        return *metrics_ptr;
    }

    ~SharedMemoryManager() {
        if (metrics_ptr && metrics_ptr != MAP_FAILED) {
            munmap(metrics_ptr, SHM_SIZE);
        }

        if (shm_fd != -1) {
            close(shm_fd);
        }

        // Only unlink if we created the segment and we're shutting down cleanly
        if (is_owner && !g_running) {
            shm_unlink(SHM_NAME);
            std::cout << "[SHM] Cleaned up shared memory segment\n";
        }
    }
};

/**
 * SQLite Benchmark Database Manager
 * Handles periodic storage of performance benchmarks and historical data
 * Uses in-memory database for speed while maintaining persistence through dumps
 */
class BenchmarkDatabase {
private:
    sqlite3* db;
    std::mutex db_mutex;  // Protect database operations
    std::string service_name;

    /**
     * Initialize database schema for benchmark storage
     */
    bool initializeSchema() {
        const char* create_table_sql = R"(
            CREATE TABLE IF NOT EXISTS performance_benchmarks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp INTEGER NOT NULL,
                service_name TEXT NOT NULL,
                frames_published INTEGER,
                total_frames INTEGER,
                current_fps REAL,
                video_fps REAL,
                errors INTEGER,
                uptime_seconds INTEGER,
                memory_usage_kb INTEGER,
                cpu_usage_percent REAL
            );

            CREATE INDEX IF NOT EXISTS idx_timestamp ON performance_benchmarks(timestamp);
            CREATE INDEX IF NOT EXISTS idx_service ON performance_benchmarks(service_name);
        )";

        char* error_msg = nullptr;
        int result = sqlite3_exec(db, create_table_sql, nullptr, nullptr, &error_msg);

        if (result != SQLITE_OK) {
            std::cerr << "[SQLite] Failed to create schema: " << error_msg << std::endl;
            sqlite3_free(error_msg);
            return false;
        }

        std::cout << "[SQLite] Database schema initialized\n";
        return true;
    }

    /**
     * Get current process memory usage in KB
     */
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
        return -1;  // Unable to determine
    }

    /**
     * Get current process CPU usage percentage (simplified)
     */
    double getCpuUsage() const {
        // For embedded systems, we'll use a simple approach
        // In production, you might want to implement more sophisticated CPU monitoring
        static auto last_time = steady_clock::now();
        static clock_t last_cpu = clock();

        auto current_time = steady_clock::now();
        clock_t current_cpu = clock();

        double time_diff = duration<double>(current_time - last_time).count();
        double cpu_diff = static_cast<double>(current_cpu - last_cpu) / CLOCKS_PER_SEC;

        last_time = current_time;
        last_cpu = current_cpu;

        return (time_diff > 0) ? (cpu_diff / time_diff) * 100.0 : 0.0;
    }

public:
    BenchmarkDatabase(const std::string& name) : db(nullptr), service_name(name) {
        // Open in-memory database for maximum performance
        // Data will be periodically dumped to persistent storage if needed
        int result = sqlite3_open(":memory:", &db);

        if (result != SQLITE_OK) {
            std::cerr << "[SQLite] Failed to open database: " << sqlite3_errmsg(db) << std::endl;
            db = nullptr;
            return;
        }

        // Configure SQLite for optimal performance in embedded environment
        sqlite3_exec(db, "PRAGMA synchronous = OFF", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA temp_store = MEMORY", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA cache_size = 10000", nullptr, nullptr, nullptr);

        if (!initializeSchema()) {
            sqlite3_close(db);
            db = nullptr;
        } else {
            std::cout << "[SQLite] In-memory benchmark database initialized\n";
        }
    }

    /**
     * Store a benchmark snapshot with comprehensive system metrics
     */
    void storeBenchmark(const SharedMetrics& metrics) {
        if (!db) return;

        std::lock_guard<std::mutex> lock(db_mutex);

        const char* insert_sql = R"(
            INSERT INTO performance_benchmarks
            (timestamp, service_name, frames_published, total_frames, current_fps,
             video_fps, errors, uptime_seconds, memory_usage_kb, cpu_usage_percent)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt* stmt;
        int result = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);

        if (result != SQLITE_OK) {
            std::cerr << "[SQLite] Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return;
        }

        // Calculate uptime
        time_t current_time = time(nullptr);
        int uptime = static_cast<int>(current_time - metrics.service_start_time);

        // Bind parameters
        sqlite3_bind_int64(stmt, 1, current_time);
        sqlite3_bind_text(stmt, 2, service_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, metrics.frames_published);
        sqlite3_bind_int(stmt, 4, metrics.total_frames);
        sqlite3_bind_double(stmt, 5, metrics.current_fps);
        sqlite3_bind_double(stmt, 6, metrics.video_fps);
        sqlite3_bind_int(stmt, 7, metrics.errors);
        sqlite3_bind_int(stmt, 8, uptime);
        sqlite3_bind_int64(stmt, 9, getMemoryUsage());
        sqlite3_bind_double(stmt, 10, getCpuUsage());

        result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (result != SQLITE_DONE) {
            std::cerr << "[SQLite] Failed to insert benchmark: " << sqlite3_errmsg(db) << std::endl;
        }
    }

    /**
     * Export benchmark data to persistent storage (optional)
     * This can be called periodically or on shutdown to preserve historical data
     */
    void exportToFile(const std::string& filepath) {
        if (!db) return;

        std::lock_guard<std::mutex> lock(db_mutex);

        // Open file database for export
        sqlite3* file_db;
        int result = sqlite3_open(filepath.c_str(), &file_db);

        if (result != SQLITE_OK) {
            std::cerr << "[SQLite] Failed to open export file: " << sqlite3_errmsg(file_db) << std::endl;
            return;
        }

        // Use SQLite backup API to copy in-memory database to file
        sqlite3_backup* backup = sqlite3_backup_init(file_db, "main", db, "main");

        if (backup) {
            sqlite3_backup_step(backup, -1);
            sqlite3_backup_finish(backup);
            std::cout << "[SQLite] Benchmark data exported to " << filepath << std::endl;
        } else {
            std::cerr << "[SQLite] Failed to initialize backup\n";
        }

        sqlite3_close(file_db);
    }

    /**
     * Get recent performance statistics for monitoring
     */
    void printRecentStats(int last_n_records = 10) {
        if (!db) return;

        std::lock_guard<std::mutex> lock(db_mutex);

        const char* query_sql = R"(
            SELECT timestamp, frames_published, current_fps, errors, memory_usage_kb, cpu_usage_percent
            FROM performance_benchmarks
            WHERE service_name = ?
            ORDER BY timestamp DESC
            LIMIT ?
        )";

        sqlite3_stmt* stmt;
        int result = sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr);

        if (result != SQLITE_OK) {
            std::cerr << "[SQLite] Failed to prepare query: " << sqlite3_errmsg(db) << std::endl;
            return;
        }

        sqlite3_bind_text(stmt, 1, service_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, last_n_records);

        std::cout << "[Benchmark] Recent performance data:\n";
        std::cout << "Timestamp\t\tFrames\tFPS\tErrors\tMem(KB)\tCPU%\n";
        std::cout << "--------------------------------------------------------\n";

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            time_t timestamp = sqlite3_column_int64(stmt, 0);
            int frames = sqlite3_column_int(stmt, 1);
            double fps = sqlite3_column_double(stmt, 2);
            int errors = sqlite3_column_int(stmt, 3);
            long memory = sqlite3_column_int64(stmt, 4);
            double cpu = sqlite3_column_double(stmt, 5);

            char time_str[32];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&timestamp));

            std::cout << time_str << "\t\t" << frames << "\t"
                      << std::fixed << std::setprecision(1) << fps << "\t"
                      << errors << "\t" << memory << "\t"
                      << std::setprecision(1) << cpu << "\n";
        }

        sqlite3_finalize(stmt);
    }

    ~BenchmarkDatabase() {
        if (db) {
            // Optionally export data before closing
            // exportToFile("/tmp/frame_publisher_benchmarks.db");
            sqlite3_close(db);
            std::cout << "[SQLite] Database closed\n";
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

    // Replace Redis with shared memory and SQLite
    std::unique_ptr<SharedMemoryManager> shm_manager;
    std::unique_ptr<BenchmarkDatabase> benchmark_db;

    json config;

    // Metrics
    int frames_published = 0;
    int errors = 0;
    steady_clock::time_point start_time;
    steady_clock::time_point last_benchmark_time;

    void loadConfig() {
        // Default configuration
        config = {
            {"video_input_path", DEFAULT_VIDEO_PATH},
            {"publish_port", 5555},
            {"benchmark_db_path", "/tmp/frame_publisher_benchmarks.db"},
            {"benchmark_export_interval", 100}  // Export every 100 frames
        };

        // Try to load from config file
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

                // Only merge the keys we care about
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
    MP4FramePublisher()
        : zmq_context(1),
          zmq_socket(zmq_context, ZMQ_PUB) {

        start_time = steady_clock::now();
        last_benchmark_time = start_time;

        // Initialize shared memory and benchmark database
        shm_manager = std::make_unique<SharedMemoryManager>("mp4-frame-publisher");
        benchmark_db = std::make_unique<BenchmarkDatabase>("mp4-frame-publisher");

        // Load configuration
        loadConfig();

        // Setup ZMQ socket options
        zmq_socket.set(zmq::sockopt::sndhwm, 10);

        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // Bind ZMQ socket with configured port
        int port = config["publish_port"];
        std::string bind_addr = "tcp://*:" + std::to_string(port);
        zmq_socket.bind(bind_addr);
        std::cout << "[Publisher] ZMQ socket bound to " << bind_addr << std::endl;

        // Give subscribers time to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    bool openVideo(const std::string& video_path = "") {
        // Determine video path: command line > config file > default
        std::string final_path;
        if (!video_path.empty()) {
            final_path = video_path;
            std::cout << "[Publisher] Using command line path: " << final_path << std::endl;
        } else {
            final_path = config["video_input_path"];
            std::cout << "[Publisher] Using configured path: " << final_path << std::endl;
        }

        // Debug: Check file accessibility
        std::ifstream test_file(final_path);
        if (!test_file.good()) {
            std::cerr << "[Publisher] File not accessible: " << final_path << std::endl;
            return false;
        }
        test_file.close();
        std::cout << "[Publisher] File is accessible" << std::endl;

        // Try different backends (compatible with older OpenCV versions)
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

                // Get video properties
                int frame_count = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_COUNT));
                double fps = video_cap.get(cv::CAP_PROP_FPS);
                int width = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_WIDTH));
                int height = static_cast<int>(video_cap.get(cv::CAP_PROP_FRAME_HEIGHT));

                std::cout << "[Publisher] Video opened: " << width << "x" << height
                        << ", " << frame_count << " frames @ " << fps << " fps\n";

                // Store video info in shared memory
                shm_manager->updateMetric("total_frames", frame_count);
                shm_manager->updateMetric("video_fps", fps);
                shm_manager->updateMetric("video_width", width);
                shm_manager->updateMetric("video_height", height);

                return true;
            } else {
                std::cout << "[Publisher] Failed with backend: " << backend_name << std::endl;
            }
        }

        std::cerr << "[Publisher] Failed to open video with any backend: " << final_path << std::endl;
        return false;
    }

    void publishFrames() {
        if (!video_cap.isOpened()) {
            std::cerr << "[Publisher] Video not opened\n";
            return;
        }

        cv::Mat frame;
        int frame_id = 0;
        int export_interval = config["benchmark_export_interval"];

        std::cout << "[Publisher] Starting frame publishing...\n";

        while (g_running && video_cap.read(frame)) {
            if (frame.empty()) {
                std::cout << "[Publisher] Empty frame, skipping\n";
                continue;
            }

            try {
                // Create metadata
                json metadata;
                metadata["frame_id"] = frame_id;
                metadata["timestamp"] = duration<double>(steady_clock::now().time_since_epoch()).count();
                metadata["width"] = frame.cols;
                metadata["height"] = frame.rows;
                metadata["channels"] = frame.channels();
                metadata["source"] = "mp4";

                // Send metadata first
                std::string meta_str = metadata.dump();
                zmq::message_t meta_msg(meta_str.data(), meta_str.size());
                zmq_socket.send(meta_msg, zmq::send_flags::sndmore);

                // Send frame data
                size_t frame_size = frame.total() * frame.elemSize();
                zmq::message_t frame_msg(frame_size);
                std::memcpy(frame_msg.data(), frame.data, frame_size);
                zmq_socket.send(frame_msg, zmq::send_flags::none);

                frames_published++;
                frame_id++;

                // Update real-time metrics in shared memory every 10 frames (very frequent)
                if (frames_published % 10 == 0) {
                    auto now = steady_clock::now();
                    double elapsed = duration<double>(now - start_time).count();
                    double fps = frames_published / elapsed;

                    shm_manager->updateMetric("frames_published", frames_published);
                    shm_manager->updateMetric("current_fps", fps);
                    shm_manager->updateMetric("errors", errors);
                }

                // Store benchmark data every 1000 frames (less frequent, more comprehensive)
                if (frames_published % 1000 == 0) {
                    auto metrics_snapshot = shm_manager->getMetricsSnapshot();
                    benchmark_db->storeBenchmark(metrics_snapshot);

                    std::cout << "[Publisher] Published " << frames_published
                              << " frames, FPS: " << std::fixed << std::setprecision(1)
                              << metrics_snapshot.current_fps << "\n";
                }

                // Export to persistent DB file periodically
                if (frames_published % export_interval == 0) {
                    std::string db_path = config["benchmark_db_path"];
                    benchmark_db->exportToFile(db_path);
                    std::cout << "[Publisher] Exported benchmark data to " << db_path << std::endl;
                }

                // Print recent benchmark stats every 5000 frames
                if (frames_published % 5000 == 0) {
                    benchmark_db->printRecentStats(5);
                }

                // Small delay to prevent overwhelming subscribers
                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps

            } catch (const zmq::error_t& e) {
                std::cerr << "[Publisher] ZMQ error: " << e.what() << std::endl;
                errors++;
                shm_manager->updateMetric("errors", errors);
            } catch (const std::exception& e) {
                std::cerr << "[Publisher] Error: " << e.what() << std::endl;
                errors++;
                shm_manager->updateMetric("errors", errors);
            }
        }

        // Final metrics update and benchmark storage
        auto total_time = duration<double>(steady_clock::now() - start_time).count();
        shm_manager->updateMetric("frames_published", frames_published);
        shm_manager->updateMetric("errors", errors);

        // Store final benchmark
        auto final_metrics = shm_manager->getMetricsSnapshot();
        benchmark_db->storeBenchmark(final_metrics);

        // Final export to persistent storage
        std::string db_path = config["benchmark_db_path"];
        benchmark_db->exportToFile(db_path);
        std::cout << "[Publisher] Final export to " << db_path << std::endl;

        // Print final statistics
        std::cout << "[Publisher] Finished. Published " << frames_published
                  << " frames in " << std::fixed << std::setprecision(1) << total_time << " seconds\n";

        benchmark_db->printRecentStats(10);
    }

    ~MP4FramePublisher() {
        if (video_cap.isOpened()) {
            video_cap.release();
        }
        if (benchmark_db) {
            benchmark_db->exportToFile(config["benchmark_db_path"]);
        }
    }
};

int main(int argc, char* argv[]) {
    std::cout << "[Publisher] MP4 Frame Publisher starting...\n";

    try {
        MP4FramePublisher publisher;

        // Use command line argument for video path if provided
        std::string video_path = (argc > 1) ? argv[1] : "";

        if (!publisher.openVideo(video_path)) {
            std::cerr << "[Publisher] Failed to open video file\n";
            return 1;
        }

        publisher.publishFrames();

    } catch (const std::exception& e) {
        std::cerr << "[Publisher] Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Publisher] Shutdown complete\n";
    return 0;
}
