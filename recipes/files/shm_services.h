#pragma pack(push, 1)
struct PublisherSharedMetrics {
    char service_name[64];
    pid_t service_pid;
    uint64_t frames_published;
    uint64_t total_frames;
    uint64_t errors;
    double current_fps;
    double video_fps;
    int video_width;
    int video_height;
    bool video_healthy;
    char video_path[256];
    time_t last_update_time;
    time_t service_start_time;
    char _padding[64];
};

struct ResizerSharedMetrics {
    char service_name[64];
    pid_t service_pid;
    uint64_t frames_processed;
    uint64_t frames_dropped;
    uint64_t errors;
    double current_fps;
    double processing_time_ms;
    int input_width;
    int input_height;
    int output_width;
    int output_height;
    bool service_healthy;
    time_t last_update_time;
    time_t service_start_time;
    char _padding[64];
};

struct SaverSharedMetrics {
    char service_name[64];
    pid_t service_pid;
    uint64_t frames_saved;
    uint64_t frames_dropped;
    uint64_t errors;
    uint64_t io_errors;
    double current_fps;
    double save_time_ms;
    double disk_usage_mb;
    int frame_width;
    int frame_height;
    int frame_channels;
    bool disk_healthy;
    char output_dir[256];
    char format[16];
    time_t last_update_time;
    time_t service_start_time;
    char _padding[64];
};
#pragma pack(pop)
