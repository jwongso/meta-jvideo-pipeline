#!/usr/bin/env python3
"""Frame Resizer Service - Subscribes to frames, resizes them, and republishes"""

import sys
import time
import os
import json
import numpy as np
sys.path.append('/opt/jvideo/services')

import cv2
import zmq
from service_base import ServiceBase

class FrameResizer(ServiceBase):
    def __init__(self):
        super().__init__('frame-resizer')

        # Load configuration
        self.config = self._load_config()

        # Setup ZMQ sockets
        self.setup_zmq_connections()

        # Processing stats
        self.frames_received = 0
        self.frames_resized = 0
        self.frames_published = 0
        self.start_time = time.time()

        self.logger.info("Frame Resizer initialized")

    def _load_config(self):
        """Load configuration from file or environment"""
        default_config = {
            # ZMQ settings
            'subscribe_port': 5555,      # Port to subscribe for frames
            'subscribe_host': 'localhost',
            'publish_port': 5556,        # Port to publish resized frames

            # Resize settings
            'output_width': 320,
            'output_height': 240,
            'maintain_aspect_ratio': True,
            'interpolation': 'linear',    # 'linear', 'cubic', 'nearest', 'area', 'lanczos'
            'quality': 95,               # JPEG quality for encoding

            # Processing options
            'add_resize_info': True,     # Add resize info overlay
            'add_timestamp': True,
            'max_queue_size': 10,        # Max frames to queue
            'processing_threads': 1,      # Number of processing threads

            # Performance
            'skip_frames': 0,            # Skip N frames (0 = process all)
            'max_fps': 0,                # Limit processing FPS (0 = unlimited)
        }

        # Try to load from config file
        config_path = os.environ.get('FRAME_RESIZER_CONFIG', '/etc/jvideo/frame-resizer.conf')

        if os.path.exists(config_path):
            try:
                with open(config_path, 'r') as f:
                    user_config = json.load(f)
                    default_config.update(user_config)
                    self.logger.info(f"Loaded config from {config_path}")
            except Exception as e:
                self.logger.warning(f"Failed to load config: {e}, using defaults")

        # Override with environment variables
        if 'JVIDEO_RESIZE_WIDTH' in os.environ:
            default_config['output_width'] = int(os.environ['JVIDEO_RESIZE_WIDTH'])
        if 'JVIDEO_RESIZE_HEIGHT' in os.environ:
            default_config['output_height'] = int(os.environ['JVIDEO_RESIZE_HEIGHT'])
        if 'JVIDEO_SUBSCRIBE_PORT' in os.environ:
            default_config['subscribe_port'] = int(os.environ['JVIDEO_SUBSCRIBE_PORT'])
        if 'JVIDEO_PUBLISH_PORT' in os.environ:
            default_config['publish_port'] = int(os.environ['JVIDEO_PUBLISH_PORT'])

        return default_config

    def setup_zmq_connections(self):
        """Setup ZMQ subscriber and publisher"""
        # Subscriber socket
        self.sub_socket = self.zmq_context.socket(zmq.SUB)
        sub_addr = f"tcp://{self.config['subscribe_host']}:{self.config['subscribe_port']}"
        self.sub_socket.connect(sub_addr)
        self.sub_socket.setsockopt_string(zmq.SUBSCRIBE, "")  # Subscribe to all messages
        self.logger.info(f"Subscriber connected to {sub_addr}")

        # Publisher socket for resized frames
        self.pub_socket = self.zmq_context.socket(zmq.PUB)
        pub_addr = f"tcp://*:{self.config['publish_port']}"
        self.pub_socket.bind(pub_addr)
        self.logger.info(f"Publisher bound to {pub_addr}")

        # Give sockets time to establish
        time.sleep(1)

    def get_interpolation_method(self):
        """Get OpenCV interpolation method from config"""
        methods = {
            'nearest': cv2.INTER_NEAREST,
            'linear': cv2.INTER_LINEAR,
            'cubic': cv2.INTER_CUBIC,
            'area': cv2.INTER_AREA,
            'lanczos': cv2.INTER_LANCZOS4
        }
        return methods.get(self.config['interpolation'], cv2.INTER_LINEAR)

    def calculate_resize_dimensions(self, original_width, original_height):
        """Calculate resize dimensions based on config"""
        target_width = self.config['output_width']
        target_height = self.config['output_height']

        if self.config['maintain_aspect_ratio']:
            # Calculate aspect ratios
            aspect_ratio = original_width / original_height
            target_ratio = target_width / target_height

            if aspect_ratio > target_ratio:
                # Original is wider - fit to width
                new_width = target_width
                new_height = int(target_width / aspect_ratio)
            else:
                # Original is taller - fit to height
                new_height = target_height
                new_width = int(target_height * aspect_ratio)

            return new_width, new_height
        else:
            # Direct resize to target dimensions
            return target_width, target_height

    def resize_frame(self, frame, metadata):
        """Resize a single frame"""
        original_height, original_width = frame.shape[:2]

        # Calculate new dimensions
        new_width, new_height = self.calculate_resize_dimensions(
            original_width, original_height
        )

        # Resize the frame
        interpolation = self.get_interpolation_method()
        resized = cv2.resize(frame, (new_width, new_height), interpolation=interpolation)

        # If maintaining aspect ratio and dimensions don't match target, add padding
        if self.config['maintain_aspect_ratio']:
            target_width = self.config['output_width']
            target_height = self.config['output_height']

            if new_width != target_width or new_height != target_height:
                # Create canvas with target size
                canvas = np.zeros((target_height, target_width, 3), dtype=np.uint8)

                # Calculate position to center the resized image
                x_offset = (target_width - new_width) // 2
                y_offset = (target_height - new_height) // 2

                # Place resized image on canvas
                canvas[y_offset:y_offset+new_height, x_offset:x_offset+new_width] = resized
                resized = canvas

        # Add overlays if configured
        if self.config['add_resize_info']:
            info_text = f"Resized: {original_width}x{original_height} -> {resized.shape[1]}x{resized.shape[0]}"
            cv2.putText(resized, info_text,
                       (10, resized.shape[0] - 40), cv2.FONT_HERSHEY_SIMPLEX,
                       0.5, (255, 255, 255), 1)

        if self.config['add_timestamp']:
            timestamp = time.strftime("%H:%M:%S")
            cv2.putText(resized, timestamp,
                       (resized.shape[1] - 80, 20), cv2.FONT_HERSHEY_SIMPLEX,
                       0.5, (255, 255, 255), 1)

        # Add service identifier
        cv2.putText(resized, "[RESIZER-PY]",
                   (resized.shape[1] - 100, resized.shape[0] - 10),
                   cv2.FONT_HERSHEY_SIMPLEX,
                   0.4, (128, 128, 128), 1)

        return resized

    def process_frame(self):
        """Receive and process a single frame"""
        try:
            # Try to receive frame with timeout
            if self.sub_socket.poll(100, zmq.POLLIN):
                # Receive multipart message
                messages = self.sub_socket.recv_multipart()

                if len(messages) >= 2:
                    # Parse metadata
                    metadata = json.loads(messages[0])
                    frame_data = messages[1]

                    self.frames_received += 1

                    # Skip frames if configured
                    if self.config['skip_frames'] > 0:
                        if self.frames_received % (self.config['skip_frames'] + 1) != 0:
                            return True

                    # Decode frame
                    frame_array = np.frombuffer(frame_data, dtype=np.uint8)
                    frame = frame_array.reshape(
                        metadata['height'],
                        metadata['width'],
                        metadata['channels']
                    )

                    # Resize frame
                    resized = self.resize_frame(frame, metadata)
                    self.frames_resized += 1

                    # Prepare metadata for resized frame
                    resized_metadata = {
                        'frame_id': metadata.get('frame_id', self.frames_resized),
                        'timestamp': time.time(),
                        'original_width': metadata['width'],
                        'original_height': metadata['height'],
                        'width': resized.shape[1],
                        'height': resized.shape[0],
                        'channels': resized.shape[2] if len(resized.shape) > 2 else 1,
                        'resizer': 'python',
                        'interpolation': self.config['interpolation'],
                        'source_timestamp': metadata.get('timestamp', 0)
                    }

                    # Publish resized frame
                    self.pub_socket.send_json(resized_metadata, zmq.SNDMORE)
                    self.pub_socket.send(resized.tobytes())
                    self.frames_published += 1

                    # Update metrics
                    self.update_metrics('frames_received', self.frames_received)
                    self.update_metrics('frames_resized', self.frames_resized)
                    self.update_metrics('frames_published', self.frames_published)

                    return True
                else:
                    self.logger.warning(f"Invalid message format: {len(messages)} parts")
                    return False

            return False  # No message available

        except zmq.ZMQError as e:
            self.logger.error(f"ZMQ error: {e}")
            return False
        except Exception as e:
            self.logger.error(f"Error processing frame: {e}")
            self.update_metrics('errors', self.metrics.get('errors', 0) + 1)
            return False

    def run(self):
        """Main processing loop"""
        self.logger.info(f"Starting frame resizing")
        self.logger.info(f"Input: port {self.config['subscribe_port']}")
        self.logger.info(f"Output: {self.config['output_width']}x{self.config['output_height']} "
                        f"on port {self.config['publish_port']}")

        # Update Redis with service info
        if self.redis_available:
            service_info = {
                'service': 'frame-resizer',
                'status': 'running',
                'input_port': self.config['subscribe_port'],
                'output_port': self.config['publish_port'],
                'output_size': f"{self.config['output_width']}x{self.config['output_height']}",
                'implementation': 'python'
            }
            try:
                self.redis.hset(f"service:{self.service_name}", mapping=service_info)
            except:
                pass

        last_log_time = time.time()
        last_frame_time = time.time()

        # FPS limiting
        if self.config['max_fps'] > 0:
            frame_delay = 1.0 / self.config['max_fps']
        else:
            frame_delay = 0

        while self.running:
            # Process frame
            frame_processed = self.process_frame()

            # Log progress periodically
            if time.time() - last_log_time > 5:
                self.log_metrics()
                elapsed = time.time() - self.start_time
                if elapsed > 0 and self.frames_resized > 0:
                    fps = self.frames_resized / elapsed
                    self.logger.info(f"Processing rate: {fps:.1f} FPS, "
                                   f"Queue efficiency: {self.frames_resized}/{self.frames_received} "
                                   f"({100*self.frames_resized/max(1,self.frames_received):.1f}%)")
                last_log_time = time.time()

            # FPS limiting
            if frame_processed and frame_delay > 0:
                elapsed = time.time() - last_frame_time
                if elapsed < frame_delay:
                    time.sleep(frame_delay - elapsed)
                last_frame_time = time.time()
            elif not frame_processed:
                # No frame available, brief sleep to prevent busy waiting
                time.sleep(0.001)

        # Cleanup
        self.logger.info("Frame resizing stopped")
        self.logger.info(f"Total processed: {self.frames_resized}/{self.frames_received} frames")

        return True


