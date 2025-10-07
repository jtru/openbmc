FILESEXTRAPATHS:prepend := "${THISDIR}:"

do_patch:append() {
for DTB in ${KERNEL_DEVICETREE}
do
  DT="${DTB##*/}"
  cp -v "${UNPACKDIR}/linux-aspeed/${DT%.dtb}.dts" "${STAGING_KERNEL_DIR}/arch/${ARCH}/boot/dts/aspeed/"
done
}

SRC_URI:append = " \
        file://linux-aspeed/gigabyte-mc12-le0.dts \
        file://linux-aspeed/mc12-le0.cfg \
        "
