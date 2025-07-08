#!/usr/bin/env python3
"""
Base class for video pipeline stages - Python implementation
"""

import json
import signal
import sys
import time
import os
import sqlite3
import threading
import queue
import logging
import struct
import mmap
import fcntl
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Dict, Any, Optional, Tuple
from dataclasses import dataclass, asdict

# Try to import msgpack, fall back to json if not available
try:
    import msgpack
    USE_MSGPACK = True
except ImportError:
    USE_MSGPACK = False
    print("WARNING: msgpack not available, falling back to JSON (less efficient)")

import psutil
import zmq

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] [%(name)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)

@dataclass
class BaseMetrics:
    """Base metrics shared by all services"""
    service_name: str
    service_pid: int
    errors: int
    current_fps: float
    last_update_time: int
    service_start_time: int

@dataclass
class PublisherMetrics(BaseMetrics):
    """Publisher-specific metrics"""
    frames_published: int = 0
    total_frames: int = 0
    video_fps: float = 0.0
    video_width: int = 0
    video_height: int = 0
    video_healthy: bool = True
    video_path: str = ""

@dataclass
class ResizerMetrics(BaseMetrics):
    """Resizer-specific metrics"""
    frames_processed: int = 0
    frames_dropped: int = 0
    processing_time_ms: float = 0.0
    input_width: int = 0
    input_height: int = 0
    output_width: int = 0
    output_height: int = 0
    service_healthy: bool = True

@dataclass
class SaverMetrics(BaseMetrics):
    """Saver-specific metrics"""
    frames_saved: int = 0
    frames_dropped: int = 0
    io_errors: int = 0
    save_time_ms: float = 0.0
    disk_usage_mb: float = 0.0
    frame_width: int = 0
    frame_height: int = 0
    frame_channels: int = 0
    disk_healthy: bool = True
    output_dir: str = ""
    format: str = "jpg"
    avg_total_latency_ms: float = 0.0
    min_total_latency_ms: float = float('inf')
    max_total_latency_ms: float = 0.0
    avg_publish_latency_ms: float = 0.0
    avg_resize_latency_ms: float = 0.0
    avg_save_latency_ms: float = 0.0
    tracked_frames: int = 0


