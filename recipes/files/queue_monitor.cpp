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
#include <deque>
#include <algorithm>
#include <filesystem>
#include <sqlite3.h>
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <numeric>
#include <systemd/sd-daemon.h>

#include "metrics_interface.h"

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

    void reset_counters() {
        if (messages > 1000000) messages = 0;
        if (bytes > 1024 * 1024 * 1024) bytes = 0;
        if (errors > 10000) errors = 0;
    }
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

        if (!std::filesystem::exists(db_path)) {
            stats["db_status"] = 0; // no_file
            return stats;
        }

        if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            std::string query;
            if (service == "frame-publisher") {
                query = "SELECT AVG(current_fps) as avg_fps, MAX(current_fps) as max_fps, "
                       "MIN(current_fps) as min_fps, COUNT(*) as sample_count, "
                       "AVG(memory_usage_kb) as avg_memory_kb, MAX(errors) as max_errors "
                       "FROM publisher_benchmarks WHERE timestamp > ?";
            } else if (service == "frame-resizer") {
                query = "SELECT AVG(current_fps) as avg_fps, MAX(current_fps) as max_fps, "
                       "MIN(current_fps) as min_fps, AVG(processing_time_ms) as avg_processing_ms, "
                       "COUNT(*) as sample_count, MAX(errors) as max_errors "
                       "FROM resizer_benchmarks WHERE timestamp > ?";
            } else if (service == "frame-saver") {
                query = "SELECT AVG(current_fps) as avg_fps, MAX(current_fps) as max_fps, "
                       "MIN(current_fps) as min_fps, AVG(save_time_ms) as avg_save_ms, "
                       "MAX(disk_usage_mb) as max_disk_mb, COUNT(*) as sample_count, "
                       "MAX(io_errors) as max_io_errors "
                       "FROM saver_benchmarks WHERE timestamp > ?";
            }

            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, now - 600); // Last 10 minutes

                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    int cols = sqlite3_column_count(stmt);
                    for (int i = 0; i < cols; ++i) {
                        const char* name = sqlite3_column_name(stmt, i);
                        double value = sqlite3_column_double(stmt, i);
                        stats[std::string("db_") + name] = value;
                    }
                    stats["db_status"] = 1; // ok
                } else {
                    stats["db_status"] = 2; // no_data
                }
                sqlite3_finalize(stmt);
            } else {
                stats["db_status"] = -1; // error
            }
            sqlite3_close(db);
        } else {
            stats["db_status"] = -2; // unavailable
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
    uint64_t stats_counter = 0;

    // Service readers using new metrics interface
    std::unique_ptr<PublisherMetricsManager> publisher_reader;
    std::unique_ptr<ResizerMetricsManager> resizer_reader;
    std::unique_ptr<SaverMetricsManager> saver_reader;

    // ZMQ monitoring
    std::unique_ptr<zmq::context_t> zmq_context;
    std::vector<std::pair<int, std::unique_ptr<zmq::socket_t>>> zmq_sockets;
    std::unordered_map<int, QueueStats> queue_stats;

    // Configuration
    struct Config {
        std::vector<int> monitor_ports = {5555, 5556};
        int update_interval_ms = 1000;
        int display_interval_s = 5;
        int db_read_interval_s = 60;
        std::unordered_map<std::string, std::string> db_paths = {
            {"frame-publisher", "/var/lib/jvideo/db/publisher_benchmarks.db"},
            {"frame-resizer", "/var/lib/jvideo/db/resizer_benchmarks.db"},
            {"frame-saver", "/var/lib/jvideo/db/saver_benchmarks.db"}
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
                if (j.contains("db_read_interval")) {
                    config.db_read_interval_s = j["db_read_interval"];
                }
                if (j.contains("service_db_paths")) {
                    for (auto& [key, value] : j["service_db_paths"].items()) {
                        config.db_paths[key] = value;
                    }
                }
                std::cerr << "[Monitor] Loaded configuration\n";
            } catch (const std::exception& e) {
                std::cerr << "[Monitor] Config error: " << e.what() << ", using defaults\n";
            }
        }
    }

    void setup_signal_handlers() {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
    }

    void initialize_services() {
        std::cerr << "[Monitor] Attempting to connect to services...\n";

        try {
            std::cerr << "[Monitor] Trying to open: jvideo_frame-publisher_metrics\n";
            publisher_reader = std::make_unique<PublisherMetricsManager>("frame-publisher", true);
            std::cerr << "[Monitor] Connected to frame-publisher shared memory\n";
        } catch (const std::exception& e) {
            std::cerr << "[Monitor] Failed to connect to frame-publisher: " << e.what() << "\n";
        }

        try {
            std::cerr << "[Monitor] Trying to open: jvideo_frame-resizer_metrics\n";
            resizer_reader = std::make_unique<ResizerMetricsManager>("frame-resizer", true);
            std::cerr << "[Monitor] Connected to frame-resizer shared memory\n";
        } catch (const std::exception& e) {
            std::cerr << "[Monitor] Failed to connect to frame-resizer: " << e.what() << "\n";
        }

        try {
            std::cerr << "[Monitor] Trying to open: jvideo_frame-saver_metrics\n";
            saver_reader = std::make_unique<SaverMetricsManager>("frame-saver", true);
            std::cerr << "[Monitor] Connected to frame-saver shared memory\n";
        } catch (const std::exception& e) {
            std::cerr << "[Monitor] Failed to connect to frame-saver: " << e.what() << "\n";
        }
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
                std::cerr << "[Monitor] Monitoring ZMQ port " << port << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Monitor] ZMQ initialization failed: " << e.what() << "\n";
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
            std::cout << "\nQueue Statistics:\n" << std::string(80,  '-') << "\n";
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

        print_service_status("frame-publisher");
        print_service_status("frame-resizer");
        print_service_status("frame-saver");

        std::cout << "\n" << std::string(80, '=') << "\n";
        std::cout << "Press Ctrl+C to exit\n";
    }

    void print_service_status(const std::string& service) {
        auto now = time(nullptr);

        if (service == "frame-publisher" && publisher_reader) {
            try {
                auto data = publisher_reader->getMetrics();

                std::cerr << "[DEBUG] Publisher PID from metrics: " << data.service_pid << std::endl;
                std::cerr << "[DEBUG] Publisher last update: " << (time(nullptr) - data.last_update_time / 1000000000) << " seconds ago" << std::endl;

                bool is_running = data.service_pid > 0 &&
                                std::filesystem::exists("/proc/" + std::to_string(data.service_pid));

                std::cout << "\n" << service << " [" << (is_running ? "running" : "stopped") << "]";

                if (is_running) {
                    double uptime = (now - data.service_start_time / 1000000000) / 60.0;

                    auto db_stats = db_reader.get_stats(service, config.db_paths[service]);
                    double db_avg_fps = db_stats.count("db_avg_fps") ? db_stats["db_avg_fps"] : 0.0;

                    std::cout << " PID: " << data.service_pid << " | Uptime: "
                            << std::fixed << std::setprecision(0) << uptime << " min\n";
                    std::cout << "  Current FPS: " << std::setprecision(1) << data.current_fps
                            << " | Avg FPS (10m): " << db_avg_fps << "\n";

                    if (data.total_frames == 0) {
                        std::cout << "  Published: " << data.frames_published << " frames";
                    } else {
                        std::cout << "  Published: " << data.frames_published << "/" << data.total_frames << " frames";
                    }
                    std::cout << " | Errors: " << data.errors << "\n";

                    if (!data.video_path.empty() && data.video_path != "FILE_NOT_FOUND" && data.video_path != "OPEN_FAILED") {
                        std::cout << "  Video: " << std::filesystem::path(data.video_path).filename().string() << "\n";
                    }
                    std::cout << "  Resolution: " << data.video_width << "x" << data.video_height
                            << " @ " << std::setprecision(1) << data.video_fps << " fps\n";

                    // Database status
                    double db_status = db_stats.count("db_status") ? db_stats["db_status"] : -1;
                    if (db_status == 1) {
                        double samples = db_stats.count("db_sample_count") ? db_stats["db_sample_count"] : 0;
                        std::cout << "  DB: " << std::fixed << std::setprecision(0) << samples
                                << " samples in last 10 min\n";
                    } else {
                        std::cout << "  DB: " << (db_status == 0 ? "no_file" : "unavailable") << "\n";
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "\n" << service << " [error reading metrics]\n";
            }
        }
        else if (service == "frame-resizer" && resizer_reader) {
            try {
                auto data = resizer_reader->getMetrics();

                bool is_running = data.service_pid > 0 &&
                                std::filesystem::exists("/proc/" + std::to_string(data.service_pid));

                std::cout << "\n" << service << " [" << (is_running ? "running" : "stopped") << "]";

                if (is_running) {
                    double uptime = (now - data.service_start_time / 1000000000) / 60.0;

                    auto db_stats = db_reader.get_stats(service, config.db_paths[service]);
                    double db_avg_fps = db_stats.count("db_avg_fps") ? db_stats["db_avg_fps"] : 0.0;

                    std::cout << " PID: " << data.service_pid << " | Uptime: "
                            << std::fixed << std::setprecision(0) << uptime << " min\n";
                    std::cout << "  Current FPS: " << std::setprecision(1) << data.current_fps
                            << " | Avg FPS (10m): " << db_avg_fps << "\n";
                    std::cout << "  Processed: " << data.frames_processed
                            << " | Dropped: " << data.frames_dropped
                            << " | Errors: " << data.errors << "\n";
                    std::cout << "  Processing time: " << data.processing_time_ms << " ms\n";
                    std::cout << "  Input: " << data.input_width << "x" << data.input_height
                            << " -> Output: " << data.output_width << "x" << data.output_height << "\n";

                    // Database status
                    double db_status = db_stats.count("db_status") ? db_stats["db_status"] : -1;
                    if (db_status == 1) {
                        double samples = db_stats.count("db_sample_count") ? db_stats["db_sample_count"] : 0;
                        std::cout << "  DB: " << std::fixed << std::setprecision(0) << samples
                                << " samples in last 10 min\n";
                    } else {
                        std::cout << "  DB: " << (db_status == 0 ? "no_file" : "unavailable") << "\n";
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "\n" << service << " [error reading metrics]\n";
            }
        }
        else if (service == "frame-saver" && saver_reader) {
            try {
                auto data = saver_reader->getMetrics();

                bool is_running = data.service_pid > 0 &&
                                std::filesystem::exists("/proc/" + std::to_string(data.service_pid));

                std::cout << "\n" << service << " [" << (is_running ? "running" : "stopped") << "]";

                if (is_running) {
                    double uptime = (now - data.service_start_time / 1000000000) / 60.0;

                    auto db_stats = db_reader.get_stats(service, config.db_paths[service]);
                    double db_avg_fps = db_stats.count("db_avg_fps") ? db_stats["db_avg_fps"] : 0.0;

                    std::cout << " PID: " << data.service_pid << " | Uptime: "
                            << std::fixed << std::setprecision(0) << uptime << " min\n";
                    std::cout << "  Current FPS: " << std::setprecision(1) << data.current_fps
                            << " | Avg FPS (10m): " << db_avg_fps << "\n";
                    std::cout << "  Saved: " << data.frames_saved
                            << " | Dropped: " << data.frames_dropped
                            << " | IO Errors: " << data.io_errors << "\n";
                    std::cout << "  Save time: " << data.save_time_ms
                            << " ms | Disk usage: " << data.disk_usage_mb << " MB\n";

                    if (!data.output_dir.empty()) {
                        std::cout << "  Output: " << data.output_dir << "\n";
                    }

                    // Database status
                    double db_status = db_stats.count("db_status") ? db_stats["db_status"] : -1;
                    if (db_status == 1) {
                        double samples = db_stats.count("db_sample_count") ? db_stats["db_sample_count"] : 0;
                        std::cout << "  DB: " << std::fixed << std::setprecision(0) << samples
                                << " samples in last 10 min\n";
                    } else {
                        std::cout << "  DB: " << (db_status == 0 ? "no_file" : "unavailable") << "\n";
                    }

                    if (data.tracked_frames > 0) {
                        std::cout << "\n  Frame Pipeline Tracking:\n";
                        std::cout << "    End-to-End Latency: "
                                << std::fixed << std::setprecision(1)
                                << data.avg_total_latency_ms << "ms"
                                << " (min: " << data.min_total_latency_ms << "ms"
                                << ", max: " << data.max_total_latency_ms << "ms)\n";
                        std::cout << "    Stage Breakdown:\n";
                        std::cout << "      - Read→Publish: " << data.avg_publish_latency_ms << "ms\n";
                        std::cout << "      - Publish→Resize: " << data.avg_resize_latency_ms << "ms\n";
                        std::cout << "      - Resize→Save: " << data.avg_save_latency_ms << "ms\n";
                        std::cout << "    Tracked Frames: " << data.tracked_frames << "\n";
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "\n" << service << " [error reading metrics]\n";
            }
        } else {
            std::cout << "\n" << service << " [not connected]\n";
        }
    }

    void cleanup_stats() {
        for (auto& [port, stats] : queue_stats) {
            stats.reset_counters();
        }
    }

    void run() {
        std::cerr << "[Monitor] Starting monitoring loop...\n";

        auto last_display = steady_clock::now();
        auto last_watchdog = steady_clock::now();
        const auto watchdog_interval = std::chrono::seconds(10);

        while (running) {
            check_zmq_queues();

            auto now = steady_clock::now();

            if (duration_cast<seconds>(now - last_watchdog) >= watchdog_interval) {
                sd_notify(0, "WATCHDOG=1");
                last_watchdog = now;
            }

            if (duration_cast<seconds>(now - last_display).count() >= config.display_interval_s) {
                print_status();
                last_display = now;
            }

            // Periodic cleanup
            if (++stats_counter % 20 == 0) {
                cleanup_stats();
            }

            std::this_thread::sleep_for(milliseconds(config.update_interval_ms));
        }

        cleanup();
    }

    void cleanup() {
        std::cerr << "\n[Monitor] Cleaning up...\n";
        zmq_sockets.clear();
        zmq_context.reset();
        std::cerr << "[Monitor] Shutdown complete\n";
    }
};

QueueMonitor* QueueMonitor::instance = nullptr;

int main() {
    try {
        QueueMonitor monitor;
        monitor.run();
    } catch (const std::exception& e) {
        std::cerr << "[Monitor] Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
