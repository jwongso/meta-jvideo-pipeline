#pragma once

#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <fstream>
#include <mutex>
#include <csignal>
#include <functional>

// Check for systemd support
#ifdef __has_include
  #if __has_include(<systemd/sd-daemon.h>)
    #include <systemd/sd-daemon.h>
    #define HAS_SYSTEMD 1
  #else
    #define HAS_SYSTEMD 0
  #endif
#else
  #define HAS_SYSTEMD 0
#endif

#include "metrics_interface.h"

using json = nlohmann::json;
using namespace std::chrono;

template<typename MetricsType>
class PipelineStage {
public:
    using MetricsManagerType = MetricsManager<MetricsType>;

protected:
    // Configuration
    json config_;
    std::string config_path_;
    std::string service_name_;

    // Metrics and database
    std::unique_ptr<MetricsManagerType> metrics_mgr_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
    std::mutex db_mutex_;
    std::atomic<bool> export_in_progress_{false};
    std::thread export_thread_;

    // Timing
    steady_clock::time_point start_time_;
    steady_clock::time_point last_fps_update_;
    steady_clock::time_point last_watchdog_;

    // Control
    std::atomic<bool> running_{true};
    static std::atomic<bool> global_running_;

    // Frame counters
    uint64_t frames_processed_ = 0;
    uint64_t errors_ = 0;

    // Database configuration
    std::string db_path_;
    int export_interval_ = 1000;

    // Watchdog configuration
    bool watchdog_enabled_ = false;
    seconds watchdog_interval_{10}; // Send notification every 10 seconds

public:
    PipelineStage(const std::string& service_name, const std::string& config_path)
        : service_name_(service_name), config_path_(config_path) {

        start_time_ = steady_clock::now();
        last_fps_update_ = start_time_;
        last_watchdog_ = start_time_;

        // Initialize logger for this service
        initializeLogger();

        // Setup signal handlers
        setupSignalHandlers();

        // Check if systemd watchdog is enabled
        checkWatchdogStatus();
    }

    virtual ~PipelineStage() {
        if (export_thread_.joinable()) {
            export_thread_.join();
        }
    }

    // Initialize logger with service-specific configuration
    void initializeLogger() {
        auto& logger = Logger::getInstance();

        // Set logger configuration
        logger.setLevel(Logger::LogLevel::INFO);
        logger.setOutputMode(Logger::OutputMode::ALL);
        logger.setSyslogIdent(service_name_);

        // Set log file path
        std::string log_file = "/var/log/jvideo/" + service_name_ + ".log";
        if (logger.setLogFile(log_file)) {
            logger.setMaxFileSize(50 * 1024 * 1024); // 50MB
            logger.setMaxRotatedFiles(5);
        }

        LOG_INFO(service_name_, "Logger initialized for service");
    }

    // This MUST be called after construction
    void initialize() {
        loadConfig();
        initializeMetrics();
        initializeDatabase();

        // Notify systemd that we're ready
        notifySystemdReady();
    }

    // Main run loop - derived classes implement processFrame()
    void run() {
        // Initialize here, after object is fully constructed
        initialize();

        LOG_INFO(service_name_, "Starting processing...");

        onStart();

        while (running_ && global_running_) {
            try {
                if (!processFrame()) {
                    // No frame available, brief sleep
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }

                // Send watchdog notification if needed
                sendWatchdogNotification();

            } catch (const std::exception& e) {
                LOG_ERROR(service_name_, std::string("Processing error: ") + e.what());
                errors_++;
                updateErrorMetrics();
            }
        }

        // Notify systemd that we're stopping
        notifySystemdStopping();

        onStop();
        finalizeMetrics();

        auto total_time = duration<double>(steady_clock::now() - start_time_).count();
        std::ostringstream summary;
        summary << "Finished. Processed " << frames_processed_
                << " frames in " << std::fixed << std::setprecision(1)
                << total_time << " seconds";
        LOG_INFO(service_name_, summary.str());
    }

