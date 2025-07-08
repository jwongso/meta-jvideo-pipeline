#!/usr/bin/env python3
"""
Optimized Frame Saver Service
"""

import time
import cv2
import zmq
import json
import numpy as np
import shutil
from pathlib import Path
from datetime import datetime
from collections import deque
from pipeline_stage import PipelineStage, SaverMetrics

class FrameSaver(PipelineStage):
    """Frame saving service with tracking"""

    def __init__(self):
        super().__init__(
            service_name="frame-saver",
            config_path="/etc/jvideo/frame-saver.conf",
            metrics_class=SaverMetrics
        )

        # ZMQ setup
        self.zmq_context = None
        self.sub_socket = None

        # Frame processing
        self.frames_dropped = 0
        self.io_errors = 0
        self.last_save_time_ms = 0.0

        # Output configuration
        self.output_dir = None
        self.format = 'jpg'

        # Tracking metrics
        self.total_latencies = deque(maxlen=1000)
        self.publish_latencies = deque(maxlen=1000)
        self.resize_latencies = deque(maxlen=1000)
        self.save_latencies = deque(maxlen=1000)
        self.min_latency_ms = float('inf')
        self.max_latency_ms = 0.0
        self.tracked_frames = 0

        # Pre-allocated decode buffer
        self.decode_buffer = None

        # File naming
        self.filename_template = None
        self.filename_counter = 0

    def get_default_config(self) -> dict:
        return {
            'subscribe_port': 5556,
            'subscribe_host': 'localhost',
            'output_dir': '/var/lib/jvideo/frames',
            'format': 'jpg',
            'filename_pattern': 'frame_{timestamp}_{sequence}',
            'max_files': 10000,
            'auto_cleanup': True,
            'benchmark_export_interval': 1000
        }

    def validate_config(self, config: dict) -> bool:
        """Validate configuration"""
        # Validate port
        if 'subscribe_port' in config:
            port = config['subscribe_port']
            if not (1024 <= port <= 65535):
                self.logger.error(f"Invalid subscribe_port: {port}")
                return False

        # Validate format
        if 'format' in config:
            valid_formats = ['jpg', 'jpeg', 'png', 'bmp']
            if config['format'] not in valid_formats:
                self.logger.error(f"Invalid format: {config['format']}")
                return False

        # Validate output directory
        if 'output_dir' in config:
            try:
                Path(config['output_dir']).mkdir(parents=True, exist_ok=True)
            except Exception as e:
                self.logger.error(f"Cannot create output directory: {e}")
                return False

        return True

    def get_database_schema(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS saver_benchmarks (
                timestamp INTEGER PRIMARY KEY,
                frames_saved INTEGER,
                current_fps REAL,
                save_time_ms REAL,
                disk_usage_mb REAL,
                frame_width INTEGER,
                frame_height INTEGER,
                frame_channels INTEGER,
                io_errors INTEGER
            );
            CREATE INDEX IF NOT EXISTS idx_timestamp ON saver_benchmarks(timestamp);
        """

    def store_benchmark_data(self, metrics: SaverMetrics):
        """Store metrics to database (compatibility method)"""
        # This is kept for compatibility but won't be used
        # The actual work is done in store_benchmark_data_internal
        pass

    def store_benchmark_data_internal(self, db_conn, metrics: SaverMetrics):
        """Store metrics to database using provided connection"""
        timestamp_ns = int(time.time() * 1e9)

        db_conn.execute("""
            INSERT OR REPLACE INTO saver_benchmarks
            (timestamp, frames_saved, current_fps, save_time_ms, disk_usage_mb,
            frame_width, frame_height, frame_channels, io_errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            timestamp_ns,
            metrics.frames_saved,
            metrics.current_fps,
            metrics.save_time_ms,
            metrics.disk_usage_mb,
            metrics.frame_width,
            metrics.frame_height,
            metrics.frame_channels,
            metrics.io_errors
        ))

    def on_start(self):
        """Initialize ZMQ socket and output directory"""
        # Setup ZMQ
        self.zmq_context = zmq.Context(1)
        self.sub_socket = self.zmq_context.socket(zmq.SUB)
        self.sub_socket.setsockopt(zmq.RCVHWM, 100)
        self.sub_socket.setsockopt(zmq.SUBSCRIBE, b'')
        self.sub_socket.setsockopt(zmq.RCVTIMEO, 100)  # 100ms timeout

        host = self.config['subscribe_host']
        port = self.config['subscribe_port']
        self.sub_socket.connect(f"tcp://{host}:{port}")
        self.logger.info(f"Connected to resizer at {host}:{port}")

        # Setup output directory
        self.output_dir = Path(self.config['output_dir'])
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.logger.info(f"Output directory: {self.output_dir}")

        # Get format
        self.format = self.config.get('format', 'jpg')

        # Setup filename template
        pattern = self.config.get('filename_pattern', 'frame_{timestamp}_{sequence}')
        self.filename_template = pattern + '.{ext}'

        # Update initial metrics
        self.metrics_mgr.update(
            output_dir=str(self.output_dir),
            format=self.format,
            disk_healthy=True
        )
        self.metrics_mgr.commit()

        # Cleanup old files if needed
        if self.config.get('auto_cleanup', True):
            self.cleanup_old_files()

    def cleanup_old_files(self):
        """Remove old files if exceeding max_files limit"""
        max_files = self.config.get('max_files', 10000)

        try:
            # Get all image files
            files = list(self.output_dir.glob(f'*.{self.format}'))

            if len(files) > max_files:
                # Sort by modification time
                files.sort(key=lambda f: f.stat().st_mtime)

                # Remove oldest files
                files_to_remove = len(files) - max_files
                for f in files[:files_to_remove]:
                    f.unlink()

                self.logger.info(f"Cleaned up {files_to_remove} old files")

        except Exception as e:
            self.logger.error(f"Cleanup failed: {e}")

    def generate_filename(self, frame_id: int) -> Path:
        """Generate unique filename for frame"""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]

        filename = self.filename_template.format(
            timestamp=timestamp,
            sequence=f"{self.filename_counter:06d}",
            frame_id=f"{frame_id:06d}",
            ext=self.format
        )

        self.filename_counter += 1
        return self.output_dir / filename

    def calculate_disk_usage(self) -> float:
        """Calculate disk usage in MB"""
        try:
            usage = shutil.disk_usage(self.output_dir)
            return (usage.total - usage.free) / (1024 * 1024)
        except Exception:
            return 0.0

    def update_tracking_metrics(self, total_ms: float, publish_ms: float,
                              resize_ms: float, save_ms: float):
        """Update frame tracking metrics"""
        self.total_latencies.append(total_ms)
        self.publish_latencies.append(publish_ms)
        self.resize_latencies.append(resize_ms)
        self.save_latencies.append(save_ms)

        self.min_latency_ms = min(self.min_latency_ms, total_ms)
        self.max_latency_ms = max(self.max_latency_ms, total_ms)
        self.tracked_frames += 1

        # Update metrics
        if self.total_latencies:
            self.metrics_mgr.update(
                avg_total_latency_ms=sum(self.total_latencies) / len(self.total_latencies),
                min_total_latency_ms=self.min_latency_ms,
                max_total_latency_ms=self.max_latency_ms,
                avg_publish_latency_ms=sum(self.publish_latencies) / len(self.publish_latencies),
                avg_resize_latency_ms=sum(self.resize_latencies) / len(self.resize_latencies),
                avg_save_latency_ms=sum(self.save_latencies) / len(self.save_latencies),
                tracked_frames=self.tracked_frames
            )

    def process_frame(self) -> bool:
        """Receive and save a frame"""
        try:
            # Receive frame with timeout
            try:
                messages = self.sub_socket.recv_multipart(zmq.NOBLOCK)
            except zmq.Again:
                return False  # No frame available

            if len(messages) < 2:
                self.frames_dropped += 1
                return True

            metadata_bytes, frame_bytes = messages[0], messages[1]

            # Track save timing
            save_start = time.time()

            # Parse metadata
            metadata = json.loads(metadata_bytes)

            # Decode frame
            frame_array = np.frombuffer(frame_bytes, dtype=np.uint8)
            frame = cv2.imdecode(frame_array, cv2.IMREAD_COLOR)

            if frame is None:
                self.frames_dropped += 1
                return True

            # Generate filename
            frame_id = metadata.get('frame_id', self.frames_processed)
            filepath = self.generate_filename(frame_id)

            # Save frame
            try:
                success = cv2.imwrite(str(filepath), frame)

                if not success:
                    raise IOError(f"cv2.imwrite returned False for {filepath}")

                save_end = time.time()
                self.last_save_time_ms = (save_end - save_start) * 1000

                self.frames_processed += 1

                # Update frame properties in metrics
                height, width, channels = frame.shape
                self.metrics_mgr.update(
                    frame_width=width,
                    frame_height=height,
                    frame_channels=channels,
                    frames_saved=self.frames_processed
                )

                # Process tracking if available
                if 'tracking' in metadata:
                    self.process_tracking(metadata, save_end)

                # Update metrics
                self.update_metrics()

                # Periodic cleanup
                if self.frames_processed % 1000 == 0:
                    if self.config.get('auto_cleanup', True):
                        self.cleanup_old_files()

            except Exception as e:
                self.logger.error(f"Failed to save frame: {e}")
                self.io_errors += 1
                self.metrics_mgr.update(
                    io_errors=self.io_errors,
                    disk_healthy=False
                )

            return True

        except Exception as e:
            self.logger.error(f"Error processing frame: {e}")
            self.errors += 1
            return True

    def process_tracking(self, metadata: dict, save_timestamp: float):
        """Process frame tracking information"""
        tracking = metadata['tracking']

        if all(k in tracking for k in ['source_ts', 'publish_ts', 'resize_ts']):
            source_ts = tracking['source_ts']
            publish_ts = tracking['publish_ts']
            resize_ts = tracking['resize_ts']

            # Calculate latencies in milliseconds
            total_latency_ms = (save_timestamp - source_ts) * 1000
            publish_latency_ms = (publish_ts - source_ts) * 1000
            resize_latency_ms = (resize_ts - publish_ts) * 1000
            save_latency_ms = (save_timestamp - resize_ts) * 1000

            # Update tracking metrics
            self.update_tracking_metrics(
                total_latency_ms,
                publish_latency_ms,
                resize_latency_ms,
                save_latency_ms
            )

            # Log complete tracking for sample frames
            seq = tracking.get('sequence', 0)
            if seq % 100 == 0:
                self.logger.info(
                    f"Frame {seq} pipeline journey:\n"
                    f"  - Read→Publish: {publish_latency_ms:.1f}ms\n"
                    f"  - Publish→Resize: {resize_latency_ms:.1f}ms\n"
                    f"  - Resize→Save: {save_latency_ms:.1f}ms\n"
                    f"  - TOTAL: {total_latency_ms:.1f}ms"
                )

    def update_service_metrics(self):
        """Update saver-specific metrics"""
        # Calculate disk usage periodically
        if self.frames_processed % 100 == 0:
            disk_usage = self.calculate_disk_usage()
            self.metrics_mgr.update(disk_usage_mb=disk_usage)

        self.metrics_mgr.update(
            frames_saved=self.frames_processed,
            frames_dropped=self.frames_dropped,
            io_errors=self.io_errors,
            save_time_ms=self.last_save_time_ms
        )

    def on_stop(self):
        """Clean up resources"""
        if self.sub_socket:
            self.sub_socket.close()

        if self.zmq_context:
            self.zmq_context.term()

        # Final cleanup if needed
        if self.config.get('auto_cleanup', True):
            self.cleanup_old_files()

def main():
    """Entry point"""
    saver = FrameSaver()
    saver.run()

if __name__ == "__main__":
    main()
