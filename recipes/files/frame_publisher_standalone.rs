// Rust Frame Publisher - No external crates, using FFI directly
use std::ffi::{CString, c_void};
use std::os::raw::{c_char, c_int};
use std::ptr;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use std::f64::consts::PI;

// ZeroMQ FFI bindings
#[link(name = "zmq")]
extern "C" {
    fn zmq_ctx_new() -> *mut c_void;
    fn zmq_ctx_destroy(context: *mut c_void) -> c_int;
    fn zmq_socket(context: *mut c_void, socket_type: c_int) -> *mut c_void;
    fn zmq_close(socket: *mut c_void) -> c_int;
    fn zmq_bind(socket: *mut c_void, endpoint: *const c_char) -> c_int;
    fn zmq_send(socket: *mut c_void, buf: *const c_void, len: usize, flags: c_int) -> c_int;
}

const ZMQ_PUB: c_int = 1;
const ZMQ_SNDMORE: c_int = 2;

struct FramePublisher {
    context: *mut c_void,
    socket: *mut c_void,
    frame_count: u64,
    start_time: Instant,
    width: u32,
    height: u32,
    fps: u32,
}

impl FramePublisher {
    fn new() -> Result<Self, String> {
        unsafe {
            let context = zmq_ctx_new();
            if context.is_null() {
                return Err("Failed to create ZMQ context".to_string());
            }

            let socket = zmq_socket(context, ZMQ_PUB);
            if socket.is_null() {
                zmq_ctx_destroy(context);
                return Err("Failed to create ZMQ socket".to_string());
            }

            let endpoint = CString::new("tcp://*:5555").unwrap();
            if zmq_bind(socket, endpoint.as_ptr()) != 0 {
                zmq_close(socket);
                zmq_ctx_destroy(context);
                return Err("Failed to bind socket".to_string());
            }

            println!("[RUST] Publisher bound to tcp://*:5555");

            Ok(FramePublisher {
                context,
                socket,
                frame_count: 0,
                start_time: Instant::now(),
                width: 640,
                height: 480,
                fps: 30,
            })
        }
    }

    fn generate_synthetic_frame(&self) -> Vec<u8> {
        let mut frame = vec![0u8; (self.width * self.height * 3) as usize];
        let t = self.start_time.elapsed().as_secs_f64();

        // Create a simple pattern
        for y in 0..self.height {
            for x in 0..self.width {
                let idx = ((y * self.width + x) * 3) as usize;

                // Moving gradient
                let val = (128.0 + 127.0 * ((x as f64 / 100.0 + t).sin())) as u8;
                frame[idx] = val;
                frame[idx + 1] = val / 2;
                frame[idx + 2] = val / 3;
            }
        }

        // Add moving circles
        for i in 0..3 {
            let phase = 2.0 * PI * i as f64 / 3.0;
            let cx = (self.width as f64 * (0.5 + 0.3 * (t + phase).sin())) as i32;
            let cy = (self.height as f64 * (0.5 + 0.3 * (t * 0.7 + phase).cos())) as i32;

            // Draw circle
            for dy in -30..=30 {
                for dx in -30..=30 {
                    if dx * dx + dy * dy <= 900 {  // radius = 30
                        let px = cx + dx;
                        let py = cy + dy;
                        if px >= 0 && px < self.width as i32 && py >= 0 && py < self.height as i32 {
                            let idx = ((py * self.width as i32 + px) * 3) as usize;
                            if idx + 2 < frame.len() {
                                frame[idx] = (127.0 + 127.0 * (t + phase).sin()) as u8;
                                frame[idx + 1] = (127.0 + 127.0 * (t + phase + 2.0 * PI / 3.0).sin()) as u8;
                                frame[idx + 2] = (127.0 + 127.0 * (t + phase + 4.0 * PI / 3.0).sin()) as u8;
                            }
                        }
                    }
                }
            }
        }

        frame
    }

    fn send_frame(&mut self, frame: &[u8]) -> Result<(), String> {
        unsafe {
            // Create metadata JSON manually (no serde)
            let timestamp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs_f64();

            let metadata = format!(
                r#"{{"frame_id":{},"timestamp":{},"width":{},"height":{},"channels":3,"source_type":"synthetic","implementation":"rust"}}"#,
                self.frame_count, timestamp, self.width, self.height
            );

            // Send metadata
            let meta_bytes = metadata.as_bytes();
            if zmq_send(self.socket, meta_bytes.as_ptr() as *const c_void, meta_bytes.len(), ZMQ_SNDMORE) < 0 {
                return Err("Failed to send metadata".to_string());
            }

            // Send frame data
            if zmq_send(self.socket, frame.as_ptr() as *const c_void, frame.len(), 0) < 0 {
                return Err("Failed to send frame".to_string());
            }

            self.frame_count += 1;
            Ok(())
        }
    }

    fn run(&mut self) {
        println!("[RUST] Starting frame publishing (synthetic source)");
        println!("[RUST] Resolution: {}x{} @ {}fps", self.width, self.height, self.fps);

        let frame_duration = Duration::from_millis(1000 / self.fps as u64);
        let mut last_log = Instant::now();

        loop {
            let frame_start = Instant::now();

            // Generate and send frame
            let frame = self.generate_synthetic_frame();

            if let Err(e) = self.send_frame(&frame) {
                eprintln!("[RUST] Error sending frame: {}", e);
            }

            // Log progress
            if last_log.elapsed() >= Duration::from_secs(5) {
                let elapsed = self.start_time.elapsed().as_secs_f64();
                let actual_fps = self.frame_count as f64 / elapsed;
                println!("[RUST] Processed {} frames, FPS: {:.1}", self.frame_count, actual_fps);
                last_log = Instant::now();
            }

            // Maintain frame rate
            let elapsed = frame_start.elapsed();
            if elapsed < frame_duration {
                thread::sleep(frame_duration - elapsed);
            }
        }
    }
}

impl Drop for FramePublisher {
    fn drop(&mut self) {
        unsafe {
            if !self.socket.is_null() {
                zmq_close(self.socket);
            }
            if !self.context.is_null() {
                zmq_ctx_destroy(self.context);
            }
        }
    }
}

fn main() {
    println!("[RUST] Juni's Frame Publisher (Rust Implementation - Standalone)");

    match FramePublisher::new() {
        Ok(mut publisher) => {
            // Give subscribers time to connect
            thread::sleep(Duration::from_secs(1));
            publisher.run();
        }
        Err(e) => {
            eprintln!("[RUST] Failed to initialize: {}", e);
            std::process::exit(1);
        }
    }
}
