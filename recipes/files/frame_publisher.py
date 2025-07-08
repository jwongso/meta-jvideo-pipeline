"""
Optimized Frame Publisher Service
"""

import sys
import time
import cv2
import zmq
import json
import numpy as np
from pathlib import Path
from pipeline_stage import PipelineStage, PublisherMetrics

class FramePublisher(PipelineStage):
    """Video frame publisher service"""

    def __init__(self):
        super().__init__(
            service_name="frame-publisher",
            config_path="/etc/jvideo/frame-publisher.conf",
            metrics_class=PublisherMetrics
        )

        # ZMQ setup
        self.zmq_context = None
        self.zmq_socket = None

        # Video capture
        self.video_cap = None
        self.frame_buffer = None

        # Frame tracking
        self.frame_counter = 0
        self.loop_video = False

    def get_default_config(self) -> dict:
        return {
            'video_input_path': '/usr/share/jvideo/media/matterhorn.mp4',
            'publish_port': 5555,
            'jpeg_quality': 85,
            'frame_delay_ms': 0,
            'loop_video': False,
            'benchmark_export_interval': 1000
        }

    def validate_config(self, config: dict) -> bool:
        """Validate configuration"""
        if 'publish_port' in config:
            port = config['publish_port']
            if not (1024 <= port <= 65535):
                self.logger.error(f"Invalid port: {port}")
                return False

        if 'jpeg_quality' in config:
            quality = config['jpeg_quality']
            if not (0 <= quality <= 100):
                self.logger.error(f"Invalid JPEG quality: {quality}")
                return False

        return True

    def get_database_schema(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS publisher_benchmarks (
                timestamp INTEGER PRIMARY KEY,
                frames_published INTEGER,
                total_frames INTEGER,
                current_fps REAL,
                video_fps REAL,
                errors INTEGER,
                uptime_seconds INTEGER,
                memory_usage_kb INTEGER
            );
            CREATE INDEX IF NOT EXISTS idx_timestamp ON publisher_benchmarks(timestamp);
        """

    def store_benchmark_data(self, metrics: PublisherMetrics):
        """Store metrics to database"""
        uptime = int(time.time() - metrics.service_start_time / 1e9)

        self.db_conn.execute("""
            INSERT INTO publisher_benchmarks
            (timestamp, frames_published, total_frames, current_fps, video_fps,
             errors, uptime_seconds, memory_usage_kb)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            int(time.time()),
            metrics.frames_published,
            metrics.total_frames,
            metrics.current_fps,
            metrics.video_fps,
            metrics.errors,
            uptime,
            self.get_memory_usage()
        ))
        self.db_conn.commit()

    def on_start(self):
        """Initialize ZMQ and open video"""
        # Setup ZMQ
        self.zmq_context = zmq.Context(1)
        self.zmq_socket = self.zmq_context.socket(zmq.PUB)
        self.zmq_socket.setsockopt(zmq.SNDHWM, 100)
        self.zmq_socket.setsockopt(zmq.LINGER, 1000)

        port = self.config['publish_port']
        self.zmq_socket.bind(f"tcp://*:{port}")
        self.logger.info(f"ZMQ socket bound to port {port}")

        # Give subscribers time to connect
        time.sleep(0.5)

        # Open video
        video_path = self.config['video_input_path']
        if not self.open_video(video_path):
            raise RuntimeError(f"Failed to open video: {video_path}")

        # Cache config values
        self.loop_video = self.config.get('loop_video', False)

    def open_video(self, video_path: str) -> bool:
        """Open video file and update metrics"""
        self.logger.info(f"Opening video: {video_path}")

        if not Path(video_path).exists():
            self.logger.error(f"Video file not found: {video_path}")
            return False

        self.video_cap = cv2.VideoCapture(video_path)
        if not self.video_cap.isOpened():
            self.logger.error(f"Failed to open video: {video_path}")
            return False

        # Minimize buffering
        self.video_cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        # Get video properties
        width = int(self.video_cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(self.video_cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        fps = self.video_cap.get(cv2.CAP_PROP_FPS)
        frame_count = int(self.video_cap.get(cv2.CAP_PROP_FRAME_COUNT))

        # Fix invalid frame count
        if frame_count < 0 or frame_count > 1000000:
            frame_count = 0

        # Pre-allocate frame buffer
        self.frame_buffer = np.empty((height, width, 3), dtype=np.uint8)

        self.logger.info(f"Video opened: {width}x{height} @ {fps:.1f} FPS, "
                        f"{frame_count if frame_count > 0 else 'unknown'} frames")

        # Update metrics
        self.metrics_mgr.update(
            total_frames=frame_count,
            video_fps=fps,
            video_width=width,
            video_height=height,
            video_path=video_path,
            video_healthy=True
        )
        self.metrics_mgr.commit()

        return True

    def process_frame(self) -> bool:
        """Read and publish a frame"""
        # Track timing
        read_start = time.time()

        # Read frame into pre-allocated buffer
        ret = self.video_cap.read(self.frame_buffer)

        if not ret or self.frame_buffer is None:
            if self.loop_video:
                self.logger.info("End of video, restarting...")
                self.video_cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                self.frame_counter = 0
                return True
            else:
                self.logger.info("End of video reached")
                self.running = False
                return False

        try:
            # Create metadata
            publish_time = time.time()
            metadata = {
                'frame_id': self.frames_processed,
                'timestamp': publish_time,
                'width': self.frame_buffer.shape[1],
                'height': self.frame_buffer.shape[0],
                'channels': self.frame_buffer.shape[2],
                'source': 'mp4',
                'tracking': {
                    'frame_id': int(time.time() * 1e9),
                    'sequence': self.frame_counter,
                    'source_ts': read_start,
                    'publish_ts': publish_time
                }
            }

            # Encode frame
            encode_params = [cv2.IMWRITE_JPEG_QUALITY, self.config['jpeg_quality']]
            _, encoded = cv2.imencode('.jpg', self.frame_buffer, encode_params)

            # Send metadata and frame
            self.zmq_socket.send_json(metadata, zmq.SNDMORE | zmq.NOBLOCK)
            self.zmq_socket.send(encoded.tobytes(), zmq.NOBLOCK)

            self.frames_processed += 1
            self.frame_counter += 1

            # Update metrics
            self.update_metrics()

            # Log progress
            if self.frame_counter % 100 == 0:
                latency_ms = (publish_time - read_start) * 1000
                self.logger.debug(f"Frame {self.frame_counter} - "
                                f"Read→Publish: {latency_ms:.1f}ms")

            # Frame rate limiting
            delay_ms = self.config.get('frame_delay_ms', 0)
            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)

            return True

        except zmq.Again:
            # Socket buffer full, drop frame
            self.logger.warning(f"Dropped frame {self.frame_counter} - slow subscribers")
            return True

        except Exception as e:
            self.logger.error(f"Error processing frame: {e}")
            self.errors += 1
            return True

    def update_service_metrics(self):
        """Update publisher-specific metrics"""
        self.metrics_mgr.update(
            frames_published=self.frames_processed
        )

    def on_stop(self):
        """Clean up resources"""
        if self.video_cap:
            self.video_cap.release()

        if self.zmq_socket:
            self.zmq_socket.close()

        if self.zmq_context:
            self.zmq_context.term()

def main():
    """Entry point"""
    publisher = FramePublisher()
    publisher.run()

if __name__ == "__main__":
    main()