def main():
    # Parse command line arguments
    if len(sys.argv) > 1:
        if sys.argv[1] == '--help':
            print("Frame Resizer Service")
            print("Environment variables:")
            print("  JVIDEO_RESIZE_WIDTH    - Output width (default: 320)")
            print("  JVIDEO_RESIZE_HEIGHT   - Output height (default: 240)")
            print("  JVIDEO_SUBSCRIBE_PORT  - Input port (default: 5555)")
            print("  JVIDEO_PUBLISH_PORT    - Output port (default: 5556)")
            sys.exit(0)

    # Create and run resizer
    resizer = FrameResizer()

    try:
        resizer.run()
    except KeyboardInterrupt:
        resizer.logger.info("Keyboard interrupt received")
    except Exception as e:
        resizer.logger.error(f"Fatal error: {e}")
        import traceback
        resizer.logger.error(traceback.format_exc())
        sys.exit(1)
    finally:
        resizer.logger.info("Shutting down frame resizer")
        if hasattr(resizer, 'sub_socket'):
            resizer.sub_socket.close()
        if hasattr(resizer, 'pub_socket'):
            resizer.pub_socket.close()
        if hasattr(resizer, 'zmq_context'):
            resizer.zmq_context.term()


if __name__ == "__main__":
    main()
