#!/usr/bin/env python3
"""Frame Publisher Service - Configurable camera or synthetic video source"""

import sys
import time
import os
import json
import signal
sys.path.append('/opt/jvideo/services')

import cv2
import numpy as np
import zmq
from enum import Enum
from service_base import ServiceBase

class SourceType(Enum):
    CAMERA = "camera"
    SYNTHETIC = "synthetic"

class FramePublisher(ServiceBase):
    def __init__(self):
        super().__init__('frame-publisher')

        # Load configuration
        self.config = self._load_config()
        self.source_type = SourceType(self.config['source_type'])

        # Setup ZMQ publisher socket
        port = self.config.get('publish_port', 5555)
        self.socket = self.zmq_context.socket(zmq.PUB)
        self.socket.bind(f"tcp://*:{port}")
        self.logger.info(f"Publisher bound to tcp://*:{port}")

        # Initialize source
        self.cap = None
        self.frame_count = 0
        self.start_time = time.time()

        # Give subscribers time to connect
        time.sleep(1)

    def _load_config(self):
        """Load configuration from file or environment"""
        default_config = {
            'source_type': 'synthetic',  # 'camera' or 'synthetic'
            'publish_port': 5555,

            # Camera settings
            'camera_device': 0,
            'camera_backend': 'auto',  # 'auto', 'v4l2', 'gstreamer'
            'camera_buffer_size': 1,

            # Frame settings (applies to both sources)
            'width': 640,
            'height': 480,
            'fps': 30,

            # Synthetic settings
            'synthetic_pattern': 'moving_shapes',  # 'moving_shapes', 'color_bars', 'checkerboard'
            'synthetic_motion_speed': 1.0,

            # Processing options
            'add_timestamp': True,
            'add_frame_number': True,
            'add_source_label': True
        }

        # Try to load from config file
        config_path = os.environ.get('FRAME_PUBLISHER_CONFIG', '/etc/jvideo/frame-publisher.conf')

        if os.path.exists(config_path):
            try:
                with open(config_path, 'r') as f:
                    user_config = json.load(f)
                    default_config.update(user_config)
                    self.logger.info(f"Loaded config from {config_path}")
            except Exception as e:
                self.logger.warning(f"Failed to load config: {e}, using defaults")

        # Override with environment variables
        if 'JVIDEO_SOURCE_TYPE' in os.environ:
            default_config['source_type'] = os.environ['JVIDEO_SOURCE_TYPE']
        if 'JVIDEO_CAMERA_DEVICE' in os.environ:
            default_config['camera_device'] = int(os.environ['JVIDEO_CAMERA_DEVICE'])
        if 'JVIDEO_WIDTH' in os.environ:
            default_config['width'] = int(os.environ['JVIDEO_WIDTH'])
        if 'JVIDEO_HEIGHT' in os.environ:
            default_config['height'] = int(os.environ['JVIDEO_HEIGHT'])
        if 'JVIDEO_FPS' in os.environ:
            default_config['fps'] = int(os.environ['JVIDEO_FPS'])

        return default_config

    def _init_camera(self):
        """Initialize camera source"""
        device = self.config['camera_device']

        # Determine backend
        backend = cv2.CAP_ANY
        if self.config['camera_backend'] == 'v4l2':
            backend = cv2.CAP_V4L2
        elif self.config['camera_backend'] == 'gstreamer':
            backend = cv2.CAP_GSTREAMER

        self.logger.info(f"Initializing camera device {device}")

        # Open camera
        self.cap = cv2.VideoCapture(device, backend)

        if not self.cap.isOpened():
            self.logger.error(f"Failed to open camera device {device}")
            # Try alternative devices
            for alt_device in [0, 1, '/dev/video0', '/dev/video1']:
                if alt_device != device:
                    self.logger.info(f"Trying device: {alt_device}")
                    self.cap = cv2.VideoCapture(alt_device)
                    if self.cap.isOpened():
                        device = alt_device
                        break

            if not self.cap.isOpened():
                self.logger.error("No camera found, falling back to synthetic")
                self.source_type = SourceType.SYNTHETIC
                return self._init_synthetic()

        # Configure camera
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.config['width'])
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.config['height'])
        self.cap.set(cv2.CAP_PROP_FPS, self.config['fps'])
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, self.config['camera_buffer_size'])

        # Get actual settings
        actual_width = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        actual_height = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        actual_fps = self.cap.get(cv2.CAP_PROP_FPS)

        self.logger.info(
            f"Camera initialized: {actual_width}x{actual_height} @ {actual_fps}fps"
        )

        # Update config with actual values
        self.config['width'] = actual_width
        self.config['height'] = actual_height
        if actual_fps > 0:
            self.config['fps'] = actual_fps

        return True

    def _init_synthetic(self):
        """Initialize synthetic source"""
        self.logger.info(
            f"Synthetic source initialized: {self.config['width']}x{self.config['height']} "
            f"@ {self.config['fps']}fps, pattern: {self.config['synthetic_pattern']}"
        )
        return True

    def _get_camera_frame(self):
        """Get frame from camera"""
        if self.cap is None:
            return None

        ret, frame = self.cap.read()
        if not ret:
            self.logger.warning("Failed to read from camera")
            return None

        return frame

    def _generate_synthetic_frame(self):
        """Generate synthetic frame based on pattern"""
        width = self.config['width']
        height = self.config['height']
        pattern = self.config['synthetic_pattern']
        speed = self.config['synthetic_motion_speed']

        # Create base frame
        frame = np.zeros((height, width, 3), dtype=np.uint8)

        # Time for animation
        t = (time.time() - self.start_time) * speed

        if pattern == 'moving_shapes':
            # Background gradient
            for y in range(height):
                val = int(20 + 10 * np.sin(y / 50.0 + t * 0.5))
                frame[y, :] = (val, val//2, val//3)

            # Moving circles
            num_circles = 3
            for i in range(num_circles):
                phase = 2 * np.pi * i / num_circles
                cx = int(width * (0.5 + 0.3 * np.sin(t + phase)))
                cy = int(height * (0.5 + 0.3 * np.cos(t * 0.7 + phase)))

                color = (
                    int(127 + 127 * np.sin(t + phase)),
                    int(127 + 127 * np.sin(t + phase + 2*np.pi/3)),
                    int(127 + 127 * np.sin(t + phase + 4*np.pi/3))
                )
                cv2.circle(frame, (cx, cy), 30, color, -1)

            # Moving rectangle
            rx = int(width * (0.5 + 0.4 * np.cos(t * 0.8)))
            ry = height // 3
            cv2.rectangle(frame,
                         (rx - 40, ry - 20),
                         (rx + 40, ry + 20),
                         (255, 200, 0), -1)

        elif pattern == 'color_bars':
            # SMPTE-style color bars
            colors = [
                (192, 192, 192),  # Gray
                (192, 192, 0),    # Yellow
                (0, 192, 192),    # Cyan
                (0, 192, 0),      # Green
                (192, 0, 192),    # Magenta
                (192, 0, 0),      # Red
                (0, 0, 192),      # Blue
            ]

            bar_width = width // len(colors)
            for i, color in enumerate(colors):
                x_start = i * bar_width
                x_end = (i + 1) * bar_width if i < len(colors) - 1 else width
                frame[:, x_start:x_end] = color

            # Animated line
            line_y = int(height * (0.5 + 0.4 * np.sin(t)))
            cv2.line(frame, (0, line_y), (width, line_y), (255, 255, 255), 2)

        elif pattern == 'checkerboard':
            # Animated checkerboard
            square_size = 40
            offset = int(t * 10) % (square_size * 2)

            for y in range(0, height, square_size):
                for x in range(0, width, square_size):
                    if ((x + offset) // square_size + y // square_size) % 2 == 0:
                        cv2.rectangle(frame,
                                    (x, y),
                                    (min(x + square_size, width), min(y + square_size, height)),
                                    (255, 255, 255), -1)

        return frame

    def _add_overlays(self, frame):
        """Add text overlays to frame"""
        if frame is None:
            return frame

        height, width = frame.shape[:2]

        # Source type label
        if self.config.get('add_source_label', True):
            label = f"{self.source_type.value.upper()}"
            if self.source_type == SourceType.SYNTHETIC:
                label += f" ({self.config['synthetic_pattern']})"

            cv2.putText(frame, label,
                       (10, 30), cv2.FONT_HERSHEY_SIMPLEX,
                       0.7, (255, 255, 255), 2)

        # Frame number
        if self.config.get('add_frame_number', True):
            cv2.putText(frame, f"Frame: {self.frame_count}",
                       (10, 60), cv2.FONT_HERSHEY_SIMPLEX,
                       0.6, (255, 255, 255), 1)

        # Timestamp
        if self.config.get('add_timestamp', True):
            timestamp = time.strftime("%H:%M:%S")
            cv2.putText(frame, timestamp,
                       (width - 100, 30), cv2.FONT_HERSHEY_SIMPLEX,
                       0.6, (255, 255, 255), 1)

        # FPS indicator
        elapsed = time.time() - self.start_time
        if elapsed > 0:
            actual_fps = self.frame_count / elapsed
            cv2.putText(frame, f"FPS: {actual_fps:.1f}",
                       (width - 100, 60), cv2.FONT_HERSHEY_SIMPLEX,
                       0.6, (255, 255, 255), 1)

        return frame

    def run(self):
        """Main publishing loop"""
        # Initialize source
        if self.source_type == SourceType.CAMERA:
            if not self._init_camera():
                return False
        else:
            if not self._init_synthetic():
                return False

        # Publishing configuration
        target_fps = self.config['fps']
        frame_delay = 1.0 / target_fps if target_fps > 0 else 0.033

        self.logger.info(f"Starting frame publishing from {self.source_type.value}")
        self.logger.info(f"Target: {self.config['width']}x{self.config['height']} @ {target_fps}fps")

        # Update Redis with source info
        if self.redis_available:
            source_info = {
                'source_type': self.source_type.value,
                'width': self.config['width'],
                'height': self.config['height'],
                'fps': self.config['fps'],
                'implementation': os.environ.get('JVIDEO_SERVICE_IMPL', 'python')
            }
            try:
                self.redis.hset(f"source:{self.service_name}", mapping=source_info)
            except:
                pass

        # Main loop
        last_log_time = time.time()

        while self.running:
            loop_start = time.time()

            # Get frame based on source type
            if self.source_type == SourceType.CAMERA:
                frame = self._get_camera_frame()
            else:
                frame = self._generate_synthetic_frame()

            if frame is None:
                self.logger.error("Failed to get frame")
                time.sleep(0.1)
                continue

            # Add overlays
            frame = self._add_overlays(frame)

            # Prepare metadata
            metadata = {
                'frame_id': self.frame_count,
                'timestamp': time.time(),
                'width': frame.shape[1],
                'height': frame.shape[0],
                'channels': 3,
                'source_type': self.source_type.value,
                'implementation': os.environ.get('JVIDEO_SERVICE_IMPL', 'python')
            }

            try:
                # Send as multipart message
                self.socket.send_json(metadata, zmq.SNDMORE)
                self.socket.send(frame.tobytes())

                # Update metrics
                self.frame_count += 1
                self.update_metrics('frames_processed', self.frame_count)
                self.update_metrics('last_frame_id', self.frame_count)

            except Exception as e:
                self.logger.error(f"Error publishing frame {self.frame_count}: {e}")
                self.update_metrics('errors', self.metrics.get('errors', 0) + 1)

            # Log progress periodically
            if time.time() - last_log_time > 5:
                self.log_metrics()
                last_log_time = time.time()

            # Maintain frame rate
            elapsed = time.time() - loop_start
            sleep_time = frame_delay - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

        # Cleanup
        if self.cap is not None:
            self.cap.release()

        self.logger.info("Frame publishing stopped")
        return True


def main():
    # Parse command line arguments for quick testing
    if len(sys.argv) > 1:
        arg = sys.argv[1]

        # Quick source type override
        if arg in ['camera', 'synthetic']:
            os.environ['JVIDEO_SOURCE_TYPE'] = arg
            print(f"Using source type: {arg}")

        # Camera device number
        elif arg.isdigit():
            os.environ['JVIDEO_SOURCE_TYPE'] = 'camera'
            os.environ['JVIDEO_CAMERA_DEVICE'] = arg
            print(f"Using camera device: {arg}")

    # Create and run publisher
    publisher = FramePublisher()

    try:
        publisher.run()
    except KeyboardInterrupt:
        publisher.logger.info("Keyboard interrupt received")
    except Exception as e:
        publisher.logger.error(f"Fatal error: {e}")
        import traceback
        publisher.logger.error(traceback.format_exc())
        sys.exit(1)
    finally:
        publisher.logger.info("Shutting down frame publisher")
        if hasattr(publisher, 'cap') and publisher.cap is not None:
            publisher.cap.release()
        if hasattr(publisher, 'socket'):
            publisher.socket.close()
        if hasattr(publisher, 'zmq_context'):
            publisher.zmq_context.term()


if __name__ == "__main__":
    main()
