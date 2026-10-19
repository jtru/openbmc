# Debug aid: log every IPMI command for which no handler is registered
# (NetFn/Cmd/channel/payload), so in-band BIOS probes such as SMBIOS MDR
# traffic can be observed in the ipmid journal without guessing.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI += "file://log-unhandled-ipmi.patch"