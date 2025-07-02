#!/usr/bin/env python3
"""Frame Saver Service with Enhanced Memory Management"""

import sys
import time
import os
import json
import uuid
import datetime
import gc
import resource
from pathlib import Path
sys.path.append('/opt/jvideo/services')

import cv2
import numpy as np
import zmq
from service_base import ServiceBase

try:
    import psutil
    PSUTIL_AVAILABLE = True
except ImportError:
    PSUTIL_AVAILABLE = False

class FrameSaver(ServiceBase):
    def __init__(self):
        super().__init__('frame-saver')

        # Set process memory limit (soft limit of 200MB, hard limit of 250MB)
        try:
            resource.setrlimit(resource.RLIMIT_AS, (200 * 1024 * 1024, 250 * 1024 * 1024))
            self.logger.info("Set memory limits: 200MB soft, 250MB hard")
        except Exception as e:
            self.logger.warning(f"Could not set memory limits: {e}")

        # Load configuration with memory-safe defaults
        self.config = self._load_config()

        # Setup ZMQ with strict memory controls
        self.setup_zmq_with_limits()

        # Initialize output directory
        self.ensure_output_directory()

        # Statistics and state tracking
        self.frames_received = 0
        self.frames_saved = 0
        self.frames_skipped = 0
        self.bytes_written = 0
        self.errors = 0
        self.start_time = time.time()
        self.last_mem_check = time.time()

        # Initial memory cleanup
        gc.collect()
        self.logger.info(f"Service initialized. Memory: {self.get_memory_usage_mb():.1f} MB")

    def _load_config(self):
        """Load configuration with memory-conservative defaults"""
        default_config = {
            'subscribe_port': 5556,
            'subscribe_host': 'localhost',
            'output_dir': '/var/lib/jvideo/frames',
            'format': 'jpg',
            'quality': 85,  # Reduced from 90 for memory efficiency
            'compression': 6,
            'naming_pattern': 'frame_{timestamp}_{frame_id}.{ext}',
            'timestamp_format': '%Y%m%d_%H%M%S',
            'max_files': 1000,
            'max_size_mb': 500,
            'purge_oldest': True,
            'save_every_n': 5,  # Higher default skip rate
            'min_interval_ms': 0,
            'max_queue_size': 5,  # Reduced from 10
            'max_fps': 10,
            'save_metadata': False,  # Disabled by default
            'add_timestamp_overlay': True,
            'memory_check_interval': 5,  # Seconds between memory checks
            'gc_threshold_mb': 100  # MB at which to force GC
        }

        # Config file loading (unchanged from your version)
        config_path = os.environ.get('FRAME_SAVER_CONFIG', '/etc/jvideo/frame-saver.conf')
        if os.path.exists(config_path):
            try:
                with open(config_path, 'r') as f:
                    user_config = json.load(f)
                    default_config.update(user_config)
            except Exception as e:
                self.logger.warning(f"Config load error: {e}")

        # Environment overrides (unchanged)
        for var in ['JVIDEO_OUTPUT_DIR', 'JVIDEO_FORMAT', 'JVIDEO_SUBSCRIBE_PORT', 'JVIDEO_SAVE_EVERY_N']:
            if var in os.environ:
                default_config[var[7:].lower()] = os.environ[var]

        return default_config

    def setup_zmq_with_limits(self):
        """Configure ZMQ with strict memory controls"""
        self.sub_socket = self.zmq_context.socket(zmq.SUB)

        # Critical memory-related settings
        self.sub_socket.setsockopt(zmq.RCVHWM, self.config['max_queue_size'])
        self.sub_socket.setsockopt(zmq.RCVBUF, 65536)
        self.sub_socket.setsockopt(zmq.LINGER, 0)
        self.sub_socket.setsockopt(zmq.MAXMSGSIZE, 10 * 1024 * 1024)  # 10MB max frame size

        sub_addr = f"tcp://{self.config['subscribe_host']}:{self.config['subscribe_port']}"
        self.sub_socket.connect(sub_addr)
        self.sub_socket.setsockopt_string(zmq.SUBSCRIBE, "")
        self.logger.info(f"ZMQ connected to {sub_addr} with HWM={self.config['max_queue_size']}")

    def ensure_output_directory(self):
        """Create output directory with existence check"""
        output_dir = Path(self.config['output_dir'])
        try:
            output_dir.mkdir(parents=True, exist_ok=True)
            self.logger.info(f"Output directory ready: {output_dir}")
        except Exception as e:
            self.logger.error(f"Failed to create output directory: {e}")
            raise

    def get_memory_usage_mb(self):
        """Robust memory measurement with fallbacks"""
        try:
            if PSUTIL_AVAILABLE:
                return psutil.Process().memory_info().rss / (1024 * 1024)

            # Linux fallback
            if os.path.exists('/proc/self/status'):
                with open('/proc/self/status') as f:
                    for line in f:
                        if line.startswith('VmRSS:'):
                            return int(line.split()[1]) / 1024
        except:
            pass
        return 0

    def generate_filename(self, metadata):
        """Generate filename with minimal memory overhead"""
        now = datetime.datetime.now()
        try:
            timestamp = now.strftime(self.config['timestamp_format'])
            if '%f' in self.config['timestamp_format']:
                timestamp = timestamp.replace('%f', f"{now.microsecond:06d}")

            frame_id = metadata.get('frame_id', str(uuid.uuid4())[:8])

            return str(Path(self.config['output_dir']) / self.config['naming_pattern'].format(
                timestamp=timestamp,
                frame_id=frame_id,
                ext=self.config['format']
            ))
        except Exception as e:
            self.logger.error(f"Filename generation failed: {e}")
            return str(Path(self.config['output_dir']) / f"frame_{int(time.time())}.{self.config['format']}")

    def save_frame(self, frame, metadata):
        """Optimized frame saving with memory awareness"""
        filename = self.generate_filename(metadata)

        try:
            # In-place overlay to avoid copies
            if self.config['add_timestamp_overlay']:
                ts_text = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
                cv2.putText(frame, ts_text, (10, frame.shape[0] - 10),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)

            # Format-specific saving
            if self.config['format'] == 'jpg':
                success = cv2.imwrite(filename, frame, [
                    cv2.IMWRITE_JPEG_QUALITY, self.config['quality'],
                    cv2.IMWRITE_JPEG_OPTIMIZE, 1
                ])
            elif self.config['format'] == 'png':
                success = cv2.imwrite(filename, frame, [
                    cv2.IMWRITE_PNG_COMPRESSION, self.config['compression']
                ])
            elif self.config['format'] == 'raw':
                with open(filename, 'wb') as f:
                    f.write(frame.tobytes())
                success = True
            else:
                success = cv2.imwrite(filename, frame)

            if not success:
                raise IOError("Failed to write image file")

            # Update stats
            file_size = os.path.getsize(filename)
            self.bytes_written += file_size
            self.frames_saved += 1

            # Optional metadata
            if self.config.get('save_metadata', False):
                meta_filename = f"{os.path.splitext(filename)[0]}.json"
                with open(meta_filename, 'w') as f:
                    json.dump({
                        **metadata,
                        'saved_at': datetime.datetime.now().isoformat(),
                        'file_size': file_size
                    }, f)

            return True

        except Exception as e:
            self.logger.error(f"Frame save failed: {e}")
            self.errors += 1
            return False

    def manage_storage(self):
        """Storage management with error handling"""
        if not self.config['purge_oldest']:
            return

        try:
            output_dir = Path(self.config['output_dir'])
            pattern = f"*.{self.config['format']}"
            files = sorted(output_dir.glob(pattern), key=lambda x: x.stat().st_ctime)

            # File count limit
            if self.config['max_files'] > 0 and len(files) > self.config['max_files']:
                for file in files[:len(files) - self.config['max_files']]:
                    try:
                        file.unlink()
                        meta_file = file.with_suffix('.json')
                        if meta_file.exists():
                            meta_file.unlink()
                    except Exception as e:
                        self.logger.warning(f"Failed to purge {file}: {e}")

            # Size limit
            if self.config['max_size_mb'] > 0:
                total_size = sum(f.stat().st_size for f in files) / (1024 * 1024)
                files = sorted(files, key=lambda x: x.stat().st_ctime)

                while total_size > self.config['max_size_mb'] and files:
                    file = files.pop(0)
                    try:
                        file_size = file.stat().st_size / (1024 * 1024)
                        file.unlink()
                        meta_file = file.with_suffix('.json')
                        if meta_file.exists():
                            meta_file.unlink()
                        total_size -= file_size
                    except Exception as e:
                        self.logger.warning(f"Size purge failed for {file}: {e}")
        except Exception as e:
            self.logger.error(f"Storage management error: {e}")

    def process_frame(self):
        """Memory-optimized frame processing"""
        messages = None
        frame = None
        frame_array = None

        try:
            # Memory check and GC
            current_mem = self.get_memory_usage_mb()
            if current_mem > self.config['gc_threshold_mb']:
                self.logger.warning(f"Memory threshold exceeded ({current_mem:.1f}MB), forcing GC")
                gc.collect()
                time.sleep(0.01)  # Brief pause

            # Non-blocking receive
            try:
                messages = self.sub_socket.recv_multipart(zmq.NOBLOCK, copy=False)
            except zmq.Again:
                return False

            if len(messages) < 2:
                self.logger.warning("Incomplete message received")
                return False

            self.frames_received += 1

            # Early skip check to avoid processing
            if self.frames_received % self.config['save_every_n'] != 0:
                self.frames_skipped += 1
                return True

            # Process frame
            try:
                metadata = json.loads(messages[0].bytes.decode('utf-8'))
                frame_array = np.frombuffer(messages[1].bytes, dtype=np.uint8)
                frame = frame_array.reshape(
                    metadata['height'],
                    metadata['width'],
                    metadata['channels']
                )

                if not self.save_frame(frame, metadata):
                    self.errors += 1

                # Periodic storage management
                if self.frames_saved % 50 == 0:
                    self.manage_storage()

                return True

            except Exception as e:
                self.logger.error(f"Frame processing error: {e}")
                self.errors += 1
                return False

        finally:
            # Ensure cleanup
            if frame is not None:
                del frame
            if frame_array is not None:
                del frame_array
            if messages:
                for msg in messages:
                    if hasattr(msg, 'close'):
                        msg.close()
                del messages
            gc.collect()

    def run(self):
        """Main loop with comprehensive memory management"""
        self.logger.info("Starting frame saver service")
        self.logger.info(f"Config: {json.dumps({k:v for k,v in self.config.items() if not isinstance(v, (str, int))}, indent=2)}")

        # Service registration
        if self.redis_available:
            try:
                self.redis.hset(f"service:{self.service_name}", mapping={
                    'status': 'running',
                    'start_time': datetime.datetime.now().isoformat(),
                    'config': json.dumps(self.config)
                })
            except Exception as e:
                self.logger.warning(f"Redis update failed: {e}")

        # Timing controls
        last_log = time.time()
        last_frame_time = time.time()
        frame_delay = 1.0 / self.config['max_fps'] if self.config['max_fps'] > 0 else 0

        while self.running:
            try:
                # Process frames
                processed = self.process_frame()

                # FPS control
                if processed and frame_delay > 0:
                    elapsed = time.time() - last_frame_time
                    if elapsed < frame_delay:
                        time.sleep(frame_delay - elapsed)
                    last_frame_time = time.time()

                # Status logging
                if time.time() - last_log >= self.config['memory_check_interval']:
                    elapsed_total = time.time() - self.start_time
                    mem_usage = self.get_memory_usage_mb()

                    self.logger.info(
                        f"Stats: Saved {self.frames_saved}/{self.frames_received} "
                        f"(Skipped: {self.frames_skipped}) | "
                        f"FPS: {self.frames_saved/max(1, elapsed_total):.1f} | "
                        f"Memory: {mem_usage:.1f}MB | "
                        f"Errors: {self.errors}"
                    )

                    last_log = time.time()

            except KeyboardInterrupt:
                self.logger.info("Shutdown signal received")
                break
            except Exception as e:
                self.logger.error(f"Unexpected error: {e}")
                time.sleep(1)  # Prevent tight error loops

        # Clean shutdown
        self.logger.info("Shutting down...")
        if hasattr(self, 'sub_socket'):
            self.sub_socket.close()
        if hasattr(self, 'zmq_context'):
            self.zmq_context.term()

        self.logger.info(
            f"Final stats: Saved {self.frames_saved} frames "
            f"({self.bytes_written/(1024*1024):.1f}MB) in "
            f"{time.time() - self.start_time:.1f} seconds"
        )
        return True


def main():
    """Entry point with enhanced error handling"""
    saver = None
    try:
        saver = FrameSaver()
        saver.run()
    except KeyboardInterrupt:
        if saver:
            saver.logger.info("Keyboard interrupt received")
    except Exception as e:
        if saver:
            saver.logger.error(f"Fatal error: {e}")
        else:
            print(f"Initialization failed: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        if saver:
            saver.logger.info("Cleanup complete")
        gc.collect()


if __name__ == "__main__":
    main()
