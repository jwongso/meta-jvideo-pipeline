#!/usr/bin/env python3
"""Queue Monitor - Reads metrics from C++ services via shared memory and SQLite"""

import sys
import time
import os
import json
import sqlite3
import mmap
import struct
import signal
from collections import deque
from contextlib import contextmanager

# Try to import ZMQ
try:
    import zmq
    ZMQ_AVAILABLE = True
except ImportError:
    print("WARNING: ZMQ not available, queue monitoring disabled", file=sys.stderr)
    ZMQ_AVAILABLE = False

class SharedMemoryReader:
    """Read metrics from C++ service shared memory segments"""

    def __init__(self, shm_name, struct_format, field_names):
        self.shm_name = shm_name
        self.struct_format = struct_format
        self.field_names = field_names
        self.struct_size = struct.calcsize(struct_format)
        self.fd = None
        self.mm = None

    def open(self):
        """Open shared memory segment"""
        try:
            # Direct file access to /dev/shm
            shm_path = f"/dev/shm{self.shm_name}"
            if not os.path.exists(shm_path):
                return False

            self.fd = os.open(shm_path, os.O_RDONLY)
            self.mm = mmap.mmap(self.fd, self.struct_size, access=mmap.ACCESS_READ)
            return True

        except Exception as e:
            print(f"ERROR opening {self.shm_name}: {e}", file=sys.stderr)
            return False

    def read_metrics(self):
        """Read and parse metrics from shared memory"""
        if not self.mm:
            return {}

        try:
            self.mm.seek(0)
            data = self.mm.read(self.struct_size)
            values = struct.unpack(self.struct_format, data)

            metrics = {}
            for i, field in enumerate(self.field_names):
                if i < len(values):
                    value = values[i]
                    # Decode string fields
                    if isinstance(value, bytes):
                        value = value.decode('utf-8', errors='ignore').rstrip('\x00')
                    metrics[field] = value

            return metrics

        except Exception as e:
            print(f"ERROR reading {self.shm_name}: {e}", file=sys.stderr)
            return {}

    def close(self):
        """Clean up resources"""
        if self.mm:
            self.mm.close()
            self.mm = None
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

