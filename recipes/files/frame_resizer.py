#!/usr/bin/env python3
import json
import signal
import sys
import time
import numpy as np
import cv2
import zmq

class FrameResizer:
    def __init__(self):
        self.running = True
        self.config = {
            'subscribe_port': 5555,
            'publish_port': 5556,
            'output_width': 160,
            'output_height': 120,
            'interpolation': 'linear'
        }
        self.load_config()
        self.setup_sockets()
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

    def load_config(self):
        try:
            with open('/etc/jvideo/frame-resizer.conf') as f:
                user_config = json.load(f)
                # Validate and merge config
                self.config.update({
                    k: v for k, v in user_config.items()
                    if k in self.config
                })
                # Ensure valid dimensions
                self.config['output_width'] = max(1, self.config['output_width'])
                self.config['output_height'] = max(1, self.config['output_height'])
        except Exception:
            pass  # Use defaults

        # Map interpolation method
        self.interpolation = {
            'nearest': cv2.INTER_NEAREST,
            'linear': cv2.INTER_LINEAR,
            'cubic': cv2.INTER_CUBIC,
            'area': cv2.INTER_AREA,
            'lanczos': cv2.INTER_LANCZOS4
        }.get(self.config['interpolation'], cv2.INTER_LINEAR)

    def setup_sockets(self):
        self.context = zmq.Context()
        self.sub_socket = self.context.socket(zmq.SUB)
        self.sub_socket.connect(f"tcp://localhost:{self.config['subscribe_port']}")
        self.sub_socket.setsockopt_string(zmq.SUBSCRIBE, '')

        self.pub_socket = self.context.socket(zmq.PUB)
        self.pub_socket.bind(f"tcp://*:{self.config['publish_port']}")

    def signal_handler(self, signum, frame):
        self.running = False

    def process_frame(self):
        try:
            if not self.sub_socket.poll(100):
                return False

            metadata, frame_data = self.sub_socket.recv_multipart()
            metadata = json.loads(metadata)

            # Validate and process frame
            frame = np.frombuffer(frame_data, dtype=np.uint8).reshape(
                metadata['height'], metadata['width'], metadata.get('channels', 3))

            resized = cv2.resize(
                frame,
                (self.config['output_width'], self.config['output_height']),
                interpolation=self.interpolation
            )

            # Update metadata
            metadata['width'] = resized.shape[1]
            metadata['height'] = resized.shape[0]

            self.pub_socket.send_json(metadata, zmq.SNDMORE)
            self.pub_socket.send(resized.tobytes())
            return True
        except Exception:
            return False  # Drop corrupt frames

    def run(self):
        while self.running:
            if not self.process_frame():
                time.sleep(0.001)

        self.cleanup()

    def cleanup(self):
        self.sub_socket.close()
        self.pub_socket.close()
        self.context.term()

if __name__ == "__main__":
    FrameResizer().run()
