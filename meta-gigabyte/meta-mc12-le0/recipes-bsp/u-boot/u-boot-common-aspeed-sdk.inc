FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append:gigabyte-mc12-le0-u-boot-cfg = " file://mc12-le0-u-boot.cfg"
SRC_URI:append:gigabyte-mc12-le0-devicetree = " file://ast2500-gigabyte-mc12-le0.dts"
