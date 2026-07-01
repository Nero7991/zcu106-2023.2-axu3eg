SUMMARY = "Live SoC-temp / fan-PWM sensor monitor for the AXU3EG board"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://axu-mon"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/axu-mon ${D}${bindir}/axu-mon
}

FILES:${PN} = "${bindir}/axu-mon"
RDEPENDS:${PN} = "bash"
