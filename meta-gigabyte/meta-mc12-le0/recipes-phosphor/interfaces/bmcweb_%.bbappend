# bmcweb's default HTTP request body limit is 30 MiB.  The generated
# mc12-le0 BMC update tarball is already ~33 MiB (image-rofs + kernel +
# u-boot + rwfs + MANIFEST), so uploads through the web UI/Redfish are
# rejected before phosphor-software-manager sees them.
#
# 64 MiB gives room for rootfs growth and signing metadata while staying
# well below the limits used by larger BMC platforms:
#   meta-amd 128, meta-ampere 65, meta-facebook 256, meta-ibm 400.
#
# This is a compile-time option; it also controls the Redfish
# UpdateService.MaxImageSizeBytes value.
EXTRA_OEMESON:append = " \
    -Dhttp-body-limit=64 \
"
