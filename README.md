# Juni's Video Pipeline

A high-performance, multi-language video processing pipeline designed for embedded Linux systems running on Yocto. The system provides distributed video frame processing with dynamic language implementation switching, comprehensive monitoring, and robust service management.

## Architecture Overview

Juni's Video Pipeline implements a microservices architecture where video frames flow through a series of processing stages. The system is built on ZeroMQ for inter-service communication and Redis for metrics collection and service coordination. Each service can be implemented in multiple programming languages (Python, C++, Rust) to optimize for different performance and development requirements.

### Core Components

- **Service Controller**: Central orchestration and management system
- **Frame Publisher**: Video input and frame distribution service
- **Frame Resizer**: Video frame transformation and scaling service  
- **Frame Saver**: Video frame persistence and storage service
- **Queue Monitor**: Pipeline health monitoring and performance tracking

## System Requirements

### Hardware
- ARM or x86_64 processor
- Minimum 512MB RAM (1GB recommended)
- Storage for video processing and logs

### Software
- Yocto Linux distribution
- Python 3.6+
- systemd service manager
- ZeroMQ library
- Redis server (optional, for metrics)

### Dependencies
- `python3-zmq`
- `python3-redis`
- `systemd`
- Language-specific compilers (gcc, rustc) for native implementations

## Installation

### 1. System Setup
```bash
# Create system directories
sudo mkdir -p /var/log/jvideo
sudo mkdir -p /etc/jvideo

# Create service user
sudo useradd -r -s /bin/false jvideo
sudo chown -R jvideo:jvideo /var/log/jvideo
```

### 2. Service Installation
```bash
# Copy service binaries
sudo cp service_controller.py /usr/local/bin/control
sudo cp service_base.py /usr/local/lib/python3.x/site-packages/
sudo chmod +x /usr/local/bin/control

# Install systemd service files
sudo cp systemd/*.service /etc/systemd/system/
sudo systemctl daemon-reload
```

### 3. Configuration
```bash
# Create default configuration
sudo cp config/services.conf /etc/jvideo/
sudo chown jvideo:jvideo /etc/jvideo/services.conf
```

## Configuration

### Service Configuration File
Location: `/etc/jvideo/services.conf`

```json
{
  "services": {
    "frame-publisher": {
      "enabled": true,
      "language": "cpp"
    },
    "frame-resizer": {
      "enabled": true,
      "language": "cpp"
    },
    "frame-saver": {
      "enabled": true,
      "language": "python"
    },
    "queue-monitor": {
      "enabled": true,
      "language": "python"
    }
  }
}
```

### Logging Configuration
Logs are written to `/var/log/jvideo/` with the following structure:
- `service-controller.log`: Central controller operations
- `frame-publisher.log`: Publisher service logs
- `frame-resizer.log`: Resizer service logs
- `frame-saver.log`: Saver service logs
- `queue-monitor.log`: Monitor service logs

## Usage

### Basic Operations

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

**Stop Services**
```bash
control stop frame-publisher
control stop frame-resizer
```

**Switch Implementation Language**
```bash
# Hot-swap to different implementation
control switch frame-publisher rust
control switch frame-resizer cpp
```

**Restart Services**
```bash
control restart frame-publisher
```

**Apply Configuration**
```bash
# Apply default configuration
control apply

# Apply specific configuration file
control apply /path/to/custom/services.conf
```

**Monitor Services**
```bash
# Continuous monitoring mode
control monitor
```

### Advanced Operations

**Batch Service Management**
```bash
# Start all services with optimal implementations
control apply /etc/jvideo/production.conf

# Monitor system performance
control monitor &
tail -f /var/log/jvideo/service-controller.log
```

## Service Implementations

### Language-Specific Implementations

**C++ Implementations**
- Optimized for high-throughput video processing
- Minimal memory footprint
- Direct hardware acceleration support
- Recommended for production deployments

**Python Implementations**
- Rapid development and prototyping
- Extensive library ecosystem
- Flexible configuration options
- Ideal for development and testing

**Rust Implementations**
- Memory safety without garbage collection
- High performance with safety guarantees
- Concurrent processing capabilities
- Suitable for safety-critical applications

## Monitoring and Metrics

### Redis Metrics
When Redis is available, the system automatically collects:
- Frame processing rates
- Error counts and types
- Service uptime and performance
- Queue depth and latency metrics

### Service Health Monitoring
```bash
# Real-time status monitoring
control status

# Detailed service information
systemctl status jvideo-frame-publisher-cpp.service
systemctl status jvideo-frame-resizer-cpp.service
```

### Log Analysis
```bash
# Monitor service logs
tail -f /var/log/jvideo/frame-publisher.log

# Search for errors
grep ERROR /var/log/jvideo/*.log

# Performance metrics
grep "Metrics" /var/log/jvideo/*.log
```

## Performance Tuning

### CPU Optimization
- Use C++ implementations for CPU-intensive services
- Adjust service priorities using systemd
- Configure CPU affinity for critical services

### Memory Management
- Monitor memory usage through service logs
- Use Rust implementations for memory-sensitive environments
- Configure appropriate swap settings

### Network Optimization
- Tune ZeroMQ buffer sizes
- Configure Redis memory policies
- Optimize inter-service communication patterns

## Troubleshooting

### Common Issues

**Service Won't Start**
```bash
# Check service status
systemctl status jvideo-frame-publisher-cpp.service

# Check logs
journalctl -u jvideo-frame-publisher-cpp.service

# Verify dependencies
control status
```

**Performance Issues**
```bash
# Check system resources
top -p $(pgrep -d, jvideo)

# Monitor queue depths
control monitor

# Check for errors
grep ERROR /var/log/jvideo/*.log
```

**Redis Connection Issues**
```bash
# Test Redis connectivity
redis-cli ping

# Check Redis logs
journalctl -u redis.service

# Verify Redis configuration
redis-cli info
```

### Debug Mode
Enable debug logging by setting environment variables:
```bash
export JVIDEO_DEBUG=1
export JVIDEO_LOG_LEVEL=DEBUG
control start frame-publisher
```

## Development

### Adding New Services
1. Extend `ServiceBase` class for common functionality
2. Implement service-specific processing logic
3. Create systemd service file
4. Add service configuration to controller
5. Update service registry in `service_controller.py`

### Contributing Language Implementations
1. Follow existing naming conventions
2. Implement equivalent functionality across languages
3. Maintain performance benchmarks
4. Update systemd service files
5. Test implementation switching

## Security Considerations

### Access Control
- Services run as dedicated `jvideo` user
- Log files have restricted permissions
- Configuration files secured with appropriate ownership

### Network Security
- ZeroMQ endpoints bound to localhost by default
- Redis access restricted to local connections
- No external network dependencies required

## License

MIT
