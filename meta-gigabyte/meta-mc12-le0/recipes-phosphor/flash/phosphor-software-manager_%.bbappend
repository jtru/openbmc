FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://bios-update-mc12-le0"

PACKAGECONFIG:append = " flash_bios"
RDEPENDS:${PN} += "bash flashrom phosphor-ipmi-ipmb"

do_install:append() {
    install -d ${D}/${bindir}
    install -m 0755 ${UNPACKDIR}/bios-update-mc12-le0 ${D}/${bindir}/
}
