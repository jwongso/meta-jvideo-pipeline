#include <iostream>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <csignal>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <deque>
#include <algorithm>
#include <filesystem>
#include <sqlite3.h>
#include <zmq.hpp>
#include <nlohmann/json.hpp>

#include "shm_services.h"

using json = nlohmann::json;
using namespace std::chrono;

class SystemStats {
private:
    uint64_t last_cpu_total = 0, last_cpu_idle = 0;

public:
    struct Stats {
        double cpu_percent = 0.0;
        double memory_percent = 0.0;
        double memory_mb = 0.0;
    };

    Stats get_stats() {
        Stats stats;

        // CPU usage
        std::ifstream stat_file("/proc/stat");
        std::string line;
        if (std::getline(stat_file, line) && line.substr(0, 4) == "cpu ") {
            std::istringstream iss(line.substr(5));
            std::vector<uint64_t> values(7);
            for (auto& val : values) iss >> val;

            uint64_t total = std::accumulate(values.begin(), values.end(), 0ULL);
            uint64_t idle = values[3];

            if (last_cpu_total > 0) {
                uint64_t total_diff = total - last_cpu_total;
                uint64_t idle_diff = idle - last_cpu_idle;
                if (total_diff > 0) {
                    stats.cpu_percent = 100.0 * (1.0 - static_cast<double>(idle_diff) / total_diff);
                }
            }
            last_cpu_total = total;
            last_cpu_idle = idle;
        }

        // Memory usage
        std::ifstream meminfo("/proc/meminfo");
        std::unordered_map<std::string, uint64_t> mem_data;
        while (std::getline(meminfo, line)) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                uint64_t value;
                std::istringstream(line.substr(pos + 1)) >> value;
                mem_data[key] = value;
            }
        }

        if (mem_data.count("MemTotal") && mem_data.count("MemAvailable")) {
            uint64_t total = mem_data["MemTotal"];
            uint64_t available = mem_data["MemAvailable"];
            stats.memory_mb = total / 1024.0;
            stats.memory_percent = 100.0 * (1.0 - static_cast<double>(available) / total);
        }

        return stats;
    }
};

template<typename T>
class SharedMemoryReader {
private:
    std::string shm_name;
    int fd = -1;
    void* mapped_mem = nullptr;

public:
    SharedMemoryReader(const std::string& name) : shm_name(name) {}

    ~SharedMemoryReader() { close(); }

    bool open() {
        std::string shm_path = "/dev/shm" + shm_name;
        fd = ::open(shm_path.c_str(), O_RDONLY);
        if (fd == -1) return false;

        mapped_mem = mmap(nullptr, sizeof(T), PROT_READ, MAP_SHARED, fd, 0);
        return mapped_mem != MAP_FAILED;
    }

    std::optional<T> read() {
        if (!mapped_mem) return std::nullopt;
        T data;
        std::memcpy(&data, mapped_mem, sizeof(T));
        return data;
    }

    void close() {
        if (mapped_mem && mapped_mem != MAP_FAILED) {
            munmap(mapped_mem, sizeof(T));
            mapped_mem = nullptr;
        }
        if (fd != -1) {
            ::close(fd);
            fd = -1;
        }
    }
};

struct QueueStats {
    uint64_t messages = 0;
    uint64_t bytes = 0;
    time_t last_seen = 0;
    uint32_t errors = 0;
    std::deque<double> latencies;

    void add_latency(double latency) {
        if (latencies.size() >= 100) latencies.pop_front();
        latencies.push_back(latency);
    }

    double avg_latency() const {
        if (latencies.empty()) return 0.0;
        return std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    }
};

struct ServiceMetrics {
    std::string status = "unknown";
    pid_t pid = 0;
    double uptime = 0;
    double current_fps = 0;
    double db_avg_fps = 0;
    uint64_t processed_count = 0;
    uint64_t error_count = 0;
    std::unordered_map<std::string, std::string> extra_info;
};