    void stop() {
        running_ = false;
    }

protected:
    // Watchdog support
    void checkWatchdogStatus() {
#if HAS_SYSTEMD
        // Check if watchdog is enabled via environment variable
        const char* watchdog_usec = std::getenv("WATCHDOG_USEC");
        if (watchdog_usec) {
            uint64_t usec = std::stoull(watchdog_usec);
            if (usec > 0) {
                watchdog_enabled_ = true;
                // Set interval to half of the watchdog timeout for safety
                watchdog_interval_ = std::chrono::duration_cast<seconds>(
                    microseconds(usec / 2)
                );
                std::ostringstream msg;
                msg << "Systemd watchdog enabled with interval: "
                    << watchdog_interval_.count() << "s";
                LOG_INFO(service_name_, msg.str());
            }
        }
#else
        // No systemd support compiled in
        watchdog_enabled_ = false;
#endif
    }

    void sendWatchdogNotification() {
        if (!watchdog_enabled_) return;

        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_watchdog_) >= watchdog_interval_) {
#if HAS_SYSTEMD
            sd_notify(0, "WATCHDOG=1");
            LOG_TRACE(service_name_, "Sent watchdog notification");
#endif
            last_watchdog_ = now;
        }
    }

    void notifySystemdReady() {
#if HAS_SYSTEMD
        sd_notify(0, "READY=1");
        LOG_INFO(service_name_, "Notified systemd: READY");
#endif
    }

    void notifySystemdStopping() {
#if HAS_SYSTEMD
        sd_notify(0, "STOPPING=1");
        LOG_INFO(service_name_, "Notified systemd: STOPPING");
#endif
    }

    // Pure virtual - must be implemented by derived classes
    virtual bool processFrame() = 0;
    virtual void onStart() {}
    virtual void onStop() {}
    virtual void updateServiceSpecificMetrics(MetricsType& metrics) = 0;
    virtual std::string getDatabaseSchema() const = 0;
    virtual void storeBenchmarkData(sqlite3_stmt* stmt, const MetricsType& metrics) = 0;

    // Common functionality
    void loadConfig() {
        // Load default config first
        config_ = getDefaultConfig();

        // Try to load user config
        std::ifstream config_file(config_path_);
        if (config_file.is_open()) {
            try {
                json user_config;
                config_file >> user_config;
                config_.merge_patch(user_config);
                LOG_INFO(service_name_, "Loaded config from " + config_path_);
            } catch (const std::exception& e) {
                std::string error_msg = "Config parse error: ";
                error_msg += e.what();
                error_msg += ", using defaults";
                LOG_WARN(service_name_, error_msg);
            }
        }

        // Extract common settings
        db_path_ = config_.value("benchmark_db_path", getDefaultDbPath());
        export_interval_ = config_.value("benchmark_export_interval", 1000);

        // Apply logger settings from config if present
        if (config_.contains("log_level")) {
            std::string level_str = config_["log_level"];
            Logger::LogLevel level = Logger::LogLevel::INFO;

            if (level_str == "TRACE") level = Logger::LogLevel::TRACE;
            else if (level_str == "DEBUG") level = Logger::LogLevel::DEBUG;
            else if (level_str == "INFO") level = Logger::LogLevel::INFO;
            else if (level_str == "WARN") level = Logger::LogLevel::WARN;
            else if (level_str == "ERROR") level = Logger::LogLevel::ERROR;
            else if (level_str == "FATAL") level = Logger::LogLevel::FATAL;

            Logger::getInstance().setLevel(level);
        }
    }

    virtual json getDefaultConfig() const {
        return {
            {"benchmark_export_interval", 1000},
            {"log_level", "INFO"}
        };
    }

    std::string getDefaultDbPath() const {
        return "/var/lib/jvideo/db/" + service_name_ + "_benchmarks.db";
    }

    void initializeMetrics() {
        metrics_mgr_ = std::make_unique<MetricsManagerType>(service_name_);
        LOG_INFO(service_name_, "Initialized metrics manager");
    }

    void initializeDatabase() {
        // Create in-memory database
        sqlite3* db_raw = nullptr;
        if (sqlite3_open(":memory:", &db_raw) != SQLITE_OK) {
            throw std::runtime_error("Failed to open in-memory database");
        }
        db_.reset(db_raw);

        // Configure for performance
        executeSQL("PRAGMA synchronous = OFF");
        executeSQL("PRAGMA journal_mode = MEMORY");

        // Create schema - NOW it's safe to call virtual function
        executeSQL(getDatabaseSchema());

        // Force initial export to create file
        createDirectoryPath(db_path_);
        asyncExportDatabase();

        LOG_INFO(service_name_, "Database initialized: " + db_path_);
    }

    void executeSQL(const std::string& sql) {
        char* err_msg = nullptr;
        if (sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::string error = err_msg ? err_msg : "Unknown error";
            sqlite3_free(err_msg);
            throw std::runtime_error("SQL error: " + error);
        }
    }

    void updateMetrics() {
        auto& metrics = metrics_mgr_->metrics();

        // Update common metrics
        metrics.errors = errors_;
        metrics.current_fps = calculateFPS();

        // Let derived class update specific metrics
        updateServiceSpecificMetrics(metrics);

        // Commit to shared memory
        metrics_mgr_->commit();

        // Store benchmark periodically
        if (frames_processed_ % export_interval_ == 0) {
            storeBenchmark();
            asyncExportDatabase();

            std::ostringstream status;
            status << "Processed " << frames_processed_
                   << " frames, FPS: " << std::fixed << std::setprecision(1)
                   << metrics.current_fps;
            LOG_INFO(service_name_, status.str());
        }
    }

    void updateErrorMetrics() {
        auto& metrics = metrics_mgr_->metrics();
        metrics.errors = errors_;
        metrics_mgr_->commit();
    }

    double calculateFPS() {
        auto now = steady_clock::now();
        double elapsed = duration<double>(now - start_time_).count();
        return elapsed > 0 ? frames_processed_ / elapsed : 0.0;
    }

    void storeBenchmark() {
        std::lock_guard<std::mutex> lock(db_mutex_);

        auto metrics = metrics_mgr_->getMetrics();

        // Prepare statement based on derived class schema
        std::string sql = getBenchmarkInsertSQL();
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            LOG_ERROR(service_name_, "Failed to prepare benchmark statement");
            return;
        }

        // Bind common fields
        sqlite3_bind_int64(stmt, 1, time(nullptr));

        // Let derived class bind specific fields
        storeBenchmarkData(stmt, metrics);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    virtual std::string getBenchmarkInsertSQL() const = 0;

    void asyncExportDatabase() {
        if (export_in_progress_.load()) return;

        export_in_progress_ = true;

        if (export_thread_.joinable()) {
            export_thread_.join();
        }

        export_thread_ = std::thread([this]() {
            try {
                std::lock_guard<std::mutex> lock(db_mutex_);

                sqlite3* file_db;
                if (sqlite3_open(db_path_.c_str(), &file_db) != SQLITE_OK) {
                    LOG_ERROR(service_name_, "Failed to open DB: " + db_path_);
                    export_in_progress_ = false;
                    return;
                }

                // Create schema in file database
                char* err_msg = nullptr;
                sqlite3_exec(file_db, getDatabaseSchema().c_str(), nullptr, nullptr, &err_msg);
                if (err_msg) sqlite3_free(err_msg);

                // Backup
                sqlite3_backup* backup = sqlite3_backup_init(file_db, "main", db_.get(), "main");
                if (backup) {
                    sqlite3_backup_step(backup, -1);
                    sqlite3_backup_finish(backup);
                }

                sqlite3_close(file_db);
                LOG_TRACE(service_name_, "Database exported successfully");
            } catch (const std::exception& e) {
                std::string error_msg = "Export failed: ";
                error_msg += e.what();
                LOG_ERROR(service_name_, error_msg);
            }
            export_in_progress_ = false;
        });
    }

    void finalizeMetrics() {
        auto& metrics = metrics_mgr_->metrics();
        metrics.errors = errors_;
        updateServiceSpecificMetrics(metrics);
        metrics_mgr_->commit();

        storeBenchmark();

        // Wait for final export to complete
        asyncExportDatabase();
        if (export_thread_.joinable()) {
            export_thread_.join();
        }
    }

    void createDirectoryPath(const std::string& filepath) {
        size_t pos = filepath.find_last_of("/");
        if (pos != std::string::npos) {
            std::string dir = filepath.substr(0, pos);
            system(("mkdir -p " + dir).c_str());
        }
    }

    // Signal handling
    static void signalHandler(int sig) {
        std::ostringstream msg;
        msg << "Received signal " << sig << ", shutting down...";

        // Get any instance to log through - signal handlers are static
        // This is safe because the logger is a singleton
        LOG_INFO("SignalHandler", msg.str());

        global_running_ = false;
    }

    void setupSignalHandlers() {
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
    }

    // Helper for derived classes
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
};

