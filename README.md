# Juni's Video Pipeline

A high-performance, multi-language video processing pipeline designed for embedded Linux systems built with Yocto. The system provides distributed video frame processing with dynamic language implementation switching, comprehensive monitoring, and robust service management through a custom Yocto meta-layer.

## Architecture Overview

Juni's Video Pipeline implements a microservices architecture where video frames flow through a series of processing stages. The system is built on ZeroMQ for inter-service communication and Redis for metrics collection and service coordination. Each service can be implemented in multiple programming languages (Python, C++, Rust) to optimize for different performance and development requirements.

### Core Components

- **Service Controller**: Central orchestration and management system
- **Frame Publisher**: Video input and frame distribution service
- **Frame Resizer**: Video frame transformation and scaling service  
- **Frame Saver**: Video frame persistence and storage service
- **Queue Monitor**: Pipeline health monitoring and performance tracking

## Yocto Integration

This project is delivered as a complete Yocto meta-layer (`meta-jvideo-pipeline`) that provides:

- BitBake recipes for all service implementations
- Systemd service files for automatic startup
- Configuration management
- Multi-language build support (Python, C++, Rust)
- Test content and dependencies

### Meta-Layer Structure

```
meta-jvideo-pipeline/
├── conf/
│   └── layer.conf                    # Layer configuration
├── recipes/
│   ├── files/                        # Service implementations and configs
│   │   ├── service_controller.py     # Main service controller
│   │   ├── service_base.py          # Base class for services
│   │   ├── frame_publisher.*        # Publisher implementations (py/cpp/rs)
│   │   ├── frame_resizer.*          # Resizer implementations (py/cpp)
│   │   ├── frame_saver.*            # Saver implementations (py/cpp)
│   │   ├── queue_monitor.py         # Queue monitoring service
│   │   ├── *.service                # Systemd service files
│   │   └── *.conf                   # Service configuration files
│   ├── jvideo-services.bb           # Main services recipe
│   └── jvideo-pipeline-image.bb     # Complete image recipe
├── recipes-devtools/
│   └── nlohmann-json/               # JSON library dependency
└── recipes-multimedia/
    └── jvideo-content.bb            # Test content recipe
```

## System Requirements

### Build Host Requirements
- Yocto Kirkstone (4.0) compatible build environment
- Python 3.6+
- Rust toolchain (for Rust implementations)
- CMake 3.10+ (for C++ implementations)
- 12+ CPU cores recommended (configured for parallel builds)

### Target Hardware
- x86_64 processor (QEMU x86-64 target configured)
- 1GB RAM (enforced via QB_MEM configuration)
- Storage for video processing and logs
- Hardware video acceleration support (optional)

### Runtime Dependencies
- systemd service manager (configured as init system)
- ZeroMQ library
- Redis server (optional, for metrics)
- OpenCV with FFmpeg support
- GStreamer 1.0 multimedia framework
- FFmpeg with commercial codec support

## Building with Yocto

### 1. Layer Setup
```bash
# Add meta-layer to your build configuration
echo 'BBLAYERS += "/path/to/meta-jvideo-pipeline"' >> conf/bblayers.conf

# Layer automatically configures:
# - SystemD as init manager
# - Commercial codec licensing
# - FFmpeg with advanced codec support (H.264, H.265, VP8/VP9, MP3)
# - OpenCV with FFmpeg and GStreamer integration
# - 12-thread parallel compilation
# - 1GB target memory allocation
```

### 2. Build Options

**Build Individual Services**
```bash
# Build all services
bitbake jvideo-services

# Build with specific language implementations
bitbake jvideo-services -c configure
```

**Build Complete Image**
```bash
# Build complete system image with pipeline
bitbake jvideo-pipeline-image
```

**Build with Test Content**
```bash
# Include test video content
bitbake jvideo-content
```

### 3. Deployment
```bash
# Flash to target device
dd if=tmp/deploy/images/MACHINE/jvideo-pipeline-image-MACHINE.wic of=/dev/sdX bs=4M

# Or deploy to running system
scp tmp/deploy/rpm/ARCH/jvideo-services-*.rpm root@target:/tmp/
rpm -Uvh /tmp/jvideo-services-*.rpm
```

## Multimedia Support

The layer provides comprehensive multimedia processing capabilities through integrated support for:

