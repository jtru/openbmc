FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI += " \
    file://power-config-host0.json \
    "

# This board routes the front-panel power/reset buttons through the BMC: the
# button drives the BMC's button-power-n input, and the BMC has to forward the
# press to the host by driving control-power-n.
#
# Without button-passthrough, powerStateOff's handler for
# Event::powerButtonPressed only advances the state machine to waitForPowerOK
# and never asserts the PowerOut line, so a physical press does nothing. The
# WebUI/IPMI path is unaffected because it arrives as Event::powerOnRequest,
# which does call powerOn().
#
# Upstream defaults this to disabled, presumably because on most boards the
# button is wired straight to the host's PWRBTN# and the BMC only observes it.
EXTRA_OEMESON:append = " -Dbutton-passthrough=enabled"

do_install:append() {
    install -d  ${D}/${datadir}/${PN}
    install -m 0644 ${UNPACKDIR}/power-config-host0.json ${D}/${datadir}/${PN}
}
