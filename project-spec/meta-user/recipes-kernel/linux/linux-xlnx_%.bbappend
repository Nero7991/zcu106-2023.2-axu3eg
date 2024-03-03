FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://bsp.cfg"
KERNEL_FEATURES:append = " bsp.cfg"
SRC_URI += "file://user_2024-02-21-23-24-00.cfg \
            file://user_2024-02-22-04-38-00.cfg \
            "