// Static member definition
template<typename MetricsType>
std::atomic<bool> PipelineStage<MetricsType>::global_running_{true};

// Convenience base classes remain the same...
class PublisherStage : public PipelineStage<PublisherMetrics> {
public:
    using PipelineStage::PipelineStage;

protected:
    std::string getDatabaseSchema() const override {
        return R"(
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
    }

    std::string getBenchmarkInsertSQL() const override {
        return R"(
            INSERT INTO publisher_benchmarks
            (timestamp, frames_published, total_frames, current_fps, video_fps,
             errors, uptime_seconds, memory_usage_kb)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        )";
    }

    void storeBenchmarkData(sqlite3_stmt* stmt, const PublisherMetrics& metrics) override {
        int uptime = static_cast<int>(time(nullptr) - metrics.service_start_time / 1000000000);

        sqlite3_bind_int64(stmt, 2, metrics.frames_published);
        sqlite3_bind_int64(stmt, 3, metrics.total_frames);
        sqlite3_bind_double(stmt, 4, metrics.current_fps);
        sqlite3_bind_double(stmt, 5, metrics.video_fps);
        sqlite3_bind_int64(stmt, 6, metrics.errors);
        sqlite3_bind_int(stmt, 7, uptime);
        sqlite3_bind_int64(stmt, 8, getMemoryUsage());
    }
};

