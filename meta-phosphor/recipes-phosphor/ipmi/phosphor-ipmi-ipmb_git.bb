SUMMARY = "IPMB bridge"
DESCRIPTION = "The IPMB bridge implements a Dbus compliant interface for \
implementing IPMB interfaces"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=e3fc50a88d0a364313df4b21ef20c29e"
DEPENDS = "sdbusplus \
           phosphor-logging \
           i2c-tools \
           boost \
           nlohmann-json"
SRCREV = "00c8382c202b82dfea901a9e9f1001a5d1bc17df"
PV = "0.1+git${SRCPV}"

SRC_URI = "git://github.com/openbmc/ipmbbridge.git;branch=master;protocol=https"

SYSTEMD_SERVICE:${PN} = "ipmb.service"
S = "${WORKDIR}/git"

inherit meson pkgconfig systemd

FILES:${PN} += "${datadir}/ipmbbridge/ipmb-channels.json"