class MetricsManager:
    """Manages shared memory metrics using MessagePack or JSON"""

    def __init__(self, service_name: str, metrics_class: type, create: bool = True):
        self.service_name = service_name
        self.metrics_class = metrics_class
        self.shm_path = f"/dev/shm/jvideo_{service_name}_metrics"
        self.metrics = None
        self.mm = None
        self.file = None
        self._lock = threading.Lock()

        if create:
            self._create_metrics()
        else:
            self._open_metrics()

    def _create_metrics(self):
        """Create new metrics in shared memory"""
        self.metrics = self.metrics_class(
            service_name=self.service_name,
            service_pid=os.getpid(),
            errors=0,
            current_fps=0.0,
            last_update_time=int(time.time() * 1e9),
            service_start_time=int(time.time() * 1e9)
        )

        # Create shared memory file
        with open(self.shm_path, 'wb') as f:
            f.write(b'\0' * 65536)  # 64KB

        self.file = open(self.shm_path, 'r+b')
        self.mm = mmap.mmap(self.file.fileno(), 0)
        self.commit()

    def _open_metrics(self):
        """Open existing metrics from shared memory"""
        if not os.path.exists(self.shm_path):
            raise FileNotFoundError(f"Shared memory not found: {self.shm_path}")

        self.file = open(self.shm_path, 'rb')
        self.mm = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        self.metrics = self._read_from_shm()

    def _get_ordered_dict(self, metrics):
        """Convert metrics to OrderedDict with correct field order"""
        from collections import OrderedDict

        # Base fields (same for all)
        result = OrderedDict([
            ('service_name', metrics.service_name),
            ('service_pid', metrics.service_pid),
            ('errors', metrics.errors),
            ('current_fps', metrics.current_fps),
            ('last_update_time', metrics.last_update_time),
            ('service_start_time', metrics.service_start_time),
        ])

        # Service-specific fields
        if isinstance(metrics, PublisherMetrics):
            result.update([
                ('frames_published', metrics.frames_published),
                ('total_frames', metrics.total_frames),
                ('video_fps', metrics.video_fps),
                ('video_width', metrics.video_width),
                ('video_height', metrics.video_height),
                ('video_healthy', metrics.video_healthy),
                ('video_path', metrics.video_path),
            ])
        elif isinstance(metrics, ResizerMetrics):
            result.update([
                ('frames_processed', metrics.frames_processed),
                ('frames_dropped', metrics.frames_dropped),
                ('processing_time_ms', metrics.processing_time_ms),
                ('input_width', metrics.input_width),
                ('input_height', metrics.input_height),
                ('output_width', metrics.output_width),
                ('output_height', metrics.output_height),
                ('service_healthy', metrics.service_healthy),
            ])
        elif isinstance(metrics, SaverMetrics):
            result.update([
                ('frames_saved', metrics.frames_saved),
                ('frames_dropped', metrics.frames_dropped),
                ('io_errors', metrics.io_errors),
                ('save_time_ms', metrics.save_time_ms),
                ('disk_usage_mb', metrics.disk_usage_mb),
                ('frame_width', metrics.frame_width),
                ('frame_height', metrics.frame_height),
                ('frame_channels', metrics.frame_channels),
                ('disk_healthy', metrics.disk_healthy),
                ('output_dir', metrics.output_dir),
                ('format', metrics.format),
                ('avg_total_latency_ms', metrics.avg_total_latency_ms),
                ('min_total_latency_ms', metrics.min_total_latency_ms),
                ('max_total_latency_ms', metrics.max_total_latency_ms),
                ('avg_publish_latency_ms', metrics.avg_publish_latency_ms),
                ('avg_resize_latency_ms', metrics.avg_resize_latency_ms),
                ('avg_save_latency_ms', metrics.avg_save_latency_ms),
                ('tracked_frames', metrics.tracked_frames),
            ])

        return result

    def commit(self):
        """Write metrics to shared memory"""
        if not self.mm or self.mm.closed:
            return

        with self._lock:
            self.metrics.last_update_time = int(time.time() * 1e9)

            # Use ordered dict to ensure consistent field order
            ordered_data = self._get_ordered_dict(self.metrics)

            if USE_MSGPACK:
                data = msgpack.packb(ordered_data)
            else:
                data = json.dumps(ordered_data).encode('utf-8')

            # File locking for multi-process safety
            fcntl.flock(self.file.fileno(), fcntl.LOCK_EX)
            try:
                self.mm.seek(64)  # Skip reserved area
                self.mm.write(struct.pack('<Q', len(data)))  # Little-endian
                self.mm.write(data)
                self.mm.flush()
            finally:
                fcntl.flock(self.file.fileno(), fcntl.LOCK_UN)

    def _read_from_shm(self):
        """Read metrics from shared memory"""
        with self._lock:
            fcntl.flock(self.file.fileno(), fcntl.LOCK_SH)
            try:
                self.mm.seek(64)  # Skip reserved area
                size_bytes = self.mm.read(8)
                if len(size_bytes) < 8:
                    return None

                size = struct.unpack('<Q', size_bytes)[0]  # Little-endian
                if size == 0 or size > 8192:
                    return None

                data = self.mm.read(size)

                if USE_MSGPACK:
                    metrics_dict = msgpack.unpackb(data, raw=False)
                else:
                    metrics_dict = json.loads(data.decode('utf-8'))

                return self.metrics_class(**metrics_dict)
            finally:
                fcntl.flock(self.file.fileno(), fcntl.LOCK_UN)

    def update(self, **kwargs):
        """Update metrics fields"""
        for key, value in kwargs.items():
            if hasattr(self.metrics, key):
                setattr(self.metrics, key, value)

    def get_metrics(self) -> Optional[BaseMetrics]:
        """Get current metrics"""
        if self.mm and not self.mm.closed:
            return self._read_from_shm()
        return self.metrics

    def close(self):
        """Clean up resources"""
        if self.mm:
            self.mm.close()
        if self.file:
            self.file.close()


