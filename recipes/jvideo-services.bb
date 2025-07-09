SUMMARY = "Juni's Video Pipeline Services"
DESCRIPTION = "Video processing microservices using ZeroMQ, MessagePack and SQLite - Python and C++ implementations"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Build dependencies (Boost removed)
DEPENDS = " \
    zeromq \
    cppzmq \
    opencv \
    ffmpeg \
    cmake-native \
    pkgconfig-native \
    nlohmann-json \
    sqlite3 \
    msgpack-c \
    msgpack-cpp \
    "

# Runtime dependencies (Boost removed)
RDEPENDS:${PN} = " \
    python3-core \
    python3-pyzmq \
    python3-opencv \
    python3-numpy \
    python3-json \
    python3-logging \
    python3-threading \
    python3-datetime \
    python3-psutil \
    python3-msgpack \
    python3-curses \
    python3-ctypes \
    python3-mmap \
    python3-fcntl \
    zeromq \
    sqlite3 \
    opencv \
    bash \
    ffmpeg \
    libavcodec \
    libavformat \
    libavutil \
    msgpack-c \
    "

# Source files
SRC_URI = " \
    file://service_base.py \
    file://service_controller.py \
    file://frame_publisher.py \
    file://frame_publisher.cpp \
    file://frame_resizer.py \
    file://frame_resizer.cpp \
    file://frame_saver.py \
    file://frame_saver.cpp \
    file://queue_monitor.py \
    file://queue_monitor.cpp \
    file://metrics_interface.h \
    file://frame_tracking.h \
    file://pipeline_stage.h \
    file://logger.h \
    file://pipeline_stage.py \
    file://CMakeLists.txt \
    file://frame-publisher.conf \
    file://frame-resizer.conf \
    file://frame-saver.conf \
    file://queue-monitor.conf \
    file://jvideo-dashboard.sh \
    file://jvideo-frame-publisher-python.service \
    file://jvideo-frame-publisher-cpp.service \
    file://jvideo-frame-resizer-python.service \
    file://jvideo-frame-resizer-cpp.service \
    file://jvideo-frame-saver-python.service \
    file://jvideo-frame-saver-cpp.service \
    file://jvideo-queue-monitor-cpp.service \
    file://jvideo-queue-monitor-python.service \
    "

S = "${WORKDIR}"

inherit systemd cmake pkgconfig python3-dir

# Updated to include all services
SYSTEMD_SERVICE:${PN} = " \
    jvideo-frame-publisher-python.service \
    jvideo-frame-publisher-cpp.service \
    jvideo-frame-resizer-python.service \
    jvideo-frame-resizer-cpp.service \
    jvideo-frame-saver-python.service \
    jvideo-frame-saver-cpp.service \
    jvideo-queue-monitor-cpp.service \
    jvideo-queue-monitor-python.service \
    "

SYSTEMD_AUTO_ENABLE:${PN} = "disable"

OECMAKE_SOURCEPATH = "${S}"
OECMAKE_BUILDPATH = "${WORKDIR}/build"

EXTRA_OECMAKE = " \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/jvideo \
    -DSQLITE3_INCLUDE_DIRS=${STAGING_INCDIR} \
    -DSQLITE3_LIBRARIES=${STAGING_LIBDIR}/libsqlite3.so \
    "

