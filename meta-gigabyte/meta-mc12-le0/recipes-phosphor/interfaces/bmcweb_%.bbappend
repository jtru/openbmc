# bmcweb's default HTTP request body limit is 30 MiB.  The generated
# mc12-le0 BMC update tarball is already ~33 MiB (image-rofs + kernel +
# u-boot + rwfs + MANIFEST), so uploads through the web UI/Redfish are
# rejected before phosphor-software-manager sees them.
#
# 65 MiB matches meta-ampere and meta-bytedance, and leaves 1 MiB over a
# 64 MiB payload for multipart framing and headers.  It remains well below
# the limits used by larger BMC platforms:
#   meta-amd 128, meta-facebook 256, meta-ibm 400.
#
# This is a compile-time option; it also controls the Redfish
# UpdateService.MaxImageSizeBytes value.
EXTRA_OEMESON:append = " \
    -Dhttp-body-limit=65 \
"
