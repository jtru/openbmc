SUMMARY = "AMI-style USB host interface (bring-up probe)"
DESCRIPTION = "Presents a standard USB ethernet function (RNDIS in config 1, \
CDC-ECM in config 2, with AMI's USB IDs) to the host over the AST virtual hub, \
addressing it as the stock firmware does, and logs any HTTP request that \
arrives on it. The stock BMC serves this interface through AMI's own usbe/iUSB/\
eth kernel drivers; this reproduces the host-facing interface with the mainline \
gadget stack, to find out whether the host firmware will talk to it. A probe, \
not a finished feature -- see CLAUDE.md."

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

# systemd for pkg-config (the unit install dir); no other deps -- the listener
# is deliberately dependency-free.
DEPENDS = "systemd"

SRC_URI = " \
    file://gadget-setup.sh;subdir=host-if-gadget \
    file://host-if-listen.c;subdir=host-if-gadget \
    file://host-if-gadget.service;subdir=host-if-gadget \
    file://host-if-listen.service;subdir=host-if-gadget \
    file://meson.build;subdir=host-if-gadget \
    "

S = "${UNPACKDIR}/host-if-gadget"

inherit meson pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd"

SYSTEMD_SERVICE:${PN} = "host-if-gadget.service host-if-listen.service"
