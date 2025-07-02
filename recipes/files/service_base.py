#!/usr/bin/env python3
"""Base class for Juni's Video Pipeline services"""

import zmq
import redis
import json
import time
import logging
import os
import signal
import sys

class ServiceBase:
    def __init__(self, service_name):
        self.service_name = service_name
        self.running = True
        self.setup_logging()
        self.setup_zmq()
        self.setup_redis()
        self.setup_signal_handlers()

        self.metrics = {
            'frames_processed': 0,
            'errors': 0,
            'start_time': time.time()
        }

    def setup_logging(self):
        """Setup logging to file and console"""
        os.makedirs('/var/log/jvideo', exist_ok=True)

        # Create logger
        self.logger = logging.getLogger(self.service_name)
        self.logger.setLevel(logging.DEBUG)

        # File handler
        fh = logging.FileHandler(f'/var/log/jvideo/{self.service_name}.log')
        fh.setLevel(logging.DEBUG)

        # Console handler
        ch = logging.StreamHandler()
        ch.setLevel(logging.INFO)

        # Formatter
        formatter = logging.Formatter(
            '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )
        fh.setFormatter(formatter)
        ch.setFormatter(formatter)

        self.logger.addHandler(fh)
        self.logger.addHandler(ch)

    def setup_zmq(self):
        """Setup ZeroMQ context"""
        self.zmq_context = zmq.Context()
        self.logger.info("ZeroMQ context created")

    def setup_redis(self):
        """Setup Redis connection with fallback"""
        try:
            self.redis = redis.Redis(
                host='localhost',
                port=6379,
                decode_responses=True,
                socket_connect_timeout=2
            )
            self.redis.ping()
            self.redis_available = True
            self.logger.info("Redis connected successfully")
        except Exception as e:
            self.redis_available = False
            self.logger.warning(f"Redis not available: {e}")
            self.logger.warning("Running without Redis metrics")

    def setup_signal_handlers(self):
        """Setup graceful shutdown"""
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

    def signal_handler(self, signum, frame):
        """Handle shutdown signals"""
        self.logger.info(f"Received signal {signum}, shutting down...")
        self.running = False

    def update_metrics(self, key, value):
        """Update metrics in Redis if available"""
        self.metrics[key] = value

        if self.redis_available:
            try:
                self.redis.hset(
                    f'metrics:{self.service_name}',
                    key,
                    str(value)
                )
            except Exception as e:
                self.logger.debug(f"Redis update failed: {e}")

    def log_metrics(self):
        """Periodically log metrics"""
        uptime = time.time() - self.metrics['start_time']
        fps = self.metrics['frames_processed'] / uptime if uptime > 0 else 0
        self.logger.info(
            f"Metrics - Frames: {self.metrics['frames_processed']}, "
            f"FPS: {fps:.2f}, Errors: {self.metrics['errors']}"
        )