class ServiceMetricsCollector:
    """Collect metrics from all C++ services"""

    def __init__(self, config):
        self.config = config

        # Service configurations matching C++ structs with tracking
        self.services = {
            'frame-publisher': {
                'shm_name': '/jvideo_publisher_metrics',
                'db_path': config.get('service_db_paths', {}).get(
                    'frame-publisher',
                    '/var/lib/jvideo/db/publisher_benchmarks.db'
                ),
                # PublisherSharedMetrics structure - with 4-byte padding after pid
                'struct_format': '64s I 4x QQQ dd ii ? 256s qq 53s',
                'fields': [
                    'service_name',      # char[64]
                    'service_pid',       # pid_t (4 bytes)
                    # 4 bytes padding (4x)
                    'frames_published',  # uint64_t
                    'total_frames',      # uint64_t
                    'errors',            # uint64_t
                    'current_fps',       # double
                    'video_fps',         # double
                    'video_width',       # int32_t
                    'video_height',      # int32_t
                    'video_healthy',     # bool
                    'video_path',        # char[256]
                    'last_update_time',  # int64_t
                    'service_start_time',# int64_t
                    '_padding'           # char[53]
                ]
            },
            'frame-resizer': {
                'shm_name': '/jvideo_resizer_metrics',
                'db_path': config.get('service_db_paths', {}).get(
                    'frame-resizer',
                    '/var/lib/jvideo/db/resizer_benchmarks.db'
                ),
                # ResizerSharedMetrics structure - with 4-byte padding after pid
                'struct_format': '64s I 4x QQQ dd iiii ? qq 53s',
                'fields': [
                    'service_name',      # char[64]
                    'service_pid',       # pid_t (4 bytes)
                    # 4 bytes padding (4x)
                    'frames_processed',  # uint64_t
                    'frames_dropped',    # uint64_t
                    'errors',            # uint64_t
                    'current_fps',       # double
                    'processing_time_ms',# double
                    'input_width',       # int32_t
                    'input_height',      # int32_t
                    'output_width',      # int32_t
                    'output_height',     # int32_t
                    'service_healthy',   # bool
                    'last_update_time',  # int64_t
                    'service_start_time',# int64_t
                    '_padding'           # char[53]
                ]
            },
            'frame-saver': {
                'shm_name': '/jvideo_saver_metrics',
                'db_path': config.get('service_db_paths', {}).get(
                    'frame-saver',
                    '/var/lib/jvideo/db/saver_benchmarks.db'
                ),
                # SaverSharedMetrics structure - with 4-byte padding after pid
                'struct_format': '64s I 4x QQQQ ddd iii ? 256s 16s qq ddddddQ 53s',
                'fields': [
                    'service_name',           # char[64]
                    'service_pid',            # pid_t (4 bytes)
                    # 4 bytes padding (4x)
                    'frames_saved',           # uint64_t
                    'frames_dropped',         # uint64_t
                    'errors',                 # uint64_t
                    'io_errors',              # uint64_t
                    'current_fps',            # double
                    'save_time_ms',           # double
                    'disk_usage_mb',          # double
                    'frame_width',            # int32_t
                    'frame_height',           # int32_t
                    'frame_channels',        # int32_t
                    'disk_healthy',          # bool
                    'output_dir',            # char[256]
                    'format',                # char[16]
                    'last_update_time',      # int64_t
                    'service_start_time',    # int64_t
                    'avg_total_latency_ms',  # double
                    'min_total_latency_ms',  # double
                    'max_total_latency_ms',  # double
                    'avg_publish_latency_ms',# double
                    'avg_resize_latency_ms', # double
                    'avg_save_latency_ms',   # double
                    'tracked_frames',        # uint64_t
                    '_padding'               # char[53]
                ]
            }
        }
        self.shm_readers = {}
        self.last_db_read = {}
        self._db_stats_cache = {}

    def initialize(self):
        """Initialize all shared memory connections"""
        for service_name, config in self.services.items():
            reader = SharedMemoryReader(
                config['shm_name'],
                config['struct_format'],
                config['fields']
            )
            if reader.open():
                self.shm_readers[service_name] = reader
                print(f"Connected to {service_name} shared memory", file=sys.stderr)
            else:
                print(f"WARNING: Could not connect to {service_name} shared memory", file=sys.stderr)

    def get_service_metrics(self):
        """Collect metrics from all available services"""
        metrics = {}

        for service_name, reader in self.shm_readers.items():
            shm_data = reader.read_metrics()

            # Check if service is running
            pid = shm_data.get('service_pid', 0)
            is_running = pid > 0 and pid < 100000 and os.path.exists(f'/proc/{pid}')

            # Calculate uptime
            uptime = 0
            if is_running and 'service_start_time' in shm_data:
                start_time = shm_data['service_start_time']
                if start_time > 0 and start_time < time.time():
                    uptime = time.time() - start_time

            # Get database stats
            db_stats = self._get_db_stats(service_name)

            metrics[service_name] = {
                'status': 'running' if is_running else 'stopped',
                'pid': pid,
                'uptime': uptime,
                **shm_data,
                **db_stats
            }

        return metrics

    @contextmanager
    def _db_connection(self, db_path):
        """Safe database connection context manager"""
        conn = None
        try:
            if os.path.exists(db_path):
                conn = sqlite3.connect(db_path, timeout=1.0)
                conn.row_factory = sqlite3.Row
                yield conn
            else:
                yield None
        except sqlite3.Error as e:
            print(f"Database error ({db_path}): {e}", file=sys.stderr)
            yield None
        finally:
            if conn:
                conn.close()

    def _get_db_stats(self, service_name):
        """Get historical metrics from SQLite database"""
        config = self.services.get(service_name, {})
        db_path = config.get('db_path')

        if not db_path or not os.path.exists(db_path):
            return {'db_status': 'no_file'}

        # Check if we should read the database
        now = time.time()
        should_read = True

        if service_name in self.last_db_read:
            time_since_last_read = now - self.last_db_read[service_name]
            should_read = time_since_last_read >= self.config.get('db_read_interval', 60)

        # Return cached stats if we shouldn't read yet
        if not should_read and service_name in self._db_stats_cache:
            return self._db_stats_cache[service_name]

        # Read the database
        self.last_db_read[service_name] = now
        stats = {'db_status': 'ok'}

        with self._db_connection(db_path) as conn:
            if not conn:
                return {'db_status': 'unavailable'}

            try:
                # Query based on service type - matching C++ table names
                recent_time = int(now - 600)  # Last 10 minutes

                if service_name == 'frame-publisher':
                    cursor = conn.execute("""
                        SELECT AVG(current_fps) as avg_fps,
                               MAX(current_fps) as max_fps,
                               MIN(current_fps) as min_fps,
                               COUNT(*) as sample_count,
                               AVG(memory_usage_kb) as avg_memory_kb,
                               MAX(errors) as max_errors
                        FROM publisher_benchmarks
                        WHERE timestamp > ?
                    """, (recent_time,))

                elif service_name == 'frame-resizer':
                    cursor = conn.execute("""
                        SELECT AVG(current_fps) as avg_fps,
                               MAX(current_fps) as max_fps,
                               MIN(current_fps) as min_fps,
                               AVG(processing_time_ms) as avg_processing_ms,
                               COUNT(*) as sample_count,
                               MAX(errors) as max_errors
                        FROM resizer_benchmarks
                        WHERE timestamp > ?
                    """, (recent_time,))

                elif service_name == 'frame-saver':
                    cursor = conn.execute("""
                        SELECT AVG(current_fps) as avg_fps,
                               MAX(current_fps) as max_fps,
                               MIN(current_fps) as min_fps,
                               AVG(save_time_ms) as avg_save_ms,
                               MAX(disk_usage_mb) as max_disk_mb,
                               COUNT(*) as sample_count,
                               MAX(io_errors) as max_io_errors
                        FROM saver_benchmarks
                        WHERE timestamp > ?
                    """, (recent_time,))

                row = cursor.fetchone()
                if row:
                    for key, value in dict(row).items():
                        if value is not None:
                            stats[f'db_{key}'] = value

            except sqlite3.Error as e:
                stats['db_error'] = str(e)
                stats['db_status'] = 'error'

        self._db_stats_cache[service_name] = stats
        return stats

    def cleanup(self):
        """Clean up all resources"""
        for reader in self.shm_readers.values():
            reader.close()

