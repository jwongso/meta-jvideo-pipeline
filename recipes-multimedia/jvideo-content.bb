SUMMARY = "JVideo demo content"
LICENSE = "CLOSED"

SRC_URI = "file://matterhorn.mp4"

do_install() {
    install -d ${D}${datadir}/jvideo/media
    install -m 0644 ${WORKDIR}/matterhorn.mp4 ${D}${datadir}/jvideo/media/
}

FILES:${PN} = "${datadir}/jvideo/media/matterhorn.mp4"
