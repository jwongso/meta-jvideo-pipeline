#!/usr/bin/env python3
import json
import zmq
import cv2
import numpy as np
from pathlib import Path
from datetime import datetime

class FrameSaver:
    def __init__(self):
        self.config = self._load_config()
        self.context = zmq.Context()
        self.sub_socket = self.context.socket(zmq.SUB)
        self.sub_socket.connect(f"tcp://{self.config['subscribe_host']}:{self.config['subscribe_port']}")
        self.sub_socket.setsockopt_string(zmq.SUBSCRIBE, "")
        Path(self.config['output_dir']).mkdir(parents=True, exist_ok=True)

    def _load_config(self):
        config = {
            'subscribe_port': 5556,
            'subscribe_host': 'localhost',
            'output_dir': '/var/lib/jvideo/frames',
            'format': 'jpg'
        }
        try:
            with open('/etc/jvideo/frame-saver.conf', 'r') as f:
                config.update(json.load(f))
        except:
            pass
        return config

    def run(self):
        while True:
            try:
                metadata, frame_data = self.sub_socket.recv_multipart()
                metadata = json.loads(metadata.decode())
                frame = np.frombuffer(frame_data, dtype=np.uint8).reshape(
                    metadata['height'], metadata['width'], metadata['channels']
                )
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                cv2.imwrite(
                    f"{self.config['output_dir']}/frame_{timestamp}.{self.config['format']}",
                    frame
                )
            except zmq.Again:
                continue
            except Exception as e:
                print(f"Error: {e}")

if __name__ == "__main__":
    FrameSaver().run()