do_install() {
    # Create directory structure
    install -d ${D}/opt/jvideo
    install -d ${D}/opt/jvideo/services
    install -d ${D}/opt/jvideo/bin
    install -d ${D}/etc/jvideo
    install -d ${D}/var/lib/jvideo
    install -d ${D}/var/lib/jvideo/frames
    install -d ${D}/var/lib/jvideo/db
    install -d ${D}/usr/bin

    # Create tmpfiles.d configuration for directories
    install -d ${D}${sysconfdir}/tmpfiles.d
    echo "d /var/log/jvideo 0755 root root -" > ${D}${sysconfdir}/tmpfiles.d/jvideo.conf
    echo "d /var/lib/jvideo 0755 root root -" >> ${D}${sysconfdir}/tmpfiles.d/jvideo.conf
    echo "d /var/lib/jvideo/frames 0755 root root -" >> ${D}${sysconfdir}/tmpfiles.d/jvideo.conf
    echo "d /var/lib/jvideo/db 0755 root root -" >> ${D}${sysconfdir}/tmpfiles.d/jvideo.conf

    # Install Python services
    install -m 0755 ${S}/service_base.py ${D}/opt/jvideo/services/
    install -m 0755 ${S}/service_controller.py ${D}/opt/jvideo/services/
    install -m 0755 ${S}/frame_publisher.py ${D}/opt/jvideo/services/
    install -m 0755 ${S}/frame_resizer.py ${D}/opt/jvideo/services/
    install -m 0755 ${S}/frame_saver.py ${D}/opt/jvideo/services/
    install -m 0755 ${S}/queue_monitor.py ${D}/opt/jvideo/services/
    install -m 0755 ${S}/pipeline_stage.py ${D}/opt/jvideo/services/

    # Install C++ binaries
    if [ -f ${WORKDIR}/build/frame-publisher-cpp ]; then
        install -m 0755 ${WORKDIR}/build/frame-publisher-cpp ${D}/opt/jvideo/bin/
    else
        bbfatal "C++ frame-publisher binary not found"
    fi

    if [ -f ${WORKDIR}/build/frame-resizer-cpp ]; then
        install -m 0755 ${WORKDIR}/build/frame-resizer-cpp ${D}/opt/jvideo/bin/
    else
        bbfatal "C++ frame-resizer binary not found"
    fi

    if [ -f ${WORKDIR}/build/frame-saver-cpp ]; then
        install -m 0755 ${WORKDIR}/build/frame-saver-cpp ${D}/opt/jvideo/bin/
    else
        bbfatal "C++ frame-saver binary not found"
    fi

    if [ -f ${WORKDIR}/build/queue-monitor-cpp ]; then
        install -m 0755 ${WORKDIR}/build/queue-monitor-cpp ${D}/opt/jvideo/bin/
    else
        bbfatal "C++ queue-monitor binary not found"
    fi

    # Install configuration files
    install -m 0644 ${S}/frame-publisher.conf ${D}/etc/jvideo/
    install -m 0644 ${S}/frame-resizer.conf ${D}/etc/jvideo/
    install -m 0644 ${S}/frame-saver.conf ${D}/etc/jvideo/
    install -m 0644 ${S}/queue-monitor.conf ${D}/etc/jvideo/

    # Install systemd service files
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/jvideo-frame-publisher-python.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/jvideo-frame-publisher-cpp.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/jvideo-frame-resizer-python.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/jvideo-frame-resizer-cpp.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/jvideo-frame-saver-python.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/jvideo-frame-saver-cpp.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/jvideo-queue-monitor-cpp.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/jvideo-queue-monitor-python.service ${D}${systemd_system_unitdir}/

    # Create control script
    echo '#!/bin/bash' > ${D}/usr/bin/jvideo-control
    echo 'exec /usr/bin/python3 /opt/jvideo/services/service_controller.py "$@"' >> ${D}/usr/bin/jvideo-control
    chmod 0755 ${D}/usr/bin/jvideo-control

    install -m 0755 ${S}/jvideo-dashboard.sh ${D}/usr/bin/
}

FILES:${PN} += " \
    /opt/jvideo \
    /etc/jvideo \
    /var/lib/jvideo \
    /usr/bin/jvideo-control \
    /usr/bin/jvideo-dashboard \
    ${sysconfdir}/tmpfiles.d/jvideo.conf \
    "

CONFFILES:${PN} = " \
    /etc/jvideo/frame-publisher.conf \
    /etc/jvideo/frame-resizer.conf \
    /etc/jvideo/frame-saver.conf \
    /etc/jvideo/queue-monitor.conf \
    "

INSANE_SKIP:${PN} += "ldflags"
INSANE_SKIP:${PN} += "file-rdeps"
