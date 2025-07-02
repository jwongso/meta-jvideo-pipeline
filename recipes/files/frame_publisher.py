#!/usr/bin/env python3
"""
Memory optimization examples for Python frame publisher
"""

import gc
import sys
import psutil
import os
from typing import Optional
import numpy as np
import cv2
import zmq
import json
import time
import redis
from contextlib import contextmanager

class MemoryOptimizedFramePublisher:
    def __init__(self):
        # Pre-allocate buffers to avoid repeated allocations
        self.frame_buffer: Optional[np.ndarray] = None
        self.metadata_template = {
            "frame_id": 0,
            "timestamp": 0.0,
            "width": 0,
            "height": 0,
            "channels": 0,
            "source": "mp4_python"
        }

        # ZMQ setup with memory limits
        self.context = zmq.Context(io_threads=1)
        self.socket = self.context.socket(zmq.PUB)
        self.socket.setsockopt(zmq.SNDHWM, 100)  # High water mark
        self.socket.setsockopt(zmq.SNDTIMEO, 100)  # Send timeout
        self.socket.setsockopt(zmq.LINGER, 0)
        self.socket.bind("tcp://*:5555")

        # Redis connection
        self.redis_client = redis.Redis(
            host='127.0.0.1',
            port=6379,
            decode_responses=True,
            socket_connect_timeout=2,
            socket_timeout=2
        )

        # Memory monitoring
        self.process = psutil.Process()
        self.frames_published = 0

    def setup_video_capture(self, video_path: str) -> bool:
        """Setup video capture with memory-conscious settings"""
        self.cap = cv2.VideoCapture(video_path)

        if not self.cap.isOpened():
            return False

        # Minimize buffering
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        # Get video properties and pre-allocate frame buffer
        width = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

        # Pre-allocate frame buffer (reuse same memory)
        self.frame_buffer = np.empty((height, width, 3), dtype=np.uint8)

        print(f"[Python] Pre-allocated frame buffer: {width}x{height}")
        return True

    @contextmanager
    def memory_monitor(self):
        """Context manager to monitor memory usage"""
        before = self.process.memory_info().rss / 1024 / 1024  # MB
        yield
        after = self.process.memory_info().rss / 1024 / 1024   # MB
        if after - before > 10:  # Alert if memory increased by >10MB
            print(f"[Memory] Increased by {after - before:.1f}MB")

    def update_memory_metrics(self):
        """Update memory metrics in Redis"""
        memory_info = self.process.memory_info()
        self.redis_client.hset("metrics:frame-publisher-python", mapping={
            "memory_rss_mb": round(memory_info.rss / 1024 / 1024, 2),
            "memory_vms_mb": round(memory_info.vms / 1024 / 1024, 2),
            "frames_published": self.frames_published
        })

    def publish_frames(self):
        """Main publishing loop with memory optimizations"""
        frame_id = 0

        while True:
            with self.memory_monitor():
                # Read frame into pre-allocated buffer
                ret = self.cap.read(self.frame_buffer)
                if not ret or self.frame_buffer is None:
                    break

                # Update metadata template (reuse object)
                self.metadata_template.update({
                    "frame_id": frame_id,
                    "timestamp": time.time(),
                    "width": self.frame_buffer.shape[1],
                    "height": self.frame_buffer.shape[0],
                    "channels": self.frame_buffer.shape[2]
                })

                # Serialize metadata once
                metadata_bytes = json.dumps(self.metadata_template).encode('utf-8')

                try:
                    # Non-blocking send to prevent memory buildup
                    self.socket.send(metadata_bytes, zmq.SNDMORE | zmq.NOBLOCK)

                    # Send frame data directly from numpy array
                    frame_bytes = self.frame_buffer.tobytes()
                    self.socket.send(frame_bytes, zmq.NOBLOCK)

                    self.frames_published += 1

                except zmq.Again:
                    # Skip frame if can't send (prevents memory buildup)
                    print(f"[Python] Dropped frame {frame_id} - slow subscribers")
                    continue

                frame_id += 1

                # Periodic maintenance
                if frame_id % 300 == 0:
                    self.update_memory_metrics()
                    # Force garbage collection periodically
                    gc.collect()
                    print(f"[Python] Published {self.frames_published} frames")

                # Rate limiting
                time.sleep(0.033)  # ~30 FPS

    def cleanup(self):
        """Cleanup resources"""
        if hasattr(self, 'cap'):
            self.cap.release()
        self.socket.close()
        self.context.term()


# Memory optimization functions
def optimize_python_memory():
    """Apply Python-specific memory optimizations"""
    # Tune garbage collection
    gc.set_threshold(700, 10, 10)  # More aggressive GC

    # Disable debug mode if enabled
    if hasattr(sys, 'gettrace') and sys.gettrace() is not None:
        sys.settrace(None)

    # Set recursion limit lower to save stack space
    sys.setrecursionlimit(500)  # Default is usually 1000


def main():
    """Main entry point with memory monitoring"""
    optimize_python_memory()

    publisher = MemoryOptimizedFramePublisher()

    try:
        video_path = sys.argv[1] if len(sys.argv) > 1 else "/opt/jvideo/input.mp4"

        if not publisher.setup_video_capture(video_path):
            print("[Python] Failed to open video")
            return 1

        print(f"[Python] Starting frame publishing...")
        publisher.publish_frames()

    except KeyboardInterrupt:
        print("[Python] Shutting down...")
    except Exception as e:
        print(f"[Python] Error: {e}")
        return 1
    finally:
        publisher.cleanup()

    return 0


if __name__ == "__main__":
    exit(main())
