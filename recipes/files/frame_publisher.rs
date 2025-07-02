// frame_publisher.rs - Rust implementation of frame publisher
use zmq;
use opencv::{prelude::*, core, imgproc, videoio};
use serde::{Serialize, Deserialize};
use serde_json;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use std::thread;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::fs;
use std::f64::consts::PI;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
enum SourceType {
    Camera,
    Synthetic,
}

#[derive(Debug, Serialize, Deserialize)]
struct Config {
    source_type: SourceType,
    publish_port: u16,
    camera_device: i32,
    camera_backend: String,
    width: i32,
    height: i32,
    fps: i32,
    synthetic_pattern: String,
    synthetic_motion_speed: f64,
    add_timestamp: bool,
    add_frame_number: bool,
    add_source_label: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            source_type: SourceType::Synthetic,
            publish_port: 5555,
            camera_device: 0,
            camera_backend: "auto".to_string(),
            width: 640,
            height: 480,
            fps: 30,
            synthetic_pattern: "moving_shapes".to_string(),
            synthetic_motion_speed: 1.0,
            add_timestamp: true,
            add_frame_number: true,
            add_source_label: true,
        }
    }
}

struct FramePublisher {
    config: Config,
    context: zmq::Context,
    socket: zmq::Socket,
    cap: Option<videoio::VideoCapture>,
    start_time: Instant,
    frame_count: u64,
    running: Arc<AtomicBool>,
}

impl FramePublisher {
    fn new(running: Arc<AtomicBool>) -> Result<Self, Box<dyn std::error::Error>> {
        let config = Self::load_config()?;

        let context = zmq::Context::new();
        let socket = context.socket(zmq::PUB)?;

        let addr = format!("tcp://*:{}", config.publish_port);
        socket.bind(&addr)?;
        println!("[RUST] Publisher bound to {}", addr);

        // Give subscribers time to connect
        thread::sleep(Duration::from_secs(1));

        Ok(FramePublisher {
            config,
            context,
            socket,
            cap: None,
            start_time: Instant::now(),
            frame_count: 0,
            running,
        })
    }

    fn load_config() -> Result<Config, Box<dyn std::error::Error>> {
        let mut config = Config::default();

        // Try to load from file
        let config_path = std::env::var("FRAME_PUBLISHER_CONFIG")
            .unwrap_or_else(|_| "/etc/jvideo/frame-publisher.conf".to_string());

        if let Ok(contents) = fs::read_to_string(&config_path) {
            if let Ok(user_config) = serde_json::from_str::<Config>(&contents) {
                config = user_config;
                println!("[RUST] Loaded config from {}", config_path);
            }
        }

        // Override with environment variables
        if let Ok(source) = std::env::var("JVIDEO_SOURCE_TYPE") {
            config.source_type = match source.as_str() {
                "camera" => SourceType::Camera,
                _ => SourceType::Synthetic,
            };
        }

        Ok(config)
    }

    fn init_camera(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        println!("[RUST] Initializing camera device {}", self.config.camera_device);

        let mut cap = videoio::VideoCapture::new(self.config.camera_device, videoio::CAP_ANY)?;

        if !cap.is_opened()? {
            println!("[RUST] Failed to open camera, falling back to synthetic");
            self.config.source_type = SourceType::Synthetic;
            return self.init_synthetic();
        }

        // Configure camera
        cap.set(videoio::CAP_PROP_FRAME_WIDTH, self.config.width as f64)?;
        cap.set(videoio::CAP_PROP_FRAME_HEIGHT, self.config.height as f64)?;
        cap.set(videoio::CAP_PROP_FPS, self.config.fps as f64)?;
        cap.set(videoio::CAP_PROP_BUFFERSIZE, 1.0)?;

        // Get actual settings
        let actual_width = cap.get(videoio::CAP_PROP_FRAME_WIDTH)? as i32;
        let actual_height = cap.get(videoio::CAP_PROP_FRAME_HEIGHT)? as i32;
        let actual_fps = cap.get(videoio::CAP_PROP_FPS)?;

        println!("[RUST] Camera initialized: {}x{} @ {}fps",
                 actual_width, actual_height, actual_fps);

        self.config.width = actual_width;
        self.config.height = actual_height;
        if actual_fps > 0.0 {
            self.config.fps = actual_fps as i32;
        }

        self.cap = Some(cap);
        Ok(())
    }

    fn init_synthetic(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        println!("[RUST] Synthetic source initialized: {}x{} @ {}fps, pattern: {}",
                 self.config.width, self.config.height, self.config.fps, self.config.synthetic_pattern);
        Ok(())
    }

