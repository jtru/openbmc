require recipes-graphics/xorg-font/xorg-font-common.inc

SUMMARY = "Bitstream 100 DPI fonts"
HOMEPAGE = "http://cgit.freedesktop.org/xorg/font/bitstream-100dpi/"
SECTION = "x11/font"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://COPYING;md5=30330812324ff9d9bd9ea645bb944427"

DEPENDS = "util-macros-native font-util-native bdftopcf-native"
RDEPENDS:${PN} = "encodings font-util"
RDEPENDS:${PN}:class-native = "font-util-native"

UPSTREAM_CHECK_REGEX = "font\-bitstream\-100dpi\-(?P<pver>\d+(\.\d+)+).tar.gz"

inherit features_check
# depends on bdftopcf-native -> virtual/libx11
REQUIRED_DISTRO_FEATURES = "x11"

SRC_URI[sha256sum] = "ebe0d7444e3d7c8da7642055ac2206f0190ee060700d99cd876f8fc9964cb6ce"
