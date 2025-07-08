#!/usr/bin/env python3
"""
Optimized Frame Resizer Service
"""

import time
import cv2
import zmq
import json
import numpy as np
from collections import deque
from pipeline_stage import PipelineStage, ResizerMetrics

class FrameResizer(PipelineStage):
    """Frame resizing service"""

    def __init__(self):
        super().__init__(
            service_name="frame-resizer",
            config_path="/etc/jvideo/frame-resizer.conf",
            metrics_class=ResizerMetrics
        )

        # ZMQ setup
        self.zmq_context = None
        self.sub_socket = None
        self.pub_socket = None

        # Frame processing
        self.frames_dropped = 0
        self.last_processing_time_ms = 0.0

        # Performance tracking
        self.processing_times = deque(maxlen=100)
        self.adaptive_dropping = False
        self.target_processing_time_ms = 50.0  # Target 20 FPS

        # Pre-allocated buffers
        self.decode_buffer = None
        self.resize_buffer = None

        # Interpolation method
        self.interpolation = cv2.INTER_LINEAR

    def get_default_config(self) -> dict:
        return {
            'subscribe_port': 5555,
            'publish_port': 5556,
            'output_width': 160,
            'output_height': 120,
            'jpeg_quality': 85,
            'interpolation': 'linear',
            'adaptive_dropping': False,
            'target_fps': 20,
            'benchmark_export_interval': 1000
        }

    def validate_config(self, config: dict) -> bool:
        """Validate configuration"""
        # Validate ports
        for port_key in ['subscribe_port', 'publish_port']:
            if port_key in config:
                port = config[port_key]
                if not (1024 <= port <= 65535):
                    self.logger.error(f"Invalid {port_key}: {port}")
                    return False

        # Validate dimensions
        for dim_key in ['output_width', 'output_height']:
            if dim_key in config:
                dim = config[dim_key]
                if not (1 <= dim <= 4096):
                    self.logger.error(f"Invalid {dim_key}: {dim}")
                    return False

        # Validate interpolation method
        if 'interpolation' in config:
            valid_methods = ['nearest', 'linear', 'cubic', 'area', 'lanczos']
            if config['interpolation'] not in valid_methods:
                self.logger.error(f"Invalid interpolation: {config['interpolation']}")
                return False

        return True

    def get_database_schema(self) -> str:
        return """
            CREATE TABLE IF NOT EXISTS resizer_benchmarks (
                timestamp INTEGER PRIMARY KEY,
                frames_processed INTEGER,
                frames_dropped INTEGER,
                current_fps REAL,
                processing_time_ms REAL,
                memory_usage_kb INTEGER,
                input_width INTEGER,
                input_height INTEGER,
                output_width INTEGER,
                output_height INTEGER,
                errors INTEGER
            );
            CREATE INDEX IF NOT EXISTS idx_timestamp ON resizer_benchmarks(timestamp);
        """

    def store_benchmark_data(self, metrics: ResizerMetrics):
        """Store metrics to database (compatibility method)"""
        # This is kept for compatibility but won't be used
        # The actual work is done in store_benchmark_data_internal
        pass

    def store_benchmark_data_internal(self, db_conn, metrics: ResizerMetrics):
        """Store metrics to database using provided connection"""
        timestamp_ns = int(time.time() * 1e9)

        db_conn.execute("""
            INSERT OR REPLACE INTO resizer_benchmarks
            (timestamp, frames_processed, frames_dropped, current_fps, processing_time_ms,
            memory_usage_kb, input_width, input_height, output_width, output_height, errors)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            timestamp_ns,
            metrics.frames_processed,
            metrics.frames_dropped,
            metrics.current_fps,
            metrics.processing_time_ms,
            self.get_memory_usage(),
            metrics.input_width,
            metrics.input_height,
            metrics.output_width,
            metrics.output_height,
            metrics.errors
        ))

    def on_start(self):
        """Initialize ZMQ sockets and configuration"""
        # Setup ZMQ context
        self.zmq_context = zmq.Context(1)

        # Setup subscriber socket
        self.sub_socket = self.zmq_context.socket(zmq.SUB)
        self.sub_socket.setsockopt(zmq.RCVHWM, 100)
        self.sub_socket.setsockopt(zmq.SUBSCRIBE, b'')
        self.sub_socket.setsockopt(zmq.RCVTIMEO, 100)  # 100ms timeout

        sub_port = self.config['subscribe_port']
        self.sub_socket.connect(f"tcp://localhost:{sub_port}")
        self.logger.info(f"Connected to publisher on port {sub_port}")

        # Setup publisher socket
        self.pub_socket = self.zmq_context.socket(zmq.PUB)
        self.pub_socket.setsockopt(zmq.SNDHWM, 100)
        self.pub_socket.setsockopt(zmq.LINGER, 1000)

        pub_port = self.config['publish_port']
        self.pub_socket.bind(f"tcp://*:{pub_port}")
        self.logger.info(f"Publishing on port {pub_port}")

        # Configure interpolation method
        interp_map = {
            'nearest': cv2.INTER_NEAREST,
            'linear': cv2.INTER_LINEAR,
            'cubic': cv2.INTER_CUBIC,
            'area': cv2.INTER_AREA,
            'lanczos': cv2.INTER_LANCZOS4
        }
        self.interpolation = interp_map.get(
            self.config.get('interpolation', 'linear'),
            cv2.INTER_LINEAR
        )

        # Configure adaptive dropping
        self.adaptive_dropping = self.config.get('adaptive_dropping', False)
        if 'target_fps' in self.config:
            self.target_processing_time_ms = 1000.0 / self.config['target_fps']

        # Pre-allocate output buffer
        output_width = self.config['output_width']
        output_height = self.config['output_height']
        self.resize_buffer = np.empty((output_height, output_width, 3), dtype=np.uint8)

        # Update initial metrics
        self.metrics_mgr.update(
            output_width=output_width,
            output_height=output_height,
            service_healthy=True
        )
        self.metrics_mgr.commit()

    def should_drop_frame(self) -> bool:
        """Check if frame should be dropped based on performance"""
        if not self.adaptive_dropping or len(self.processing_times) < 10:
            return False

        avg_time = sum(self.processing_times) / len(self.processing_times)
        return avg_time > self.target_processing_time_ms

    def process_frame(self) -> bool:
        """Receive, resize, and forward a frame"""
        try:
            # Check if we should drop frames for performance
            if self.should_drop_frame():
                # Try to drain queue without processing
                try:
                    messages = self.sub_socket.recv_multipart(zmq.NOBLOCK)
                    self.frames_dropped += 1
                    self.logger.debug("Dropped frame due to high load")
                    return True
                except zmq.Again:
                    return False

            # Receive frame with timeout
            try:
                messages = self.sub_socket.recv_multipart(zmq.NOBLOCK)
            except zmq.Again:
                return False  # No frame available

            if len(messages) < 2:
                self.frames_dropped += 1
                return True

            metadata_bytes, frame_bytes = messages[0], messages[1]

            # Track processing time
            proc_start = time.time()

            # Parse metadata
            metadata = json.loads(metadata_bytes)

            # Update tracking timestamp
            resize_time = time.time()
            if 'tracking' in metadata:
                metadata['tracking']['resize_ts'] = resize_time

                # Log tracking for sample frames
                if 'sequence' in metadata['tracking']:
                    seq = metadata['tracking']['sequence']
                    if seq % 100 == 0:
                        source_ts = metadata['tracking'].get('source_ts', 0)
                        latency_ms = (resize_time - source_ts) * 1000
                        self.logger.debug(f"Processing frame {seq} - "
                                        f"Latency so far: {latency_ms:.1f}ms")

            # Decode frame
            frame_array = np.frombuffer(frame_bytes, dtype=np.uint8)
            frame = cv2.imdecode(frame_array, cv2.IMREAD_COLOR)

            if frame is None:
                self.frames_dropped += 1
                return True

            # Update input dimensions in metrics
            input_height, input_width = frame.shape[:2]
            self.metrics_mgr.update(
                input_width=input_width,
                input_height=input_height
            )

            # Resize frame into pre-allocated buffer
            output_size = (self.config['output_width'], self.config['output_height'])
            cv2.resize(frame, output_size, dst=self.resize_buffer,
                      interpolation=self.interpolation)

            # Encode resized frame
            encode_params = [cv2.IMWRITE_JPEG_QUALITY, self.config['jpeg_quality']]
            _, encoded = cv2.imencode('.jpg', self.resize_buffer, encode_params)

            # Calculate processing time
            proc_end = time.time()
            self.last_processing_time_ms = (proc_end - proc_start) * 1000
            self.processing_times.append(self.last_processing_time_ms)

            # Update metadata
            metadata['resized_width'] = self.resize_buffer.shape[1]
            metadata['resized_height'] = self.resize_buffer.shape[0]
            metadata['resizer_timestamp'] = proc_end
            metadata['resize_time_ms'] = self.last_processing_time_ms

            # Send resized frame
            try:
                self.pub_socket.send_json(metadata, zmq.SNDMORE | zmq.NOBLOCK)
                self.pub_socket.send(encoded.tobytes(), zmq.NOBLOCK)

                self.frames_processed += 1
                self.update_metrics()

            except zmq.Again:
                self.frames_dropped += 1
                self.logger.warning("Output queue full, dropped frame")

            return True

        except Exception as e:
            self.logger.error(f"Error processing frame: {e}")
            self.errors += 1
            return True

    def update_service_metrics(self):
        """Update resizer-specific metrics"""
        self.metrics_mgr.update(
            frames_processed=self.frames_processed,
            frames_dropped=self.frames_dropped,
            processing_time_ms=self.last_processing_time_ms
        )

    def on_stop(self):
        """Clean up resources"""
        if self.sub_socket:
            self.sub_socket.close()

        if self.pub_socket:
            self.pub_socket.close()

        if self.zmq_context:
            self.zmq_context.term()

def main():
    """Entry point"""
    resizer = FrameResizer()
    resizer.run()

if __name__ == "__main__":
    main()
