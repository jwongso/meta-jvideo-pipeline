#pragma once

#include <cstdint>
#include <sys/types.h>
#include <ctime>

// Ensure structures match Python's struct.pack format exactly
#pragma pack(push, 1)

// PublisherSharedMetrics structure
struct PublisherSharedMetrics {
    char service_name[64];          // 64s
    pid_t service_pid;              // pid_t (4 bytes on x86_64)
    uint64_t frames_published;      // Q
    uint64_t total_frames;          // Q
    uint64_t errors;                // Q
    double current_fps;             // d
    double video_fps;               // d
    int32_t video_width;            // i
    int32_t video_height;           // i
    bool video_healthy;             // ? (1 byte)
    char video_path[256];           // 256s
    int64_t last_update_time;       // q
    int64_t service_start_time;     // q
    char _padding[64];              // 64s
} __attribute__((packed));

// ResizerSharedMetrics structure
struct ResizerSharedMetrics {
    char service_name[64];          // 64s
    pid_t service_pid;              // pid_t (4 bytes)
    uint64_t frames_processed;      // Q
    uint64_t frames_dropped;        // Q
    uint64_t errors;                // Q
    double current_fps;             // d
    double processing_time_ms;      // d
    int32_t input_width;            // i
    int32_t input_height;           // i
    int32_t output_width;           // i
    int32_t output_height;          // i
    bool service_healthy;           // ? (1 byte)
    int64_t last_update_time;       // q
    int64_t service_start_time;     // q
    char _padding[64];              // 64s
} __attribute__((packed));

// SaverSharedMetrics structure with tracking metrics
struct SaverSharedMetrics {
    char service_name[64];          // 64s
    pid_t service_pid;              // pid_t (4 bytes)
    uint64_t frames_saved;          // Q
    uint64_t frames_dropped;        // Q
    uint64_t errors;                // Q
    uint64_t io_errors;             // Q
    double current_fps;             // d
    double save_time_ms;            // d
    double disk_usage_mb;           // d
    int32_t frame_width;            // i
    int32_t frame_height;           // i
    int32_t frame_channels;         // i
    bool disk_healthy;              // ? (1 byte)
    char output_dir[256];           // 256s
    char format[16];                // 16s
    int64_t last_update_time;       // q
    int64_t service_start_time;     // q

    // Frame tracking metrics
    double avg_total_latency_ms;    // d
    double min_total_latency_ms;    // d
    double max_total_latency_ms;    // d
    double avg_publish_latency_ms;  // d
    double avg_resize_latency_ms;   // d
    double avg_save_latency_ms;     // d
    uint64_t tracked_frames;        // Q

    char _padding[60];              // 60s to make total 492 bytes
} __attribute__((packed));

#pragma pack(pop)

// Helper function to safely extract null-terminated strings
inline std::string safe_string(const char* str, size_t max_len) {
    if (!str) return "";
    size_t len = strnlen(str, max_len);
    return std::string(str, len);
}

inline bool is_valid_pid(uint64_t pid) {
    return pid > 0 && pid < 100000;
}

inline bool is_valid_fps(double fps) {
    return fps >= 0.0 && fps < 1000.0;
}

inline bool is_valid_time(int64_t timestamp) {
    // Check if timestamp is reasonable (between year 2020 and 2030)
    return timestamp > 1577836800 && timestamp < 1893456000;
}