### Video Codecs and Formats
- **H.264/AVC**: Hardware-accelerated encoding and decoding
- **H.265/HEVC**: Next-generation video compression
- **VP8/VP9**: WebM video codecs for web delivery
- **MJPEG**: Motion JPEG for high-quality frame sequences

### Audio Codecs
- **MP3**: LAME encoder integration
- **AAC**: Advanced Audio Coding support
- **Vorbis**: Open-source audio compression

### Multimedia Frameworks
- **FFmpeg**: Complete multimedia framework with commercial codec support
- **OpenCV**: Computer vision library with video I/O capabilities
- **GStreamer 1.0**: Modular multimedia pipeline framework

### Development and Debugging Tools
- **v4l-utils**: Video4Linux utilities for camera management
- **GStreamer examples**: Sample applications and plugins
- **FFmpeg command-line tools**: Direct video processing capabilities

### Hardware Integration
- **Video4Linux (V4L2)**: Camera and capture device support
- **Hardware acceleration**: GPU-assisted video processing where available
- **Memory optimization**: 1GB allocation with efficient buffer management

### Service Configuration
Service configurations are deployed to `/etc/jvideo/` during build:

**Frame Publisher** (`/etc/jvideo/frame-publisher.conf`)
```ini
[input]
source_type = camera
device_path = /dev/video0
resolution = 1920x1080
framerate = 30

[output]
zmq_endpoint = tcp://localhost:5555
```

**Frame Resizer** (`/etc/jvideo/frame-resizer.conf`)
```ini
[input]
zmq_endpoint = tcp://localhost:5555

[processing]
target_resolutions = 720p,480p,240p
quality = high

[output]
zmq_endpoint = tcp://localhost:5556
```

**Frame Saver** (`/etc/jvideo/frame-saver.conf`)
```ini
[input]
zmq_endpoint = tcp://localhost:5556

[storage]
output_directory = /var/lib/jvideo/frames
format = jpeg
compression_quality = 85
```

**Queue Monitor** (`/etc/jvideo/queue-monitor.conf`)
```ini
[monitoring]
check_interval = 5
alert_threshold = 1000
metrics_endpoint = tcp://localhost:5557
```

### Runtime Service Configuration
```json
{
  "services": {
    "frame-publisher": {
      "enabled": true,
      "language": "cpp",
      "priority": "high"
    },
    "frame-resizer": {
      "enabled": true,
      "language": "cpp",
      "auto_scale": true
    },
    "frame-saver": {
      "enabled": true,
      "language": "python",
      "storage_backend": "local"
    },
    "queue-monitor": {
      "enabled": true,
      "language": "python",
      "alert_enabled": true
    }
  }
}
```

## Usage

### Service Management

**View Service Status**
```bash
control status
```

**Start Services**
```bash
# Start with default implementation
control start frame-publisher

# Start with specific language implementation  
control start frame-publisher cpp
control start frame-resizer python
```

**Dynamic Implementation Switching**
```bash
# Hot-swap to different implementation
control switch frame-publisher rust
control switch frame-resizer cpp
```

**Apply Configuration**
```bash
# Apply service configuration
control apply /etc/jvideo/services.conf
```

**System Monitoring**
```bash
# Continuous monitoring
control monitor
```

### Service Implementations

**Available Implementations by Service:**

| Service | Python | C++ | Rust | Default |
|---------|--------|-----|------|---------|
| frame-publisher | ✓ | ✓ | ✓ | cpp |
| frame-resizer | ✓ | ✓ | - | cpp |  
| frame-saver | ✓ | ✓ | - | cpp |
| queue-monitor | ✓ | - | - | python |

**Performance Characteristics:**

- **C++ Implementations**: Optimized for throughput and low latency
- **Python Implementations**: Flexible configuration and rapid development
- **Rust Implementations**: Memory-safe high-performance processing

## Development and Customization

**Layer Dependencies and Compatibility**
```bash
# layer.conf configuration
LAYERDEPENDS_jvideo-pipeline = "core"
LAYERSERIES_COMPAT_jvideo-pipeline = "kirkstone"
BBFILE_PRIORITY_jvideo-pipeline = "10"
```

**Optimized Build Configuration**
```bash
# Parallel build settings (configured in layer)
BB_NUMBER_THREADS = "12"
PARALLEL_MAKE = "-j 12"

# Target system configuration
MACHINE = "qemux86-64"
QB_MEM = "-m 1024"
APPEND += "mem=1024M"
```

