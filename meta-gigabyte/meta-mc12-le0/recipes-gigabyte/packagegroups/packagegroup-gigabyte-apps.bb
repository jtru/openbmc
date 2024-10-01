SUMMARY = "OpenBMC for Gigabyte - Applications"
PR = "r1"

inherit packagegroup

PROVIDES = "${PACKAGES}"
PACKAGES = " \
        ${PN}-flash \
        ${PN}-system \
        "

PROVIDES += "virtual/obmc-flash-mgmt"
PROVIDES += "virtual/obmc-system-mgmt"

RPROVIDES:${PN}-flash += "virtual-obmc-flash-mgmt"
RPROVIDES:${PN}-system += "virtual-obmc-system-mgmt"

SUMMARY:${PN}-flash = "Gigabyte Flash"
RDEPENDS:${PN}-flash = " \
        phosphor-ipmi-flash \
        "

SUMMARY:${PN}-system = "Gigabyte System"
RDEPENDS:${PN}-system = " \
        phosphor-host-postd \
        phosphor-post-code-manager \
        phosphor-power-regulators \
        entity-manager \
        phosphor-pid-control \
        phosphor-software-manager \
        iperf3 \
        picocom \
        ipmitool \
        strace \
        "