class PipelineStage(ABC):
    """Base class for all pipeline stages"""

    def __init__(self, service_name: str, config_path: str, metrics_class: type):
        self.service_name = service_name
        self.config_path = config_path
        self.metrics_class = metrics_class
        self.logger = logging.getLogger(service_name)

        # Control flags
        self.running = True
        self._shutdown_event = threading.Event()

        # Timing
        self.start_time = time.time()
        self.last_fps_update = self.start_time
        self.last_watchdog = self.start_time

        # Counters
        self.frames_processed = 0
        self.errors = 0

        # Configuration
        self.config = {}
        self.db_path = ""
        self.export_interval = 1000

        # Database
        self.db_queue = queue.Queue()
        self.db_thread = None

        # Metrics
        self.metrics_mgr = None

        # Process monitor
        self.process = psutil.Process()

        # Systemd watchdog
        self.watchdog_interval = None
        self._setup_watchdog()

        # Setup signal handlers
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _setup_watchdog(self):
        """Setup systemd watchdog if enabled"""
        watchdog_usec = os.environ.get('WATCHDOG_USEC')
        if watchdog_usec:
            # Use half the watchdog interval for safety
            self.watchdog_interval = int(watchdog_usec) / 1000000 / 2
            self.logger.info(f"Systemd watchdog enabled, interval: {self.watchdog_interval:.1f}s")

    def _notify_watchdog(self):
        """Notify systemd watchdog that we're alive"""
        if self.watchdog_interval:
            now = time.time()
            if now - self.last_watchdog >= self.watchdog_interval:
                try:
                    # Notify systemd
                    notify_socket = os.environ.get('NOTIFY_SOCKET')
                    if notify_socket:
                        import socket
                        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
                        sock.sendto(b'WATCHDOG=1', notify_socket)
                        sock.close()
                        self.last_watchdog = now
                except Exception as e:
                    self.logger.error(f"Failed to notify watchdog: {e}")

    def _signal_handler(self, signum, frame):
        """Handle shutdown signals"""
        self.logger.info(f"Received signal {signum}, shutting down...")
        self.running = False
        self._shutdown_event.set()

    def initialize(self):
        """Initialize the service"""
        self.load_config()
        self.initialize_metrics()
        self.initialize_database()
        self.logger.info("Service initialized")

    def load_config(self):
        """Load configuration from file"""
        self.config = self.get_default_config()

        try:
            if os.path.exists(self.config_path):
                with open(self.config_path, 'r') as f:
                    user_config = json.load(f)
                    if self.validate_config(user_config):
                        self.config.update(user_config)
                        self.logger.info(f"Loaded config from {self.config_path}")
        except Exception as e:
            self.logger.error(f"Config error: {e}, using defaults")

        # Extract common settings
        self.db_path = self.config.get('benchmark_db_path',
                                       f"/var/lib/jvideo/db/{self.service_name.replace('-', '_')}_benchmarks.db")
        self.export_interval = self.config.get('benchmark_export_interval', 1000)

    def initialize_metrics(self):
        """Initialize metrics manager"""
        try:
            self.metrics_mgr = MetricsManager(self.service_name, self.metrics_class)
            self.logger.info("Metrics manager initialized")
        except Exception as e:
            self.logger.error(f"Failed to initialize metrics: {e}")

    def initialize_database(self):
        """Initialize SQLite database"""
        try:
            # Create directory if needed
            Path(self.db_path).parent.mkdir(parents=True, exist_ok=True)

            # Start background thread for database operations
            self.db_thread = threading.Thread(target=self._db_worker, daemon=True)
            self.db_thread.start()

        except Exception as e:
            self.logger.error(f"Database initialization failed: {e}")

    def _db_worker(self):
        """Background worker for database operations"""
        # Create database connection in this thread
        db_conn = None
        try:
            db_conn = sqlite3.connect(self.db_path)
            db_conn.execute("PRAGMA synchronous = OFF")
            db_conn.execute("PRAGMA journal_mode = WAL")

            # Create schema
            db_conn.executescript(self.get_database_schema())
            db_conn.commit()

            self.logger.info("Database worker started")

            while not self._shutdown_event.is_set():
                try:
                    # Wait for task
                    task = self.db_queue.get(timeout=1.0)

                    if task['type'] == 'store':
                        self._store_benchmark_internal(db_conn, task['metrics'])
                    elif task['type'] == 'shutdown':
                        break

                except queue.Empty:
                    continue
                except Exception as e:
                    self.logger.error(f"Database worker error: {e}")

        except Exception as e:
            self.logger.error(f"Database worker initialization failed: {e}")
        finally:
            if db_conn:
                db_conn.close()
            self.logger.info("Database worker stopped")

    def _store_benchmark_internal(self, db_conn, metrics):
        """Store benchmark data (called in db worker thread)"""
        try:
            self.store_benchmark_data_internal(db_conn, metrics)
            db_conn.commit()
        except Exception as e:
            self.logger.error(f"Failed to store benchmark: {e}")

    def store_benchmark_data_internal(self, db_conn, metrics):
        """Store metrics to database (to be overridden by subclasses)"""
        # This will be implemented by derived classes
        pass

    def store_benchmark(self):
        """Queue benchmark storage"""
        if not self.metrics_mgr:
            return

        try:
            metrics = self.metrics_mgr.get_metrics()
            if metrics:
                self.db_queue.put_nowait({
                    'type': 'store',
                    'metrics': metrics
                })
        except queue.Full:
            self.logger.warning("Database queue full, dropping benchmark")
        except Exception as e:
            self.logger.error(f"Failed to queue benchmark: {e}")

    def update_metrics(self):
        """Update metrics and store benchmark"""
        if not self.metrics_mgr:
            return

        # Calculate FPS
        elapsed = time.time() - self.start_time
        current_fps = self.frames_processed / elapsed if elapsed > 0 else 0

        # Update common metrics
        self.metrics_mgr.update(
            errors=self.errors,
            current_fps=current_fps
        )

        # Let derived class update specific metrics
        self.update_service_metrics()

        # Commit to shared memory
        self.metrics_mgr.commit()

        # Store benchmark periodically
        if self.frames_processed % self.export_interval == 0:
            self.store_benchmark()
            self.logger.info(f"Processed {self.frames_processed} frames, "
                           f"FPS: {current_fps:.1f}")

    def run(self):
        """Main run loop"""
        self.initialize()
        self.on_start()

        self.logger.info("Starting processing...")

        try:
            while self.running:
                # Notify watchdog
                self._notify_watchdog()

                if not self.process_frame():
                    time.sleep(0.001)  # Brief sleep if no frame

        except KeyboardInterrupt:
            self.logger.info("Interrupted by user")
        except Exception as e:
            self.logger.error(f"Fatal error: {e}")
        finally:
            self.on_stop()
            self.cleanup()

    def cleanup(self):
        """Clean up resources"""
        self.logger.info("Cleaning up...")

        # Final metrics update
        if self.metrics_mgr:
            self.update_metrics()
            self.store_benchmark()

        # Shutdown database worker
        if self.db_queue:
            self.db_queue.put({'type': 'shutdown'})

        if self.db_thread:
            self.db_thread.join(timeout=5.0)

        # Close metrics
        if self.metrics_mgr:
            self.metrics_mgr.close()

        self.logger.info("Shutdown complete")

    def get_memory_usage(self) -> int:
        """Get current memory usage in KB"""
        return self.process.memory_info().rss // 1024

    # Abstract methods to be implemented by derived classes
    @abstractmethod
    def get_default_config(self) -> Dict[str, Any]:
        """Return default configuration"""
        pass

    @abstractmethod
    def validate_config(self, config: Dict[str, Any]) -> bool:
        """Validate configuration"""
        return True

    @abstractmethod
    def get_database_schema(self) -> str:
        """Return SQL schema for benchmarks"""
        pass

    @abstractmethod
    def store_benchmark_data(self, metrics: BaseMetrics):
        """Store metrics to database (compatibility method)"""
        pass

    def store_benchmark_data_internal(self, db_conn, metrics: BaseMetrics):
        """Store metrics to database using provided connection"""
        # Default implementation calls the old method for compatibility
        # Subclasses should override this to use db_conn directly
        self.store_benchmark_data(metrics)

    @abstractmethod
    def process_frame(self) -> bool:
        """Process a single frame. Return True if frame was processed"""
        pass

    @abstractmethod
    def update_service_metrics(self):
        """Update service-specific metrics"""
        pass

    def on_start(self):
        """Called when service starts"""
        pass

    def on_stop(self):
        """Called when service stops"""
        pass
