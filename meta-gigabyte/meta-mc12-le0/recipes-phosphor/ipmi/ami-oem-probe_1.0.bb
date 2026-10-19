SUMMARY = "Bring-up probe for AMI's in-band OEM IPMI protocol"
DESCRIPTION = "Claims the two OEM IANA numbers the MC12-LE0's host BIOS \
addresses the BMC with on NetFn 0x2E, logs every command under them with its \
full body, and answers ccSuccess. It exists to observe AMI's proprietary \
in-band inventory/SMBIOS transfer, whose real handler lives in AMI's compiled \
IPMI stack and is therefore absent from an OpenBMC image. It is a discovery \
instrument, not an implementation: remove it once the protocol is understood \
or abandoned."

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

DEPENDS = "phosphor-ipmi-host phosphor-logging sdbusplus"

# Sources live in the sibling ${PN} directory, matching the other recipes in
# this directory (see phosphor-ipmi-host_%.bbappend).
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI = " \
    file://main.cpp;subdir=ami-oem-probe \
    file://meson.build;subdir=ami-oem-probe \
    "

S = "${UNPACKDIR}/ami-oem-probe"

inherit meson pkgconfig

# ipmid dlopens every regular .so it finds in this directory; no symlink or
# registration file is needed (ipmid-new.cpp loadProviders).
FILES:${PN} += "${libdir}/ipmid-providers"
