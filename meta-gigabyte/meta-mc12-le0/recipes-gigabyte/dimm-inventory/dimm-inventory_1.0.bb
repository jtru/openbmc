SUMMARY = "Publish DDR4 SPD EEPROM contents as DIMM inventory"
DESCRIPTION = "Reads DDR4 SPD EEPROMs (ee1004) from the BMC I2C buses and \
publishes each populated DIMM as an xyz.openbmc_project.Inventory.Item.Dimm \
object. bmcweb serves those as Redfish Systems/<id>/Memory and webui-vue \
renders them under Hardware status -> Memory. Slots with no DIMM, or whose \
SPD fails its JEDEC CRC16 check, are skipped."

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

DEPENDS = "boost sdbusplus"

SRC_URI = " \
    file://main.cpp;subdir=dimm-inventory \
    file://smbios.cpp;subdir=dimm-inventory \
    file://smbios.hpp;subdir=dimm-inventory \
    file://spd.cpp;subdir=dimm-inventory \
    file://spd.hpp;subdir=dimm-inventory \
    file://meson.build;subdir=dimm-inventory \
    file://dimm-inventory.service;subdir=dimm-inventory \
    "

S = "${UNPACKDIR}/dimm-inventory"

inherit meson pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd"

SYSTEMD_SERVICE:${PN} = "dimm-inventory.service"