    fn generate_synthetic_frame(&self) -> Result<Mat, Box<dyn std::error::Error>> {
        let mut frame = Mat::zeros(self.config.height, self.config.width, core::CV_8UC3)?;

        let t = self.start_time.elapsed().as_secs_f64() * self.config.synthetic_motion_speed;

        match self.config.synthetic_pattern.as_str() {
            "moving_shapes" => {
                // Background gradient
                for y in 0..self.config.height {
                    let val = (20.0 + 10.0 * (y as f64 / 50.0 + t * 0.5).sin()) as u8;
                    for x in 0..self.config.width {
                        let pixel = frame.at_2d_mut::<core::Vec3b>(y, x)?;
                        pixel[0] = val / 3;
                        pixel[1] = val / 2;
                        pixel[2] = val;
                    }
                }

                // Moving circles
                for i in 0..3 {
                    let phase = 2.0 * PI * i as f64 / 3.0;
                    let cx = (self.config.width as f64 * (0.5 + 0.3 * (t + phase).sin())) as i32;
                    let cy = (self.config.height as f64 * (0.5 + 0.3 * (t * 0.7 + phase).cos())) as i32;

                    let color = core::Scalar::new(
                        127.0 + 127.0 * (t + phase).sin(),
                        127.0 + 127.0 * (t + phase + 2.0 * PI / 3.0).sin(),
                        127.0 + 127.0 * (t + phase + 4.0 * PI / 3.0).sin(),
                        0.0
                    );

                    imgproc::circle(
                        &mut frame,
                        core::Point::new(cx, cy),
                        30,
                        color,
                        -1,
                        imgproc::LINE_8,
                        0
                    )?;
                }

                // Moving rectangle
                let rx = (self.config.width as f64 * (0.5 + 0.4 * (t * 0.8).cos())) as i32;
                let ry = self.config.height / 3;

                imgproc::rectangle(
                    &mut frame,
                    core::Rect::new(rx - 40, ry - 20, 80, 40),
                    core::Scalar::new(0.0, 200.0, 255.0, 0.0),
                    -1,
                    imgproc::LINE_8,
                    0
                )?;
            },
            "color_bars" => {
                // SMPTE-style color bars
                let colors = vec![
                    core::Scalar::new(192.0, 192.0, 192.0, 0.0),  // Gray
                    core::Scalar::new(0.0, 192.0, 192.0, 0.0),    // Yellow
                    core::Scalar::new(192.0, 192.0, 0.0, 0.0),    // Cyan
                    core::Scalar::new(0.0, 192.0, 0.0, 0.0),      // Green
                    core::Scalar::new(192.0, 0.0, 192.0, 0.0),    // Magenta
                    core::Scalar::new(0.0, 0.0, 192.0, 0.0),      // Red
                    core::Scalar::new(192.0, 0.0, 0.0, 0.0),      // Blue
                ];

                let bar_width = self.config.width / colors.len() as i32;
                for (i, color) in colors.iter().enumerate() {
                    let x_start = i as i32 * bar_width;
                    let x_end = if i == colors.len() - 1 { self.config.width } else { (i as i32 + 1) * bar_width };

                    imgproc::rectangle(
                        &mut frame,
                        core::Rect::new(x_start, 0, x_end - x_start, self.config.height),
                        *color,
                        -1,
                        imgproc::LINE_8,
                        0
                    )?;
                }

                // Animated line
                let line_y = (self.config.height as f64 * (0.5 + 0.4 * t.sin())) as i32;
                imgproc::line(
                    &mut frame,
                    core::Point::new(0, line_y),
                    core::Point::new(self.config.width, line_y),
                    core::Scalar::new(255.0, 255.0, 255.0, 0.0),
                    2,
                    imgproc::LINE_8,
                    0
                )?;
            },
            _ => {
                // Checkerboard pattern
                let square_size = 40;
                let offset = ((t * 10.0) as i32) % (square_size * 2);

                for y in (0..self.config.height).step_by(square_size as usize) {
                    for x in (0..self.config.width).step_by(square_size as usize) {
                        if ((x + offset) / square_size + y / square_size) % 2 == 0 {
                            imgproc::rectangle(
                                &mut frame,
                                core::Rect::new(
                                    x, y,
                                    (square_size).min(self.config.width - x),
                                    (square_size).min(self.config.height - y)
                                ),
                                core::Scalar::new(255.0, 255.0, 255.0, 0.0),
                                -1,
                                imgproc::LINE_8,
                                0
                            )?;
                        }
                    }
                }
            }
        }

        Ok(frame)
    }

