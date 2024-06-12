FILESEXTRAPATHS:prepend := "${THISDIR}/linux-aspeed:"
SRC_URI:append = " file://gigabyte-mc12-le0.dts"

do_patch:append() {
for DTB in ${KERNEL_DEVICETREE}
do
  cp -v "${WORKDIR}/${DTB%.dtb}.dts" "${STAGING_KERNEL_DIR}/arch/${ARCH}/boot/dts/"
done
}