class ResizerStage : public PipelineStage<ResizerMetrics> {
public:
    using PipelineStage::PipelineStage;

protected:
    std::string getDatabaseSchema() const override {
        return R"(
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
    }

    std::string getBenchmarkInsertSQL() const override {
        return R"(
            INSERT INTO resizer_benchmarks
            (timestamp, frames_processed, frames_dropped, current_fps, processing_time_ms,
             memory_usage_kb, input_width, input_height, output_width, output_height, errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";
    }

    void storeBenchmarkData(sqlite3_stmt* stmt, const ResizerMetrics& metrics) override {
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
    }
};

class SaverStage : public PipelineStage<SaverMetrics> {
public:
    using PipelineStage::PipelineStage;

protected:
    std::string getDatabaseSchema() const override {
        return R"(
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
    }

    std::string getBenchmarkInsertSQL() const override {
        return R"(
            INSERT INTO saver_benchmarks
            (timestamp, frames_saved, current_fps, save_time_ms, disk_usage_mb,
             frame_width, frame_height, frame_channels, io_errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";
    }

    void storeBenchmarkData(sqlite3_stmt* stmt, const SaverMetrics& metrics) override {
        sqlite3_bind_int64(stmt, 2, metrics.frames_saved);
        sqlite3_bind_double(stmt, 3, metrics.current_fps);
        sqlite3_bind_double(stmt, 4, metrics.save_time_ms);
        sqlite3_bind_double(stmt, 5, metrics.disk_usage_mb);
        sqlite3_bind_int(stmt, 6, metrics.frame_width);
        sqlite3_bind_int(stmt, 7, metrics.frame_height);
        sqlite3_bind_int(stmt, 8, metrics.frame_channels);
        sqlite3_bind_int64(stmt, 9, metrics.io_errors);
    }
};