class QueueMonitor:
    """Main Queue Monitor Application"""

    def __init__(self):
        self.running = True
        self.zmq_context = None
        self.mon_sockets = []
        self.queue_stats = {}
        self.stats_counter = 0

        # Default configuration
        self.config = {
            'monitor_ports': [5555, 5556],
            'update_interval': 1,
            'display_interval': 5,
            'db_read_interval': 60,
            'service_db_paths': {
                'frame-publisher': '/var/lib/jvideo/db/publisher_benchmarks.db',
                'frame-resizer': '/var/lib/jvideo/db/resizer_benchmarks.db',
                'frame-saver': '/var/lib/jvideo/db/saver_benchmarks.db'
            }
        }

        # Load configuration
        self._load_config()

        # Initialize components
        self.metrics_collector = ServiceMetricsCollector(self.config)
        self._setup_signal_handlers()

        # Initialize ZMQ if available
        if ZMQ_AVAILABLE:
            self._setup_zmq_monitoring()

        # Initialize shared memory connections
        self.metrics_collector.initialize()

        print("[Monitor] Queue Monitor initialized", file=sys.stderr)

    def _load_config(self):
        """Load configuration from file"""
        config_path = '/etc/jvideo/queue-monitor.conf'
        try:
            if os.path.exists(config_path):
                with open(config_path) as f:
                    user_config = json.load(f)
                    self.config.update(user_config)
                    print(f"[Monitor] Loaded config from {config_path}", file=sys.stderr)
        except Exception as e:
            print(f"[Monitor] Config error: {e}, using defaults", file=sys.stderr)

    def _setup_signal_handlers(self):
        """Configure signal handlers for graceful shutdown"""
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, signum, frame):
        """Handle shutdown signals"""
        print(f"\n[Monitor] Received signal {signum}, shutting down...", file=sys.stderr)
        self.running = False

    def _setup_zmq_monitoring(self):
        """Initialize ZMQ monitoring sockets"""
        self.zmq_context = zmq.Context()

        for port in self.config['monitor_ports']:
            try:
                socket = self.zmq_context.socket(zmq.SUB)
                socket.setsockopt(zmq.RCVTIMEO, 100)
                socket.setsockopt(zmq.RCVHWM, 10)
                socket.setsockopt(zmq.LINGER, 0)
                socket.connect(f"tcp://localhost:{port}")
                socket.setsockopt_string(zmq.SUBSCRIBE, "")

                self.mon_sockets.append((port, socket))
                self.queue_stats[port] = {
                    'messages': 0,
                    'bytes': 0,
                    'last_seen': 0,
                    'errors': 0,
                    'latencies': deque(maxlen=100)
                }

                print(f"[Monitor] Monitoring ZMQ port {port}", file=sys.stderr)

            except Exception as e:
                print(f"[Monitor] Failed to monitor port {port}: {e}", file=sys.stderr)

    def _check_zmq_queue(self, port, socket):
        """Check a ZMQ queue for messages"""
        try:
            # Try to receive message parts
            messages = socket.recv_multipart(zmq.NOBLOCK)
            if messages:
                self.queue_stats[port]['messages'] += 1
                self.queue_stats[port]['bytes'] += sum(len(m) for m in messages)
                self.queue_stats[port]['last_seen'] = time.time()

                # Try to parse metadata for latency
                if len(messages) >= 1:
                    try:
                        metadata = json.loads(messages[0])
                        if 'timestamp' in metadata:
                            latency = (time.time() - metadata['timestamp']) * 1000
                            if 0 <= latency <= 10000:  # Sanity check
                                self.queue_stats[port]['latencies'].append(latency)
                    except:
                        pass

        except zmq.Again:
            # No messages available
            pass
        except Exception as e:
            self.queue_stats[port]['errors'] += 1

    def _get_system_stats(self):
        """Get system CPU and memory stats"""
        stats = {'cpu_percent': 0.0, 'memory_percent': 0.0, 'memory_mb': 0}

        # CPU usage from /proc/stat
        try:
            with open('/proc/stat') as f:
                cpu_line = f.readline()
                if cpu_line.startswith('cpu '):
                    values = cpu_line.split()[1:8]
                    total = sum(int(v) for v in values)
                    idle = int(values[3])
                    if hasattr(self, '_last_cpu_total'):
                        total_diff = total - self._last_cpu_total
                        idle_diff = idle - self._last_cpu_idle
                        if total_diff > 0:
                            stats['cpu_percent'] = 100.0 * (1.0 - idle_diff / total_diff)
                    self._last_cpu_total = total
                    self._last_cpu_idle = idle
        except:
            pass

        # Memory usage from /proc/meminfo
        try:
            with open('/proc/meminfo') as f:
                meminfo = {}
                for line in f:
                    parts = line.split()
                    if len(parts) >= 2:
                        key = parts[0].rstrip(':')
                        value = int(parts[1])
                        if key in ['MemTotal', 'MemAvailable']:
                            meminfo[key] = value

                if 'MemTotal' in meminfo and 'MemAvailable' in meminfo:
                    total = meminfo['MemTotal']
                    available = meminfo['MemAvailable']
                    stats['memory_mb'] = total / 1024
                    stats['memory_percent'] = 100.0 * (1.0 - available / total)
        except:
            pass

        return stats

    def _print_status(self):
        """Generate and print system status report"""
        sys_stats = self._get_system_stats()
        services = self.metrics_collector.get_service_metrics()

        # Clear screen for dashboard effect
        os.system('clear 2>/dev/null || cls 2>/dev/null')

        # Initialize lines list here!
        lines = []

        # Header
        lines.append("=" * 80)
        lines.append(f"JVideo Pipeline Monitor - {time.strftime('%Y-%m-%d %H:%M:%S')}")
        lines.append(f"System: CPU {sys_stats['cpu_percent']:.1f}% | "
                    f"Memory {sys_stats['memory_percent']:.1f}% "
                    f"({sys_stats['memory_mb']:.0f} MB)")
        lines.append("=" * 80)

        # Queue Statistics
        if self.mon_sockets:
            lines.append("\nQueue Statistics:")
            lines.append("-" * 80)
            for port, stats in self.queue_stats.items():
                age = "Never"
                if stats['last_seen'] > 0:
                    age = f"{time.time() - stats['last_seen']:.1f}s ago"

                avg_latency = 0
                if stats['latencies']:
                    avg_latency = sum(stats['latencies']) / len(stats['latencies'])

                lines.append(f"Port {port}: {stats['messages']:>8} msgs | "
                            f"{stats['bytes']/1024/1024:>6.1f} MB | "
                            f"Latency: {avg_latency:>5.1f} ms | "
                            f"Last: {age:>12} | "
                            f"Errors: {stats['errors']}")

        # Service Statistics
        lines.append("\nService Status:")
        lines.append("-" * 80)

        for name, info in services.items():
            status = info.get('status', 'unknown')
            pid = info.get('pid', 0)

            if status == 'running':
                uptime_min = info.get('uptime', 0) / 60
                fps = info.get('current_fps', 0)
                db_avg_fps = info.get('db_avg_fps', 0)

                lines.append(f"\n{name.upper()} [{status}] PID: {pid} | Uptime: {uptime_min:.0f} min")
                lines.append(f"  Current FPS: {fps:.1f} | Avg FPS (10m): {db_avg_fps:.1f}")

                # Service-specific details
                if name == 'frame-publisher':
                    published = info.get('frames_published', 0)
                    total = info.get('total_frames', 0)
                    errors = info.get('errors', 0)
                    video_path = info.get('video_path', 'unknown')

                    # Fix for large frame count
                    if total == 0 or total > 1000000000:
                        frame_info = f"{published} frames"
                    else:
                        frame_info = f"{published}/{total} frames"

                    lines.append(f"  Published: {frame_info} | Errors: {errors}")
                    lines.append(f"  Video: {os.path.basename(video_path)}")
                    lines.append(f"  Resolution: {info.get('video_width', 0)}x{info.get('video_height', 0)} @ {info.get('video_fps', 0):.1f} fps")

                elif name == 'frame-resizer':
                    processed = info.get('frames_processed', 0)
                    dropped = info.get('frames_dropped', 0)
                    proc_time = info.get('processing_time_ms', 0)
                    errors = info.get('errors', 0)

                    lines.append(f"  Processed: {processed} | Dropped: {dropped} | Errors: {errors}")
                    lines.append(f"  Processing time: {proc_time:.1f} ms")
                    lines.append(f"  Input: {info.get('input_width', 0)}x{info.get('input_height', 0)} -> "
                                f"Output: {info.get('output_width', 0)}x{info.get('output_height', 0)}")

                elif name == 'frame-saver':
                    saved = info.get('frames_saved', 0)
                    dropped = info.get('frames_dropped', 0)
                    io_errors = info.get('io_errors', 0)
                    save_time = info.get('save_time_ms', 0)
                    disk_mb = info.get('disk_usage_mb', 0)

                    lines.append(f"  Saved: {saved} | Dropped: {dropped} | IO Errors: {io_errors}")
                    lines.append(f"  Save time: {save_time:.1f} ms | Disk usage: {disk_mb:.1f} MB")
                    lines.append(f"  Output: {info.get('output_dir', 'unknown')}")

                    # Add frame pipeline tracking for frame-saver
                    tracked_frames = info.get('tracked_frames', 0)
                    if tracked_frames >= 0:
                        avg_total = info.get('avg_total_latency_ms', 0)
                        min_total = info.get('min_total_latency_ms', 0)
                        max_total = info.get('max_total_latency_ms', 0)
                        avg_publish = info.get('avg_publish_latency_ms', 0)
                        avg_resize = info.get('avg_resize_latency_ms', 0)
                        avg_save = info.get('avg_save_latency_ms', 0)

                        lines.append("")
                        lines.append("  Frame Pipeline Tracking:")
                        lines.append(f"    End-to-End Latency: {avg_total:.1f}ms "
                                    f"(min: {min_total:.1f}ms, max: {max_total:.1f}ms)")
                        lines.append("    Stage Breakdown:")
                        lines.append(f"      - Read→Publish: {avg_publish:.1f}ms")
                        lines.append(f"      - Publish→Resize: {avg_resize:.1f}ms")
                        lines.append(f"      - Resize→Save: {avg_save:.1f}ms")
                        lines.append(f"    Tracked Frames: {tracked_frames}")

                # Database stats
                db_status = info.get('db_status', 'unknown')
                if db_status == 'ok':
                    samples = info.get('db_sample_count', 0)
                    lines.append(f"  DB: {samples} samples in last 10 min")
                else:
                    lines.append(f"  DB: {db_status}")

            else:
                lines.append(f"\n{name.upper()} [{status}]")

        lines.append("\n" + "=" * 80)
        lines.append("Press Ctrl+C to exit")

        # Print all lines
        print('\n'.join(lines))

    def run(self):
        """Main monitoring loop"""
        last_display = time.time()

        print("[Monitor] Starting monitoring loop...", file=sys.stderr)

        while self.running:
            try:
                # Check ZMQ queues if available
                if self.mon_sockets:
                    for port, socket in self.mon_sockets:
                        self._check_zmq_queue(port, socket)

                # Display status periodically
                now = time.time()
                if now - last_display >= self.config['display_interval']:
                    self._print_status()
                    last_display = now

                # Small sleep to prevent CPU spinning
                time.sleep(self.config['update_interval'])

                if not (self.stats_counter % 20):
                    self._cleanup_stats()
                self.stats_counter += 1

            except KeyboardInterrupt:
                break
            except Exception as e:
                print(f"[Monitor] Error in main loop: {e}", file=sys.stderr)
                time.sleep(5)

        self.cleanup()

    def cleanup(self):
        """Clean up all resources"""
        print("\n[Monitor] Cleaning up...", file=sys.stderr)

        # Close ZMQ sockets
        for _, socket in self.mon_sockets:
            try:
                socket.close()
            except:
                pass

        # Terminate ZMQ context
        if self.zmq_context:
            try:
                self.zmq_context.term()
            except:
                pass

        # Clean up metrics collector
        self.metrics_collector.cleanup()

        print("[Monitor] Shutdown complete", file=sys.stderr)

    def _cleanup_stats(self):
        """Periodic stats maintenance to prevent overflow"""
        for port in self.queue_stats:
            # Reset message counter if it gets too large
            if self.queue_stats[port]['messages'] > 1000000:
                print(f"[Monitor] Resetting message counter for port {port}", file=sys.stderr)
                self.queue_stats[port]['messages'] = 0

            # Reset byte counter if it gets too large
            if self.queue_stats[port]['bytes'] > 1024 * 1024 * 1024:  # 1GB
                print(f"[Monitor] Resetting byte counter for port {port}", file=sys.stderr)
                self.queue_stats[port]['bytes'] = 0

            # Reset error counter periodically
            if self.queue_stats[port]['errors'] > 10000:
                print(f"[Monitor] Resetting error counter for port {port}", file=sys.stderr)
                self.queue_stats[port]['errors'] = 0

def main():
    """Entry point"""
    try:
        monitor = QueueMonitor()
        monitor.run()
    except Exception as e:
        print(f"[Monitor] Fatal error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
