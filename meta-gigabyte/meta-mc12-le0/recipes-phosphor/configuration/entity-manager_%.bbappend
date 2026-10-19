FILESEXTRAPATHS:append := ":${THISDIR}/${PN}"
SRC_URI:append = " \
        file://blacklist.json \
        file://gigabyte_mc12_le0_00.json \
    "

do_install:append() {
     install -m 0444 ${UNPACKDIR}/blacklist.json -D -t ${D}${datadir}/entity-manager
     # entity-manager walks configurations/ recursively, and preserves the
     # subdirectory layout, so keep the vendor subdir the upstream fork uses.
     install -m 0444 ${UNPACKDIR}/gigabyte_mc12_le0_00.json -D \
        -t ${D}${datadir}/entity-manager/configurations/gigabyte
}
