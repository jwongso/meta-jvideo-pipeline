require recipes-core/images/core-image-minimal.bb

SUMMARY = "Juni's Video Processing Pipeline Image"
DESCRIPTION = "Custom image for video processing microservices"
LICENSE = "MIT"

# Add SSH server for remote access
IMAGE_FEATURES += "ssh-server-openssh"

# Add development/debugging tools
EXTRA_IMAGE_FEATURES += "debug-tweaks tools-debug"

# QEMU memory settings
QB_MEM = "-m 1024"

# Install systemd
IMAGE_INSTALL += " \
    systemd \
    systemd-analyze \
    systemd-serialgetty \
    "

# Install our video pipeline services
IMAGE_INSTALL += " \
    jvideo-services \
    "

# Add useful system tools
IMAGE_INSTALL += " \
    htop \
    vim \
    nano \
    python3 \
    python3-psutil \
    python3-msgpack \
    python3-fcntl \
    sqlite3 \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    "

# FFmpeg and video libraries
IMAGE_INSTALL += " \
    ffmpeg \
    libavcodec \
    libavformat \
    libavutil \
    "

# Content package
IMAGE_INSTALL += "jvideo-content"

# Increase root filesystem size (in KB)
VIDEO_STORAGE_SIZE_GB = "5"
IMAGE_ROOTFS_EXTRA_SPACE = "${@int('${VIDEO_STORAGE_SIZE_GB}') * 1024 * 1024}"

# Post-processing to set up initial configuration
ROOTFS_POSTPROCESS_COMMAND += "configure_jvideo; enable_jvideo_services; "

configure_jvideo() {
    # Create directory for config
    install -d ${IMAGE_ROOTFS}/etc/jvideo

    # Create initial configuration
    echo '{' > ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '  "services": {' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    "frame-publisher": {' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "enabled": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "language": "cpp",' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "auto_restart": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "restart_delay": 5' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    },' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    "frame-resizer": {' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "enabled": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "language": "cpp",' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "auto_restart": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "restart_delay": 5' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    },' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    "frame-saver": {' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "enabled": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "language": "cpp",' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "auto_restart": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "restart_delay": 5' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    },' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    "queue-monitor": {' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "enabled": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "language": "cpp",' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "auto_restart": true,' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '      "restart_delay": 5' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '    }' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '  }' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf
    echo '}' >> ${IMAGE_ROOTFS}/etc/jvideo/services.conf

    # Create output directory
    install -d ${IMAGE_ROOTFS}/var/lib/jvideo/frames
    install -d ${IMAGE_ROOTFS}/var/lib/jvideo/db
    chmod 755 ${IMAGE_ROOTFS}/var/lib/jvideo/frames
    chmod 755 ${IMAGE_ROOTFS}/var/lib/jvideo/db
}

enable_jvideo_services() {
    # Create systemd autostart script
    install -d ${IMAGE_ROOTFS}/etc/systemd/system/multi-user.target.wants/

    # Create a service to initialize database and start pipeline
    cat > ${IMAGE_ROOTFS}/etc/systemd/system/jvideo-pipeline.service << 'EOF'
[Unit]
Description=Juni's Video Pipeline Autostart
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/bin/jvideo-control apply /etc/jvideo/services.conf
RemainAfterExit=true
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

    # Enable the autostart service
    ln -sf /etc/systemd/system/jvideo-pipeline.service ${IMAGE_ROOTFS}/etc/systemd/system/multi-user.target.wants/jvideo-pipeline.service

    # Create startup script for manual control
    cat > ${IMAGE_ROOTFS}/etc/profile.d/jvideo-welcome.sh << 'EOF'
#!/bin/sh
echo ""
echo "=========================================================="
echo "Welcome to Juni's Video Pipeline!"
echo "=========================================================="
echo "To control services:"
echo "  jvideo-control status       - Show service status"
echo "  jvideo-control start <svc>  - Start a service"
echo "  jvideo-control stop <svc>   - Stop a service"
echo "  jvideo-control apply        - Apply configuration"
echo "  jvideo-dashboard            - Open monitoring dashboard"
echo ""
echo "Available services: frame-publisher, frame-resizer, frame-saver, queue-monitor"
echo "Saved frames: /var/lib/jvideo/frames/"
echo "Database: /var/lib/jvideo/db/"
echo "Shared memory: /dev/shm/jvideo_*"
echo "=========================================================="
echo ""
EOF
    chmod 755 ${IMAGE_ROOTFS}/etc/profile.d/jvideo-welcome.sh
}
