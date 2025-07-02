#!/usr/bin/env python3
"""Ultra-lightweight Queue Monitor - Yocto optimized"""

import sys
import time
import os
import json
import signal
from collections import deque

sys.path.append('/opt/jvideo/services')
import zmq

# Try Redis but don't fail if not available
try:
    import redis
    REDIS_AVAILABLE = True
except ImportError:
    REDIS_AVAILABLE = False

class YoctoQueueMonitor:
    def __init__(self):
        self.running = True
        self.zmq_context = zmq.Context()
        self.mon_sockets = []

        # Use bounded stats to prevent memory growth
        self.stats = {}

        # CPU tracking for accurate percentage
        self.last_cpu_times = None
        self.last_cpu_time = 0

        # Minimal config
        self.config = {
            'monitor_ports': [5555, 5556],
            'update_interval': 5,  # seconds
            'log_interval': 30,    # seconds
            'max_json_size': 1024, # 1KB max for JSON messages
            'stats_window': 100,   # Keep last 100 measurements
        }

        # Load config if exists
        try:
            with open('/etc/jvideo/queue-monitor.conf') as f:
                config_data = json.load(f)
                self.config.update(config_data)
        except (IOError, json.JSONDecodeError):
            pass

        # Setup signal handlers
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        # Setup monitoring
        self.setup_monitoring()

        # Simple logging
        self.log("Yocto Queue Monitor started")

    def log(self, message):
        """Simple logging to stdout/journal"""
        timestamp = time.strftime('%Y-%m-%d %H:%M:%S')
        print(f"[{timestamp}] {message}", flush=True)

    def signal_handler(self, signum, frame):
        """Handle shutdown signals"""
        self.log(f"Received signal {signum}, shutting down...")
        self.running = False

    def setup_monitoring(self):
        """Setup ZMQ monitoring sockets"""
        for port in self.config['monitor_ports']:
            try:
                socket = self.zmq_context.socket(zmq.SUB)
                socket.setsockopt(zmq.RCVTIMEO, 50)   # 50ms timeout
                socket.setsockopt(zmq.RCVHWM, 1)      # Keep only 1 message
                socket.setsockopt(zmq.LINGER, 0)      # Don't linger on close
                socket.setsockopt(zmq.SNDHWM, 1)      # Limit send buffer too
                socket.connect(f"tcp://localhost:{port}")
                socket.setsockopt_string(zmq.SUBSCRIBE, "")

                self.mon_sockets.append((port, socket))

                # Use bounded deques for latency tracking
                self.stats[port] = {
                    'messages': 0,
                    'last_seen': 0,
                    'latencies': deque(maxlen=self.config['stats_window']),
                    'errors': 0
                }

                self.log(f"Monitoring port {port}")
            except Exception as e:
                self.log(f"Failed to monitor port {port}: {e}")

    def check_queue(self, port, socket):
        """Check a single queue for messages"""
        messages = None
        try:
            # Try to receive without blocking
            messages = socket.recv_multipart(zmq.NOBLOCK, copy=False)

            if len(messages) >= 2:
                # Validate JSON size before parsing
                json_bytes = messages[0].bytes
                if len(json_bytes) > self.config['max_json_size']:
                    self.stats[port]['errors'] += 1
                    return

                # Parse metadata safely
                try:
                    metadata = json.loads(json_bytes.decode('utf-8'))

                    # Update stats
                    self.stats[port]['messages'] += 1
                    self.stats[port]['last_seen'] = time.time()

                    # Calculate latency if timestamp available
                    if 'timestamp' in metadata and isinstance(metadata['timestamp'], (int, float)):
                        latency = (time.time() - metadata['timestamp']) * 1000
                        if 0 <= latency <= 60000:  # Reasonable range: 0-60s
                            self.stats[port]['latencies'].append(latency)

                except (json.JSONDecodeError, UnicodeDecodeError, KeyError, TypeError):
                    self.stats[port]['errors'] += 1

        except zmq.Again:
            pass  # No message available
        except Exception as e:
            self.stats[port]['errors'] += 1
            if self.stats[port]['errors'] % 100 == 0:  # Log every 100 errors
                self.log(f"Errors on port {port}: {self.stats[port]['errors']}")
        finally:
            # Always clean up messages
            if messages:
                for msg in messages:
                    try:
                        msg.close()
                    except:
                        pass
                del messages

    def get_service_status(self):
        """Get basic service status from Redis if available"""
        if not REDIS_AVAILABLE:
            return {}

        try:
            # Use connection pool for efficiency
            r = redis.Redis(decode_responses=True, socket_connect_timeout=1, socket_timeout=1)
            status = {}

            services = ['frame-publisher', 'frame-resizer', 'frame-saver']

            # Use pipeline for efficiency
            pipe = r.pipeline()
            for service in services:
                pipe.hget(f"service:{service}", 'status')
                pipe.hget(f"metrics:{service}", 'fps')
                pipe.hget(f"metrics:{service}", 'frames_processed')

            results = pipe.execute()

            for i, service in enumerate(services):
                idx = i * 3
                status[service] = {
                    'status': results[idx] or 'unknown',
                    'fps': self._safe_float(results[idx + 1]),
                    'frames': self._safe_int(results[idx + 2])
                }

            return status

        except Exception as e:
            # Log Redis errors occasionally
            if not hasattr(self, '_last_redis_error') or time.time() - self._last_redis_error > 300:
                self.log(f"Redis error: {e}")
                self._last_redis_error = time.time()
            return {}

    def _safe_float(self, value):
        """Safely convert to float"""
        try:
            return float(value) if value else 0.0
        except (ValueError, TypeError):
            return 0.0

    def _safe_int(self, value):
        """Safely convert to int"""
        try:
            return int(value) if value else 0
        except (ValueError, TypeError):
            return 0

    def get_system_stats(self):
        """Get system stats efficiently from /proc"""
        stats = {'cpu_percent': 0, 'memory_percent': 0, 'memory_mb': 0}

        # CPU usage with proper calculation
        try:
            with open('/proc/stat', 'r') as f:
                cpu_line = f.readline().strip().split()
                if len(cpu_line) >= 8:
                    # user, nice, system, idle, iowait, irq, softirq, steal
                    times = list(map(int, cpu_line[1:8]))

                    current_time = time.time()
                    if self.last_cpu_times and current_time > self.last_cpu_time:
                        # Calculate deltas
                        deltas = [curr - prev for curr, prev in zip(times, self.last_cpu_times)]
                        total_delta = sum(deltas)
                        idle_delta = deltas[3]  # idle time delta

                        if total_delta > 0:
                            stats['cpu_percent'] = 100.0 * (total_delta - idle_delta) / total_delta

                    self.last_cpu_times = times
                    self.last_cpu_time = current_time
        except (IOError, ValueError, IndexError):
            pass

        # Memory from /proc/meminfo (more efficient parsing)
        try:
            mem_info = {}
            with open('/proc/meminfo', 'r') as f:
                for line in f:
                    if line.startswith('MemTotal:') or line.startswith('MemAvailable:'):
                        parts = line.split()
                        if len(parts) >= 2:
                            key = parts[0].rstrip(':')
                            value = int(parts[1])  # KB
                            mem_info[key] = value

                        # Early exit if we have both values
                        if len(mem_info) == 2:
                            break

            if 'MemTotal' in mem_info and 'MemAvailable' in mem_info:
                total_kb = mem_info['MemTotal']
                available_kb = mem_info['MemAvailable']
                used_kb = total_kb - available_kb

                stats['memory_percent'] = (100.0 * used_kb / total_kb) if total_kb > 0 else 0
                stats['memory_mb'] = total_kb / 1024

        except (IOError, ValueError, KeyError):
            pass

        return stats

    def print_status(self):
        """Print current status efficiently"""
        # Get stats
        sys_stats = self.get_system_stats()
        services = self.get_service_status()

        # Build status lines
        lines = [
            "=" * 50,
            f"Queue Monitor - {time.strftime('%H:%M:%S')}",
            f"System: CPU {sys_stats['cpu_percent']:.1f}% | Memory {sys_stats['memory_percent']:.1f}%",
            "-" * 50
        ]

        # Queue stats
        for port, stats in self.stats.items():
            # Calculate average latency from recent samples
            latencies = stats['latencies']
            avg_latency = sum(latencies) / len(latencies) if latencies else 0

            # Time since last message
            time_since = time.time() - stats['last_seen'] if stats['last_seen'] > 0 else -1

            if time_since < 0:
                last_seen = "Never"
            elif time_since < 60:
                last_seen = f"{time_since:.0f}s"
            else:
                last_seen = f"{time_since/60:.0f}m"

            lines.append(
                f"Port {port}: {stats['messages']} msgs | "
                f"{avg_latency:.1f}ms | {last_seen} | "
                f"err:{stats['errors']}"
            )

        # Service stats (compact)
        if services:
            service_line = "Services: "
            for name, info in services.items():
                short_name = name.split('-')[1] if '-' in name else name
                service_line += f"{short_name}:{info['status'][0].upper()}/{info['fps']:.0f} "
            lines.append(service_line)

        # Print all at once
        self.log('\n'.join(lines))

    def run(self):
        """Main monitoring loop"""
        last_log = time.time()
        loop_count = 0

        while self.running:
            try:
                # Check all queues
                for port, socket in self.mon_sockets:
                    self.check_queue(port, socket)

                # Log status periodically
                now = time.time()
                if now - last_log >= self.config['log_interval']:
                    self.print_status()
                    last_log = now

                # Periodic cleanup to prevent any memory growth
                loop_count += 1
                if loop_count % 1000 == 0:  # Every ~1.4 hours at 5s intervals
                    self._cleanup_stats()

                # Sleep to reduce CPU usage
                time.sleep(self.config['update_interval'])

            except KeyboardInterrupt:
                break
            except Exception as e:
                self.log(f"Error in main loop: {e}")
                time.sleep(5)

        # Cleanup
        self.cleanup()

    def _cleanup_stats(self):
        """Periodic cleanup to prevent any memory growth"""
        for port in self.stats:
            # Reset counters if they get too large
            if self.stats[port]['messages'] > 1000000:
                self.stats[port]['messages'] = 0
            if self.stats[port]['errors'] > 10000:
                self.stats[port]['errors'] = 0

    def cleanup(self):
        """Clean shutdown"""
        self.log("Shutting down...")

        # Close sockets
        for _, socket in self.mon_sockets:
            try:
                socket.close()
            except:
                pass

        # Close context
        try:
            self.zmq_context.term()
        except:
            pass

        self.log("Queue Monitor stopped")


def main():
    """Main entry point"""
    try:
        monitor = YoctoQueueMonitor()
        monitor.run()
    except Exception as e:
        print(f"Fatal error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
