#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <msgpack.hpp>

// Base metrics - all services have these
struct BaseMetrics {
    std::string service_name;
    int32_t service_pid;
    uint64_t errors;
    double current_fps;
    int64_t last_update_time;
    int64_t service_start_time;

    MSGPACK_DEFINE_MAP(service_name, service_pid, errors, current_fps,
                       last_update_time, service_start_time)
};

// Publisher metrics - exact field order
struct PublisherMetrics : BaseMetrics {
    uint64_t frames_published;
    uint64_t total_frames;
    double video_fps;
    int32_t video_width;
    int32_t video_height;
    bool video_healthy;
    std::string video_path;

    MSGPACK_DEFINE_MAP(service_name, service_pid, errors, current_fps,
                       last_update_time, service_start_time,
                       frames_published, total_frames, video_fps,
                       video_width, video_height, video_healthy, video_path)
};

// Resizer specific metrics
struct ResizerMetrics : BaseMetrics {
    uint64_t frames_processed;
    uint64_t frames_dropped;
    double processing_time_ms;
    int32_t input_width;
    int32_t input_height;
    int32_t output_width;
    int32_t output_height;
    bool service_healthy;

    MSGPACK_DEFINE_MAP(service_name, service_pid, errors, current_fps, last_update_time, service_start_time,
                   frames_processed, frames_dropped, processing_time_ms,
                   input_width, input_height, output_width, output_height, service_healthy)
};

// Saver specific metrics with tracking
struct SaverMetrics : BaseMetrics {
    uint64_t frames_saved;
    uint64_t frames_dropped;
    uint64_t io_errors;
    double save_time_ms;
    double disk_usage_mb;
    int32_t frame_width;
    int32_t frame_height;
    int32_t frame_channels;
    bool disk_healthy;
    std::string output_dir;
    std::string format;

    // Frame tracking metrics
    double avg_total_latency_ms;
    double min_total_latency_ms;
    double max_total_latency_ms;
    double avg_publish_latency_ms;
    double avg_resize_latency_ms;
    double avg_save_latency_ms;
    uint64_t tracked_frames;

    MSGPACK_DEFINE_MAP(service_name, service_pid, errors, current_fps, last_update_time, service_start_time,
                   frames_saved, frames_dropped, io_errors, save_time_ms, disk_usage_mb,
                   frame_width, frame_height, frame_channels, disk_healthy, output_dir, format,
                   avg_total_latency_ms, min_total_latency_ms, max_total_latency_ms,
                   avg_publish_latency_ms, avg_resize_latency_ms, avg_save_latency_ms, tracked_frames)
};

template<typename MetricsType>
class MetricsManager {
private:
    int fd;
    void* mapped_memory;
    size_t memory_size;
    std::string shm_path;
    bool is_owner;
    MetricsType local_metrics;

public:
    // Constructor for writers (owners)
    MetricsManager(const std::string& service_name, size_t size = 65536)
        : shm_path("/dev/shm/jvideo_" + service_name + "_metrics"),
          is_owner(true), memory_size(size), fd(-1) {

        fd = open(shm_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            throw std::runtime_error("Failed to create shared memory: " + std::string(strerror(errno)));
        }

        if (ftruncate(fd, memory_size) < 0) {
            close(fd);
            throw std::runtime_error("Failed to resize shared memory: " + std::string(strerror(errno)));
        }

        mapped_memory = mmap(nullptr, memory_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);

        if (mapped_memory == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Failed to map shared memory: " + std::string(strerror(errno)));
        }

        // Initialize
        local_metrics.service_name = service_name;
        local_metrics.service_pid = getpid();
        local_metrics.service_start_time =
            std::chrono::system_clock::now().time_since_epoch().count();

        updateSharedMemory();
    }

