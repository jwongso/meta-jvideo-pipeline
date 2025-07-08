#pragma once

#include <cstdint>
#include <chrono>
#include <string>

struct FrameTrackingInfo {
    uint64_t frame_id;           // Unique frame identifier
    uint64_t sequence_number;    // Sequential frame number from source
    double source_timestamp;     // When frame was read from video
    double publish_timestamp;    // When frame was published to ZMQ
    double resize_timestamp;     // When frame was resized
    double save_timestamp;       // When frame was saved to disk

    // Calculate latencies
    double get_publish_latency() const {
        return publish_timestamp - source_timestamp;
    }

    double get_resize_latency() const {
        return resize_timestamp - publish_timestamp;
    }

    double get_save_latency() const {
        return save_timestamp - resize_timestamp;
    }

    double get_total_latency() const {
        return save_timestamp - source_timestamp;
    }
};