    fn add_overlays(&self, frame: &mut Mat) -> Result<(), Box<dyn std::error::Error>> {
        if self.config.add_source_label {
            let label = match self.config.source_type {
                SourceType::Camera => "CAMERA".to_string(),
                SourceType::Synthetic => format!("SYNTHETIC ({})", self.config.synthetic_pattern),
            };

            imgproc::put_text(
                frame,
                &label,
                core::Point::new(10, 30),
                imgproc::FONT_HERSHEY_SIMPLEX,
                0.7,
                core::Scalar::new(255.0, 255.0, 255.0, 0.0),
                2,
                imgproc::LINE_8,
                false
            )?;
        }

        if self.config.add_frame_number {
            imgproc::put_text(
                frame,
                &format!("Frame: {}", self.frame_count),
                core::Point::new(10, 60),
                imgproc::FONT_HERSHEY_SIMPLEX,
                0.6,
                core::Scalar::new(255.0, 255.0, 255.0, 0.0),
                1,
                imgproc::LINE_8,
                false
            )?;
        }

        if self.config.add_timestamp {
            let now = chrono::Local::now();
            imgproc::put_text(
                frame,
                &now.format("%H:%M:%S").to_string(),
                core::Point::new(self.config.width - 100, 30),
                imgproc::FONT_HERSHEY_SIMPLEX,
                0.6,
                core::Scalar::new(255.0, 255.0, 255.0, 0.0),
                1,
                imgproc::LINE_8,
                false
            )?;
        }

        // FPS indicator
        let elapsed = self.start_time.elapsed().as_secs_f64();
        if elapsed > 0.0 {
            let fps = self.frame_count as f64 / elapsed;
            imgproc::put_text(
                frame,
                &format!("FPS: {:.0}", fps),
                core::Point::new(self.config.width - 100, 60),
                imgproc::FONT_HERSHEY_SIMPLEX,
                0.6,
                core::Scalar::new(255.0, 255.0, 255.0, 0.0),
                1,
                imgproc::LINE_8,
                false
            )?;
        }

        // Rust indicator
        imgproc::put_text(
            frame,
            "[RUST]",
            core::Point::new(self.config.width - 60, self.config.height - 10),
            imgproc::FONT_HERSHEY_SIMPLEX,
            0.5,
            core::Scalar::new(128.0, 128.0, 128.0, 0.0),
            1,
            imgproc::LINE_8,
            false
        )?;

        Ok(())
    }

    fn run(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        // Initialize source
        match self.config.source_type {
            SourceType::Camera => self.init_camera()?,
            SourceType::Synthetic => self.init_synthetic()?,
        }

        println!("[RUST] Starting frame publishing from {:?}", self.config.source_type);

        let frame_duration = Duration::from_millis(1000 / self.config.fps as u64);
        let mut last_log_time = Instant::now();
        let mut frames_processed = 0u64;
        let mut errors = 0u64;

        while self.running.load(Ordering::SeqCst) {
            let frame_start = Instant::now();

            // Get frame
            let mut frame = match self.config.source_type {
                SourceType::Camera => {
                    if let Some(ref mut cap) = self.cap {
                        let mut frame = Mat::default();
                        cap.read(&mut frame)?;
                        if frame.empty() {
                            println!("[RUST] Failed to capture frame");
                            thread::sleep(Duration::from_millis(100));
                            continue;
                        }
                        frame
                    } else {
                        continue;
                    }
                },
                SourceType::Synthetic => self.generate_synthetic_frame()?,
            };

            // Add overlays
            self.add_overlays(&mut frame)?;

            // Prepare metadata
            let metadata = serde_json::json!({
                "frame_id": self.frame_count,
                "timestamp": SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs_f64(),
                "width": frame.cols(),
                "height": frame.rows(),
                "channels": 3,
                "source_type": match self.config.source_type {
                    SourceType::Camera => "camera",
                    SourceType::Synthetic => "synthetic",
                },
                "implementation": "rust"
            });

            // Send metadata
            let meta_str = metadata.to_string();
            self.socket.send(&meta_str, zmq::SNDMORE)?;

            // Send frame data
            let frame_data = frame.data_bytes()?;
            self.socket.send(frame_data, 0)?;

            self.frame_count += 1;
            frames_processed += 1;

            // Log progress periodically
            if last_log_time.elapsed() >= Duration::from_secs(5) {
                let elapsed = self.start_time.elapsed().as_secs_f64();
                let fps = frames_processed as f64 / elapsed;
                println!("[RUST] Processed {} frames, FPS: {:.1}, Errors: {}",
                         frames_processed, fps, errors);
                last_log_time = Instant::now();
            }

            // Maintain frame rate
            let elapsed = frame_start.elapsed();
            if elapsed < frame_duration {
                thread::sleep(frame_duration - elapsed);
            }
        }

        println!("[RUST] Frame publishing stopped. Total frames: {}", frames_processed);
        Ok(())
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Setup signal handling
    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();

    ctrlc::set_handler(move || {
        println!("\n[RUST] Received interrupt signal, shutting down...");
        r.store(false, Ordering::SeqCst);
    })?;

    println!("[RUST] Juni's Frame Publisher (Rust Implementation)");

    // Parse command line arguments
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 {
        match args[1].as_str() {
            "camera" | "synthetic" => {
                std::env::set_var("JVIDEO_SOURCE_TYPE", &args[1]);
                println!("[RUST] Using source type: {}", args[1]);
            },
            _ => {}
        }
    }

    let mut publisher = FramePublisher::new(running)?;
    publisher.run()?;

    Ok(())
}