    // Constructor for readers (non-owners)
    MetricsManager(const std::string& service_name, bool read_only)
        : shm_path("/dev/shm/jvideo_" + service_name + "_metrics"),
          is_owner(false), fd(-1) {

        fd = open(shm_path.c_str(), read_only ? O_RDONLY : O_RDWR);
        if (fd < 0) {
            throw std::runtime_error("Failed to open shared memory file: " + std::string(strerror(errno)));
        }

        struct stat sb;
        if (fstat(fd, &sb) < 0) {
            close(fd);
            throw std::runtime_error("Failed to get file size: " + std::string(strerror(errno)));
        }
        memory_size = sb.st_size;

        mapped_memory = mmap(nullptr, memory_size,
                           read_only ? PROT_READ : PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
        if (mapped_memory == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Failed to map shared memory: " + std::string(strerror(errno)));
        }

        // Initialize local copy by reading from shared memory
        try {
            local_metrics = readFromSharedMemory();
        } catch (const std::exception& e) {
            // If we can't read initially, just use defaults
            local_metrics.service_name = service_name;
        }
    }

    // Destructor - clean up resources
    ~MetricsManager() {
        if (mapped_memory != MAP_FAILED) {
            munmap(mapped_memory, memory_size);
        }
        if (fd >= 0) {
            close(fd);
        }
        if (is_owner && !shm_path.empty()) {
            unlink(shm_path.c_str());
        }
    }

    // Update a metric value
    template<typename T>
    void updateMetric(const std::string& key, T value) {
        if (!is_owner) {
            throw std::runtime_error("Cannot update metrics as non-owner");
        }

        if (key == "errors") local_metrics.errors = value;
        else if (key == "current_fps") local_metrics.current_fps = value;
        // Add more fields as needed for your specific metrics types

        local_metrics.last_update_time =
            std::chrono::system_clock::now().time_since_epoch().count();
    }

    // Get current metrics
    MetricsType getMetrics() {
        if (is_owner) {
            return local_metrics;
        } else {
            return readFromSharedMemory();
        }
    }

    // Direct access to local metrics for owner
    MetricsType& metrics() {
        if (!is_owner) {
            throw std::runtime_error("Direct metrics access only available for owner");
        }
        return local_metrics;
    }

    // Commit changes to shared memory
    void commit() {
        if (is_owner) {
            local_metrics.last_update_time =
                std::chrono::system_clock::now().time_since_epoch().count();
            updateSharedMemory();
        } else {
            throw std::runtime_error("Cannot commit metrics as non-owner");
        }
    }

    // Write metrics to shared memory
    void updateSharedMemory() {
        if (!is_owner) {
            throw std::runtime_error("Cannot update shared memory as non-owner");
        }

        // Pack using msgpack with MAP format (same as Python OrderedDict)
        msgpack::sbuffer buffer;
        msgpack::pack(buffer, local_metrics);

        // Lock file
        if (flock(fd, LOCK_EX) != 0) {
            throw std::runtime_error("Failed to acquire write lock: " + std::string(strerror(errno)));
        }

        try {
            // Write at offset 64 (skip reserved area)
            uint64_t data_size = buffer.size();
            memcpy(static_cast<char*>(mapped_memory) + 64, &data_size, sizeof(data_size));
            memcpy(static_cast<char*>(mapped_memory) + 72, buffer.data(), buffer.size());

            // Flush to disk
            msync(mapped_memory, memory_size, MS_SYNC);
        } catch (...) {
            flock(fd, LOCK_UN);  // Ensure lock is released on error
            throw;
        }

        // Unlock
        flock(fd, LOCK_UN);
    }

    // Read metrics from shared memory
    MetricsType readFromSharedMemory() {
        MetricsType metrics;

        // Lock for reading
        if (flock(fd, LOCK_SH) != 0) {
            throw std::runtime_error("Failed to acquire read lock: " + std::string(strerror(errno)));
        }

        try {
            // Read data size (at offset 64)
            uint64_t data_size;
            memcpy(&data_size, static_cast<char*>(mapped_memory) + 64, sizeof(data_size));

            if (data_size == 0 || data_size > memory_size - 72) {
                throw std::runtime_error("Invalid data size in shared memory: " + std::to_string(data_size));
            }

            // Read data (at offset 72)
            const char* data = static_cast<char*>(mapped_memory) + 72;

            // Deserialize from msgpack
            msgpack::object_handle oh = msgpack::unpack(data, data_size);
            msgpack::object obj = oh.get();
            obj.convert(metrics);
        } catch (const std::exception& e) {
            flock(fd, LOCK_UN);  // Ensure lock is released on error
            throw std::runtime_error("Failed to read metrics: " + std::string(e.what()));
        } catch (...) {
            flock(fd, LOCK_UN);  // Ensure lock is released on error
            throw std::runtime_error("Unknown error reading metrics");
        }

        // Unlock
        flock(fd, LOCK_UN);

        return metrics;
    }
};

// Convenience typedefs
using PublisherMetricsManager = MetricsManager<PublisherMetrics>;
using ResizerMetricsManager = MetricsManager<ResizerMetrics>;
using SaverMetricsManager = MetricsManager<SaverMetrics>;
