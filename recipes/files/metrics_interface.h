#pragma once

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <msgpack.hpp>
#include <string>
#include <map>
#include <chrono>
#include <memory>
#include <cstring>

namespace bip = boost::interprocess;

// Base metrics data that all services share
struct BaseMetrics {
    std::string service_name;
    pid_t service_pid;
    uint64_t errors;
    double current_fps;
    int64_t last_update_time;
    int64_t service_start_time;

    MSGPACK_DEFINE(service_name, service_pid, errors, current_fps, last_update_time, service_start_time)
};

// Publisher specific metrics
struct PublisherMetrics : BaseMetrics {
    uint64_t frames_published;
    uint64_t total_frames;
    double video_fps;
    int32_t video_width;
    int32_t video_height;
    bool video_healthy;
    std::string video_path;

    MSGPACK_DEFINE(service_name, service_pid, errors, current_fps, last_update_time, service_start_time,
                   frames_published, total_frames, video_fps, video_width, video_height,
                   video_healthy, video_path)
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

    MSGPACK_DEFINE(service_name, service_pid, errors, current_fps, last_update_time, service_start_time,
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

    MSGPACK_DEFINE(service_name, service_pid, errors, current_fps, last_update_time, service_start_time,
                   frames_saved, frames_dropped, io_errors, save_time_ms, disk_usage_mb,
                   frame_width, frame_height, frame_channels, disk_healthy, output_dir, format,
                   avg_total_latency_ms, min_total_latency_ms, max_total_latency_ms,
                   avg_publish_latency_ms, avg_resize_latency_ms, avg_save_latency_ms, tracked_frames)
};

// Generic metrics manager using Boost.Interprocess and MessagePack
template<typename MetricsType>
class MetricsManager {
private:
    struct SharedData {
        bip::interprocess_mutex mutex;
        size_t data_size;
        char data[8192]; // Fixed buffer for msgpack data
    };

    std::unique_ptr<bip::managed_shared_memory> segment;
    SharedData* shared_data;
    std::string segment_name;
    bool is_owner;
    MetricsType local_metrics;

public:
    // Constructor for writers (owners)
    MetricsManager(const std::string& service_name, size_t size = 65536)
        : segment_name("jvideo_" + service_name + "_metrics"), is_owner(true) {  // No leading slash

        try {
            // Remove old segment if exists
            bip::shared_memory_object::remove(segment_name.c_str());

            // Create new segment
            segment = std::make_unique<bip::managed_shared_memory>(
                bip::create_only, segment_name.c_str(), size);

            // Allocate shared data
            shared_data = segment->construct<SharedData>("metrics")();

            // Initialize local metrics
            local_metrics.service_name = service_name;
            local_metrics.service_pid = getpid();
            local_metrics.service_start_time = std::chrono::system_clock::now().time_since_epoch().count();

            // Write initial data
            updateSharedMemory();

        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to create shared memory: " + std::string(e.what()));
        }
    }

    // Constructor for readers (non-owners)
    MetricsManager(const std::string& service_name, bool read_only)
        : segment_name("jvideo_" + service_name + "_metrics"), is_owner(false) {  // No leading slash

        try {
            segment = std::make_unique<bip::managed_shared_memory>(
                bip::open_only, segment_name.c_str());

            shared_data = segment->find<SharedData>("metrics").first;
            if (!shared_data) {
                throw std::runtime_error("Metrics data not found in shared memory");
            }

        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to open shared memory: " + std::string(e.what()));
        }
    }

    ~MetricsManager() {
        if (is_owner && !segment_name.empty()) {
            try {
                bip::shared_memory_object::remove(segment_name.c_str());
            } catch (...) {}
        }
    }

    // Update a metric value
    template<typename T>
    void updateMetric(const std::string& key, T value) {
        // Update local metrics based on key
        if (key == "frames_published" && std::is_same<MetricsType, PublisherMetrics>::value) {
            reinterpret_cast<PublisherMetrics&>(local_metrics).frames_published = value;
        } else if (key == "current_fps") {
            local_metrics.current_fps = value;
        } else if (key == "errors") {
            local_metrics.errors = value;
        }
        // Add more metric updates as needed...

        local_metrics.last_update_time = std::chrono::system_clock::now().time_since_epoch().count();
        updateSharedMemory();
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

    //Commit changes to shared memory
    void commit() {
        if (is_owner) {
            local_metrics.last_update_time = std::chrono::system_clock::now().time_since_epoch().count();
            updateSharedMemory();
        }
    }

private:
    void updateSharedMemory() {
        bip::scoped_lock<bip::interprocess_mutex> lock(shared_data->mutex);

        // Serialize to msgpack
        msgpack::sbuffer buffer;
        msgpack::pack(buffer, local_metrics);

        // Copy to shared memory
        if (buffer.size() > sizeof(shared_data->data)) {
            throw std::runtime_error("Metrics data too large for shared buffer");
        }

        shared_data->data_size = buffer.size();
        std::memcpy(shared_data->data, buffer.data(), buffer.size());
    }

    MetricsType readFromSharedMemory() {
        bip::scoped_lock<bip::interprocess_mutex> lock(shared_data->mutex);

        // Deserialize from msgpack
        msgpack::object_handle oh = msgpack::unpack(shared_data->data, shared_data->data_size);
        msgpack::object obj = oh.get();

        MetricsType metrics;
        obj.convert(metrics);
        return metrics;
    }
};

// Convenience typedefs
using PublisherMetricsManager = MetricsManager<PublisherMetrics>;
using ResizerMetricsManager = MetricsManager<ResizerMetrics>;
using SaverMetricsManager = MetricsManager<SaverMetrics>;