**Recipe Dependencies**
```bash
# jvideo-services.bb dependencies
DEPENDS = "zeromq opencv python3-redis cmake-native ffmpeg gstreamer1.0"
RDEPENDS_${PN} = "systemd zeromq python3-core python3-redis ffmpeg \
                  gstreamer1.0 gstreamer1.0-plugins-base \
                  gstreamer1.0-plugins-good v4l-utils"
```

**Cross-Compilation Support**
```bash
# C++ cross-compilation with multimedia support
inherit cmake
EXTRA_OECMAKE = "-DCMAKE_BUILD_TYPE=Release \
                 -DENABLE_OPTIMIZATION=ON \
                 -DWITH_FFMPEG=ON \
                 -DWITH_GSTREAMER=ON"

# Rust cross-compilation  
inherit cargo
CARGO_SRC_DIR = "${S}"
```

### Adding Custom Implementations

**1. Create Implementation File**
```bash
# Add to recipes/files/
cp frame_publisher.py custom_frame_publisher.py
```

**2. Create Service File**
```bash
# Add systemd service file
cp jvideo-frame-publisher-python.service jvideo-frame-publisher-custom.service
```

**3. Update BitBake Recipe**
```bash
# Edit recipes/jvideo-services.bb
SRC_URI += "file://custom_frame_publisher.py \
            file://jvideo-frame-publisher-custom.service"
```

**4. Update Service Controller**
```python
# Add to service_controller.py services dictionary
'frame-publisher': {
    'languages': ['python', 'cpp', 'rust', 'custom'],
    'default': 'cpp'
}
```

### Build System Integration

**Recipe Dependencies**
```bash
# jvideo-services.bb dependencies
DEPENDS = "zeromq opencv python3-redis cmake-native"
RDEPENDS_${PN} = "systemd zeromq python3-core python3-redis"
```

**Cross-Compilation Support**
```bash
# C++ cross-compilation
inherit cmake
EXTRA_OECMAKE = "-DCMAKE_BUILD_TYPE=Release -DENABLE_OPTIMIZATION=ON"

# Rust cross-compilation  
inherit cargo
CARGO_SRC_DIR = "${S}"
```

## Monitoring and Debugging

### System Logs
```bash
# Service-specific logs
journalctl -u jvideo-frame-publisher-cpp.service -f

# All pipeline services
journalctl -u jvideo-* -f

# Application logs
tail -f /var/log/jvideo/*.log
```

### Performance Monitoring
```bash
# Real-time metrics
control monitor

# Resource usage
systemctl status jvideo-*
htop -p $(pgrep -d, jvideo)
```

### Debugging Failed Builds
```bash
# Debug BitBake builds
bitbake jvideo-services -c compile -f -D

# Check build logs
less tmp/work/MACHINE/jvideo-services/*/temp/log.do_compile
```

## Production Deployment

### Image Customization
```bash
# Create custom image recipe
inherit core-image
IMAGE_INSTALL += "jvideo-services jvideo-content"
IMAGE_FEATURES += "ssh-server-openssh"
```

### Performance Tuning
- Hardware video acceleration (GPU-assisted processing)
- Multi-threaded compilation (12 threads configured)
- Optimized memory allocation (1GB system memory)
- Advanced codec support (H.264, H.265, VP8/VP9)
- GStreamer hardware acceleration plugins
- SIMD optimizations in OpenCV and FFmpeg

### Security Hardening
- Run services with dedicated user accounts
- Restrict network access to localhost
- Enable systemd security features
- Configure log rotation and retention

## Troubleshooting

### Common Build Issues

**Missing Dependencies**
```bash
# Check layer dependencies
bitbake-layers show-layers
bitbake jvideo-services -g -u taskexp
```

**Cross-Compilation Errors**
```bash
# Clean and rebuild
bitbake jvideo-services -c clean
bitbake jvideo-services -f
```

### Runtime Issues

**Service Startup Failures**
```bash
# Check service status
systemctl status jvideo-frame-publisher-cpp.service
journalctl -u jvideo-frame-publisher-cpp.service --no-pager
```

**Performance Issues**
```bash
# Monitor system resources
iotop -o
nethogs
control status
```

## License

MIT
