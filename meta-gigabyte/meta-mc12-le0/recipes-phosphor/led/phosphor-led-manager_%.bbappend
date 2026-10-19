# Provide the LED group config phosphor-led-manager needs.
# Without a config file the daemon waits for entity-manager to publish a
# Compatible interface with a matching config name; this machine has none,
# so the daemon never completed startup, multi-user.target never finished
# and bmc0 stayed NotReady forever. phosphor-ipmi-host then reported the
# BMC unavailable in Get Device ID (fw rev byte1 bit7), and the host BIOS
# read that as "Force Update Mode" in its BMC self-test report.
# Minimal groups so obmc-led-group-start@bmc_booted can also complete;
# enclosure_identify drives the rear ID LED (led-rear-id).

FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

SRC_URI += "file://led-group-config.json"

do_install:append() {
    install -m 0400 ${UNPACKDIR}/led-group-config.json \
        -D ${D}${datadir}/phosphor-led-manager/led-group-config.json
}