class DatabaseReader {
private:
    std::unordered_map<std::string, time_t> last_read_times;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> cached_stats;

public:
    std::unordered_map<std::string, double> get_stats(const std::string& service, const std::string& db_path) {
        auto now = time(nullptr);
        if (last_read_times[service] + 60 > now && cached_stats.count(service)) {
            return cached_stats[service];
        }

        std::unordered_map<std::string, double> stats;
        sqlite3* db = nullptr;

        if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            std::string query;
            if (service == "frame-publisher") {
                query = "SELECT AVG(current_fps) as avg_fps, MAX(current_fps) as max_fps, "
                       "COUNT(*) as sample_count FROM publisher_benchmarks WHERE timestamp > ?";
            } else if (service == "frame-resizer") {
                query = "SELECT AVG(current_fps) as avg_fps, AVG(processing_time_ms) as avg_processing_ms, "
                       "COUNT(*) as sample_count FROM resizer_benchmarks WHERE timestamp > ?";
            } else if (service == "frame-saver") {
                query = "SELECT AVG(current_fps) as avg_fps, AVG(save_time_ms) as avg_save_ms, "
                       "COUNT(*) as sample_count FROM saver_benchmarks WHERE timestamp > ?";
            }

            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, now - 600); // Last 10 minutes

                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    int cols = sqlite3_column_count(stmt);
                    for (int i = 0; i < cols; ++i) {
                        const char* name = sqlite3_column_name(stmt, i);
                        double value = sqlite3_column_double(stmt, i);
                        stats[name] = value;
                    }
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }

        last_read_times[service] = now;
        cached_stats[service] = stats;
        return stats;
    }
};

class QueueMonitor {
private:
    std::atomic<bool> running{true};
    SystemStats sys_stats;
    DatabaseReader db_reader;

    // Service readers
    std::unique_ptr<SharedMemoryReader<PublisherSharedMetrics>> publisher_reader;
    std::unique_ptr<SharedMemoryReader<ResizerSharedMetrics>> resizer_reader;
    std::unique_ptr<SharedMemoryReader<SaverSharedMetrics>> saver_reader;

    // ZMQ monitoring
    std::unique_ptr<zmq::context_t> zmq_context;
    std::vector<std::pair<int, std::unique_ptr<zmq::socket_t>>> zmq_sockets;
    std::unordered_map<int, QueueStats> queue_stats;

    // Configuration
    struct Config {
        std::vector<int> monitor_ports = {5555, 5556};
        int update_interval_ms = 100;
        int display_interval_s = 1;
        std::unordered_map<std::string, std::string> db_paths = {
            {"frame-publisher", "/var/log/jvideo/frame_publisher_benchmarks.db"},
            {"frame-resizer", "/var/log/jvideo/frame_resizer_benchmarks.db"},
            {"frame-saver", "/var/log/jvideo/frame_saver_benchmarks.db"}
        };
    } config;

    static QueueMonitor* instance;

public:
    QueueMonitor() {
        instance = this;
        load_config();
        setup_signal_handlers();
        initialize_services();
        initialize_zmq();
    }

    ~QueueMonitor() {
        cleanup();
    }

    static void signal_handler(int signum) {
        if (instance) {
            std::cerr << "\nReceived signal " << signum << ", shutting down...\n";
            instance->running = false;
        }
    }

    void load_config() {
        std::ifstream config_file("/etc/jvideo/queue-monitor.conf");
        if (config_file.is_open()) {
            try {
                json j;
                config_file >> j;
                if (j.contains("monitor_ports")) {
                    config.monitor_ports = j["monitor_ports"].get<std::vector<int>>();
                }
                if (j.contains("update_interval")) {
                    config.update_interval_ms = static_cast<int>(j["update_interval"].get<double>() * 1000);
                }
                if (j.contains("display_interval")) {
                    config.display_interval_s = j["display_interval"];
                }
                std::cerr << "Loaded configuration\n";
            } catch (const std::exception& e) {
                std::cerr << "Config error: " << e.what() << ", using defaults\n";
            }
        }
    }

    void setup_signal_handlers() {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
    }

    void initialize_services() {
        publisher_reader = std::make_unique<SharedMemoryReader<PublisherSharedMetrics>>("/jvideo_publisher_metrics");
        resizer_reader = std::make_unique<SharedMemoryReader<ResizerSharedMetrics>>("/jvideo_resizer_metrics");
        saver_reader = std::make_unique<SharedMemoryReader<SaverSharedMetrics>>("/jvideo_saver_metrics");

        if (publisher_reader->open()) std::cerr << "Connected to frame-publisher\n";
        if (resizer_reader->open()) std::cerr << "Connected to frame-resizer\n";
        if (saver_reader->open()) std::cerr << "Connected to frame-saver\n";
    }

