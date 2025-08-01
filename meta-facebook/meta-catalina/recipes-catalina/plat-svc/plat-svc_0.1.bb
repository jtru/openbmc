LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

inherit allarch systemd obmc-phosphor-systemd

S = "${WORKDIR}/sources"
UNPACKDIR = "${S}"

RDEPENDS:${PN} += "bash"
RDEPENDS:${PN} += "libgpiod-tools"
RDEPENDS:${PN} += "fb-common-functions"

SRC_URI:append = " \
    file://frontend-nic-temp-read \
    file://frontend-nic-temp-read.service \
    file://osfp-eeprom-driver-bind \
    file://osfp-eeprom-driver-bind.service \
    file://platform-early-sys-init \
    file://platform-sys-init.service \
    file://platform-sys-init.service \
    file://standby-power-enable \
    "

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN}:append = " \
    frontend-nic-temp-read.service \
    osfp-eeprom-driver-bind.service \
    platform-sys-init.service \
    "

do_install() {
    PLATSVC_LIBEXECDIR="${D}${libexecdir}/plat-svc"
    install -d ${PLATSVC_LIBEXECDIR}

    install -m 0755 ${UNPACKDIR}/frontend-nic-temp-read ${PLATSVC_LIBEXECDIR}
    install -m 0755 ${UNPACKDIR}/osfp-eeprom-driver-bind ${PLATSVC_LIBEXECDIR}
    install -m 0755 ${UNPACKDIR}/platform-early-sys-init ${PLATSVC_LIBEXECDIR}
    install -m 0755 ${UNPACKDIR}/standby-power-enable ${PLATSVC_LIBEXECDIR}

}

SRC_URI:append:catalina = " \
    file://iob-nic-temp-read \
    file://iob-nic-temp-read.service \
    "

SYSTEMD_SERVICE:${PN}:append:catalina = " \
    iob-nic-temp-read.service \
    "

do_install:append:catalina() {
    PLATSVC_LIBEXECDIR="${D}${libexecdir}/plat-svc"
    install -d ${PLATSVC_LIBEXECDIR}
    install -m 0755 ${UNPACKDIR}/iob-nic-temp-read ${PLATSVC_LIBEXECDIR}
}