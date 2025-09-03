#
# SPDX-License-Identifier: MIT
# This file Copyright (c) 2025 Johannes Truschnigg <johannes@truschnigg.info>
# Based on pre-existing work Copyright (c) 2023 Tano Systems LLC
#
SUMMARY = "IPMItool FRU Information generator / editor"
DESCRIPTION = "Universal, full-featured IPMI FRU Information \
generator / editor library and command line tool, written in \
full compliance with IPMI FRU Information Storage Definition \
v1.0, rev. 1.3."
HOMEPAGE = "https://codeberg.org/IPMItool/frugen"
SECTION = "tools"

LICENSE = "Apache-2.0 | GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://README.md;beginline=5;endline=13;md5=f637ad9cfe4cc1885e0b822fbfb8c95a"

PV = "v3.0${SRCPV}"

DEPENDS += "json-c"

inherit cmake pkgconfig

SRC_URI = "git://codeberg.org/IPMITool/frugen.git;branch=master;protocol=https"
SRCREV = "8cb4c9721b967b2083127672e94ef70a4d588b6f"
SRC_URI[sha256sum] = "3c1b6e639491f6fd8cd2483203710c92c63b77b374be708226d2bf8667a1aef2"

S = "${WORKDIR}/git"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}/patches:"

SRC_URI += "\
        file://0001-Unbounded-strncpy-produces-a-nasty-warning-here.patch \
"

SOLIBS = ".so"

FILES:${PN} = "${bindir}/frugen \
               ${libdir}/libfru.so.3 \
               ${libdir}/libfru.so.3.0.0 \
"

###BBCLASSEXTEND = "native nativesdk"
