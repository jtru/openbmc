SUMMARY = "An implementation of the GEIS interface"
DESCRIPTION = "An implementation of the GEIS (Gesture Engine Interface and Support) \
interface\
GEIS is a library for applications and toolkit programmers which \
provides a consistent platform independent interface for any \
system-wide input gesture recognition mechanism."

HOMEPAGE = "https://launchpad.net/geis"

LICENSE = "GPL-3.0-only & LGPL-3.0-only"
LIC_FILES_CHKSUM = " \
    file://COPYING;md5=6a6a8e020838b23406c81b19c1d46df6 \
    file://COPYING.GPL;md5=f27defe1e96c2e1ecd4e0c9be8967949 \
"

inherit autotools pkgconfig python3native lib_package features_check

REQUIRED_DISTRO_FEATURES = "x11 opengl"

DEPENDS += "grail dbus-glib python3 virtual/libx11 libxext libxi libxcb dbus frame"

SRC_URI = "https://launchpad.net/${BPN}/trunk/${PV}/+download/${BPN}-${PV}.tar.xz \
           file://fix-indentation-for-gcc6.patch \
           file://0001-libgeis-Compare-the-first-character-of-string-to-nul.patch \
           file://0001-provide-function-declaration-with-prototypes.patch \
           "

UPSTREAM_CHECK_URI = "https://launchpad.net/geis/trunk"

SRC_URI[sha256sum] = "8a60f5683852094038904e690d23cc5a90a980fc52da67f0f28890baa25c70eb"

EXTRA_OECONF = "--disable-integration-tests"

FILES:${PN}-bin = "${bindir}"
RDEPENDS:${PN}-bin = " \
    python3-compression \
    python3-core \
    python3-crypt \
    python3-ctypes \
    python3-fcntl \
    python3-misc \
    python3-pickle \
    python3-shell \
    python3-stringold \
    python3-threading \
"

FILES:${PN} += " \
    ${datadir}/geisview \
    ${PYTHON_SITEPACKAGES_DIR}/geis* \
    ${PYTHON_SITEPACKAGES_DIR}/_*.so \
"

FILES:${PN}-dbg += "${PYTHON_SITEPACKAGES_DIR}/.debug"

FILES:${PN}-dev += "${PYTHON_SITEPACKAGES_DIR}/_*.la"

FILES:${PN}-staticdev += "${PYTHON_SITEPACKAGES_DIR}/_*.a"
