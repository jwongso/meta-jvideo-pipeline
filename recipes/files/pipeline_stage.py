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
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Dict, Any, Optional, Tuple
from dataclasses import dataclass, asdict
import struct
import mmap
import msgpack
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
    """Manages shared memory metrics using MessagePack"""

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

    def _read_from_shm(self):
        """Read metrics from shared memory"""
        with self._lock:
            self.mm.seek(64)  # Skip mutex area
            size_bytes = self.mm.read(8)
            if len(size_bytes) < 8:
                return None

            size = struct.unpack('Q', size_bytes)[0]
            if size == 0 or size > 8192:
                return None

            data = self.mm.read(size)
            metrics_dict = msgpack.unpackb(data, raw=False)
            return self.metrics_class(**metrics_dict)

    def commit(self):
        """Write metrics to shared memory"""
        if not self.mm or self.mm.closed:
            return

        with self._lock:
            self.metrics.last_update_time = int(time.time() * 1e9)
            data = msgpack.packb(asdict(self.metrics))

            self.mm.seek(64)  # Skip mutex area
            self.mm.write(struct.pack('Q', len(data)))
            self.mm.write(data)
            self.mm.flush()

    def update(self, **kwargs):
        """Update metrics fields"""
        for key, value in kwargs.items():
            if hasattr(self.metrics, key):
                setattr(self.metrics, key, value)

    def get_metrics(self) -> Optional[BaseMetrics]:
        """Get current metrics"""
        if self.mm and not self.mm.closed and self.mm.readable():
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

        # Counters
        self.frames_processed = 0
        self.errors = 0

        # Configuration
        self.config = {}
        self.db_path = ""
        self.export_interval = 1000

        # Database
        self.db_conn = None
        self.db_queue = queue.Queue()
        self.db_thread = None

        # Metrics
        self.metrics_mgr = None

        # Process monitor
        self.process = psutil.Process()

        # Setup signal handlers
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

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
                                       f"/var/lib/jvideo/db/{self.service_name}_benchmarks.db")
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

            # Create database
            self.db_conn = sqlite3.connect(':memory:')
            self.db_conn.execute("PRAGMA synchronous = OFF")
            self.db_conn.execute("PRAGMA journal_mode = MEMORY")

            # Create schema
            self.db_conn.executescript(self.get_database_schema())

            # Start background thread for database exports
            self.db_thread = threading.Thread(target=self._db_worker, daemon=True)
            self.db_thread.start()

            # Force initial export
            self.export_database()

        except Exception as e:
            self.logger.error(f"Database initialization failed: {e}")

    def _db_worker(self):
        """Background worker for database operations"""
        while not self._shutdown_event.is_set():
            try:
                # Wait for export request
                task = self.db_queue.get(timeout=1.0)
                if task == 'export':
                    self._export_database_sync()
                elif task == 'shutdown':
                    break
            except queue.Empty:
                continue
            except Exception as e:
                self.logger.error(f"Database worker error: {e}")

    def _export_database_sync(self):
        """Synchronously export database to file"""
        try:
            file_conn = sqlite3.connect(self.db_path)

            # Create schema in file database
            file_conn.executescript(self.get_database_schema())

            # Copy data
            self.db_conn.backup(file_conn)
            file_conn.close()

            self.logger.debug(f"Exported database to {self.db_path}")

        except Exception as e:
            self.logger.error(f"Database export failed: {e}")

    def export_database(self):
        """Queue database export"""
        try:
            self.db_queue.put_nowait('export')
        except queue.Full:
            pass  # Skip if queue is full

    def store_benchmark(self):
        """Store current metrics to database"""
        if not self.db_conn or not self.metrics_mgr:
            return

        try:
            metrics = self.metrics_mgr.get_metrics()
            if metrics:
                self.store_benchmark_data(metrics)
        except Exception as e:
            self.logger.error(f"Failed to store benchmark: {e}")

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
            self.export_database()

            self.logger.info(f"Processed {self.frames_processed} frames, "
                           f"FPS: {current_fps:.1f}")

    def run(self):
        """Main run loop"""
        self.initialize()
        self.on_start()

        self.logger.info("Starting processing...")

        try:
            while self.running:
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
            self.db_queue.put('shutdown')

        if self.db_thread:
            self.db_thread.join(timeout=5.0)

        # Final database export
        if self.db_conn:
            self._export_database_sync()
            self.db_conn.close()

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
        """Store metrics to database"""
        pass

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