    void initialize_zmq() {
        try {
            zmq_context = std::make_unique<zmq::context_t>(1);

            for (int port : config.monitor_ports) {
                auto socket = std::make_unique<zmq::socket_t>(*zmq_context, ZMQ_SUB);
                socket->set(zmq::sockopt::rcvtimeo, 100);
                socket->set(zmq::sockopt::rcvhwm, 10);
                socket->set(zmq::sockopt::linger, 0);
                socket->connect("tcp://localhost:" + std::to_string(port));
                socket->set(zmq::sockopt::subscribe, "");

                zmq_sockets.emplace_back(port, std::move(socket));
                queue_stats[port] = QueueStats{};
                std::cerr << "Monitoring ZMQ port " << port << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "ZMQ initialization failed: " << e.what() << "\n";
        }
    }

    void check_zmq_queues() {
        for (auto& [port, socket] : zmq_sockets) {
            try {
                zmq::message_t msg;
                while (socket->recv(msg, zmq::recv_flags::dontwait)) {
                    auto& stats = queue_stats[port];
                    stats.messages++;
                    stats.bytes += msg.size();
                    stats.last_seen = time(nullptr);

                    // Try to parse for latency
                    try {
                        std::string data(static_cast<char*>(msg.data()), msg.size());
                        auto j = json::parse(data);
                        if (j.contains("timestamp")) {
                            double timestamp = j["timestamp"];
                            double latency = (duration_cast<milliseconds>(
                                system_clock::now().time_since_epoch()).count() / 1000.0 - timestamp) * 1000;
                            if (latency >= 0 && latency <= 10000) {
                                stats.add_latency(latency);
                            }
                        }
                    } catch (...) {}
                }
            } catch (const zmq::error_t& e) {
                if (e.num() != EAGAIN) {
                    queue_stats[port].errors++;
                }
            }
        }
    }

    ServiceMetrics get_service_metrics(const std::string& service_name) {
        ServiceMetrics metrics;
        auto now = time(nullptr);

        if (service_name == "frame-publisher") {
            auto data = publisher_reader->read();
            if (data) {
                metrics.pid = data->service_pid;
                metrics.status = (metrics.pid > 0 && std::filesystem::exists("/proc/" + std::to_string(metrics.pid))) ? "running" : "stopped";
                metrics.uptime = (metrics.status == "running") ? (now - data->service_start_time) : 0;
                metrics.current_fps = data->current_fps;
                metrics.processed_count = data->frames_published;
                metrics.error_count = data->errors;
                metrics.extra_info["video_path"] = std::string(data->video_path).substr(0, strlen(data->video_path));
                metrics.extra_info["resolution"] = std::to_string(data->video_width) + "x" + std::to_string(data->video_height);
                metrics.extra_info["video_fps"] = std::to_string(data->video_fps);
            }
        } else if (service_name == "frame-resizer") {
            auto data = resizer_reader->read();
            if (data) {
                metrics.pid = data->service_pid;
                metrics.status = (metrics.pid > 0 && std::filesystem::exists("/proc/" + std::to_string(metrics.pid))) ? "running" : "stopped";
                metrics.uptime = (metrics.status == "running") ? (now - data->service_start_time) : 0;
                metrics.current_fps = data->current_fps;
                metrics.processed_count = data->frames_processed;
                metrics.error_count = data->errors;
                metrics.extra_info["dropped"] = std::to_string(data->frames_dropped);
                metrics.extra_info["processing_time"] = std::to_string(data->processing_time_ms);
                metrics.extra_info["input_res"] = std::to_string(data->input_width) + "x" + std::to_string(data->input_height);
                metrics.extra_info["output_res"] = std::to_string(data->output_width) + "x" + std::to_string(data->output_height);
            }
        } else if (service_name == "frame-saver") {
            auto data = saver_reader->read();
            if (data) {
                metrics.pid = data->service_pid;
                metrics.status = (metrics.pid > 0 && std::filesystem::exists("/proc/" + std::to_string(metrics.pid))) ? "running" : "stopped";
                metrics.uptime = (metrics.status == "running") ? (now - data->service_start_time) : 0;
                metrics.current_fps = data->current_fps;
                metrics.processed_count = data->frames_saved;
                metrics.error_count = data->errors;
                metrics.extra_info["dropped"] = std::to_string(data->frames_dropped);
                metrics.extra_info["io_errors"] = std::to_string(data->io_errors);
                metrics.extra_info["save_time"] = std::to_string(data->save_time_ms);
                metrics.extra_info["disk_usage"] = std::to_string(data->disk_usage_mb);
                metrics.extra_info["output_dir"] = std::string(data->output_dir).substr(0, strlen(data->output_dir));
            }
        }

        // Get database stats
        if (config.db_paths.count(service_name)) {
            auto db_stats = db_reader.get_stats(service_name, config.db_paths[service_name]);
            if (db_stats.count("avg_fps")) {
                metrics.db_avg_fps = db_stats["avg_fps"];
            }
        }

        return metrics;
    }

    void print_status() {
        system("clear 2>/dev/null || cls 2>/dev/null");

        auto sys = sys_stats.get_stats();
        auto now = system_clock::now();
        auto time_t_now = system_clock::to_time_t(now);

        std::cout << std::string(80, '=') << "\n";
        std::cout << "JVideo Pipeline Monitor - " << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S") << "\n";
        std::cout << "System: CPU " << std::fixed << std::setprecision(1) << sys.cpu_percent << "% | "
                  << "Memory " << sys.memory_percent << "% (" << sys.memory_mb << " MB)\n";
        std::cout << std::string(80, '=') << "\n";

        // Queue Statistics
        if (!zmq_sockets.empty()) {
            std::cout << "\nQueue Statistics:\n" << std::string(80, '-') << "\n";
            for (const auto& [port, stats] : queue_stats) {
                std::string age = "Never";
                if (stats.last_seen > 0) {
                    age = std::to_string(time(nullptr) - stats.last_seen) + "s ago";
                }

                std::cout << "Port " << port << ": " << std::setw(8) << stats.messages << " msgs | "
                          << std::setw(6) << std::setprecision(1) << (stats.bytes / 1024.0 / 1024.0) << " MB | "
                          << "Latency: " << std::setw(5) << std::setprecision(1) << stats.avg_latency() << " ms | "
                          << "Last: " << std::setw(12) << age << " | "
                          << "Errors: " << stats.errors << "\n";
            }
        }

        // Service Statistics
        std::cout << "\nService Status:\n" << std::string(80, '-') << "\n";

        for (const std::string& service : {"frame-publisher", "frame-resizer", "frame-saver"}) {
            auto metrics = get_service_metrics(service);

            std::cout << "\n" << service << " [" << metrics.status << "]";
            if (metrics.status == "running") {
                std::cout << " PID: " << metrics.pid << " | Uptime: " << (metrics.uptime / 60) << " min\n";
                std::cout << "  Current FPS: " << std::setprecision(1) << metrics.current_fps
                          << " | Avg FPS (10m): " << metrics.db_avg_fps << "\n";

                if (service == "frame-publisher") {
                    std::cout << "  Published: " << metrics.processed_count << " frames | Errors: " << metrics.error_count << "\n";
                    std::cout << "  Video: " << std::filesystem::path(metrics.extra_info["video_path"]).filename().string() << "\n";
                    std::cout << "  Resolution: " << metrics.extra_info["resolution"] << " @ " << metrics.extra_info["video_fps"] << " fps\n";
                } else if (service == "frame-resizer") {
                    std::cout << "  Processed: " << metrics.processed_count << " | Dropped: " << metrics.extra_info["dropped"] << " | Errors: " << metrics.error_count << "\n";
                    std::cout << "  Processing time: " << metrics.extra_info["processing_time"] << " ms\n";
                    std::cout << "  Input: " << metrics.extra_info["input_res"] << " -> Output: " << metrics.extra_info["output_res"] << "\n";
                } else if (service == "frame-saver") {
                    std::cout << "  Saved: " << metrics.processed_count << " | Dropped: " << metrics.extra_info["dropped"] << " | IO Errors: " << metrics.extra_info["io_errors"] << "\n";
                    std::cout << "  Save time: " << metrics.extra_info["save_time"] << " ms | Disk usage: " << metrics.extra_info["disk_usage"] << " MB\n";
                    std::cout << "  Output: " << metrics.extra_info["output_dir"] << "\n";
                }
            } else {
                std::cout << "\n";
            }
        }

        std::cout << "\n" << std::string(80, '=') << "\n";
        std::cout << "Press Ctrl+C to exit\n";
    }

    void run() {
        std::cerr << "Starting monitoring loop...\n";

        auto last_display = steady_clock::now();

        while (running) {
            check_zmq_queues();

            auto now = steady_clock::now();
            if (duration_cast<seconds>(now - last_display).count() >= config.display_interval_s) {
                print_status();
                last_display = now;
            }

            std::this_thread::sleep_for(milliseconds(config.update_interval_ms));
        }

        cleanup();
    }

    void cleanup() {
        std::cerr << "\nCleaning up...\n";
        zmq_sockets.clear();
        zmq_context.reset();
        std::cerr << "Shutdown complete\n";
    }
};

QueueMonitor* QueueMonitor::instance = nullptr;

int main() {
    try {
        QueueMonitor monitor;
        monitor.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
