SUMMARY = "Nicole OCC Control sensor IDs"
PR = "r1"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

inherit openpower-occ-control
inherit native

PROVIDES += "virtual/openpower-occ-control-config-native"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://occ_sensor.yaml"

S = "${WORKDIR}/sources"
UNPACKDIR = "${S}"
do_install() {
    install -d ${YAML_DEST}/
    install ${S}/occ_sensor.yaml ${YAML_DEST}/
}
