#!/bin/sh
#
# Present the AMI-style USB ethernet interface to the host over the AST vhub,
# and keep it usable.
#
# Why: the stock BMC's AMI stack (usbe + iUSB + eth) offers the host a standard
# USB ethernet function -- RNDIS (0x02/0x02/0xFF) in its first configuration --
# using AMI's USB IDs. See AMI's eth-src/eth_mod.c:CreateEthernetDescriptor in
# the OSSW-12-1 release. This reproduces that with the mainline gadget stack, so
# no AMI driver needs to be ported.
#
# From the BIOS side (RedfishHi DXE module, decompiled): at POST the BIOS's own
# UEFI stack takes a Simple-Network interface to this device and speaks TLS
# Redfish to  HTTPS://169.254.0.17:443  (it assigns itself 169.254.0.18/16 by
# default, with the same port defaults -- 0x1bb -- for anything else missing in
# that Setup variable). So what matters here is getting the device class and
# interface layout right, not what Windows binds to later.
#
# This is a bring-up probe, not a finished feature. See CLAUDE.md.
#
# NETWORK ONLY, on its own vhub port. The ethernet function is RNDIS in a
# single configuration -- an earlier revision offered RNDIS *and* CDC-ECM as
# two configurations, which creates two netdevs (usb0 and usb1) and leaves it
# ambiguous which one the host actually selected. IT MUST NOT CONTAIN HID: a
# composite that includes a HID function is rejected at descriptor-parse time
# by strict hosts (CONFIRMED LIVE 2026-10-16), which kills the link. HID for
# the KVM belongs on its own vhub port; see obmc-ikvm's create_usbhid.sh.
#
# Device class is 0x02/0x00/0x00. AMI's own builder sets device class 2 with
# zero sub-class/protocol (eth_mod.c:CreateEthernetDescriptor, whose
# bDeviceClass = 0xEF block is commented out). f_rndis always emits an
# interface-association descriptor, so 0xEF/0x02/0x01 is the "0x02-with-IAD"
# limb that Windows composite-enumerates but some hosts reject; the plain
# 0x02/0x00/0x00 shape is the one that enumerates and carries traffic here
# (CONFIRMED LIVE 2026-10-17). The BIOS-side UEFI NIC binds by interface class
# (0x02/0x02/0xFF), so the device class does not matter to it.
#
# The link addressing mirrors the stock firmware's host-interface defaults:
#   DEFAULT_HI_USBADDRESS 169.254.0.17
#   DEFAULT_HI_USBMASK    255.255.0.0

set -e

GADGET_NAME=host-if
GADGET=/sys/kernel/config/usb_gadget/$GADGET_NAME
IFACE=usb0
ADDR=169.254.0.17
PREFIXLEN=16
MAINTAIN_INTERVAL=5

# The ethernet always belongs on vhub port 1; port 2 is left for the KVM HID.
PREFERRED_UDC=1e6a0000.usb-vhub:p1

# f_rndis calls these dev_addr (host side) and host_addr (device side).
DEV_MAC=02:00:00:00:01:01
HOST_MAC=02:00:00:00:01:02

log()
{
    echo "host-if-gadget: $*"
}

# Remove a stale HID function and any link to it. An older revision of this
# script built the gadget as one composite holding HID + ethernet, and a host
# that rejects that composite never configures the device, so the ethernet
# link stays dead. Converging on every start is what makes that fix stick --
# the old version early-returned whenever the gadget directory existed.
drop_hid()
{
    [ -d "$GADGET" ] || return 0

    # A bound gadget cannot have its configuration edited.
    if [ -n "$(cat "$GADGET/UDC" 2>/dev/null)" ]; then
        echo "" > "$GADGET/UDC"
    fi

    for c in "$GADGET"/configs/*; do
        [ -d "$c" ] || continue
        if [ -L "$c/hid.usb0" ]; then
            rm -f "$c/hid.usb0"
            log "removed stale hid.usb0 link from ${c##*/}"
        fi
    done
    if [ -d "$GADGET/functions/hid.usb0" ]; then
        rmdir "$GADGET/functions/hid.usb0" 2>/dev/null || true
        log "removed stale hid.usb0 function"
    fi
}

create_gadget()
{
    drop_hid

    if [ ! -d "$GADGET" ]; then
        log "creating gadget $GADGET_NAME"
        mkdir -p "$GADGET"
    fi

    cd "$GADGET"

    # AMI's USB IDs, so the host's AMI USB driver matches what it expects:
    #   VENDOR_ID 1131 decimal = 0x046b = American Megatrends Inc.
    #   PRODUCT_ID 0xffb0 = "Composite Hid and Ethernet Device" (eth_mod.c)
    echo 0x046b > idVendor
    echo 0xffb0 > idProduct
    echo 0x0100 > bcdDevice
    echo 0x0200 > bcdUSB

    mkdir -p strings/0x409
    echo "American Megatrends Inc." > strings/0x409/manufacturer
    echo "Virtual Ethernet" > strings/0x409/product
    echo "1234567890" > strings/0x409/serialnumber

    echo 0x02 > bDeviceClass
    echo 0x00 > bDeviceSubClass
    echo 0x00 > bDeviceProtocol

    mkdir -p configs/c.1/strings/0x409
    echo "RNDIS" > configs/c.1/strings/0x409/configuration

    # Ethernet only -- no HID here (see the header).
    mkdir -p functions/rndis.usb0
    echo "$DEV_MAC" > functions/rndis.usb0/dev_addr
    echo "$HOST_MAC" > functions/rndis.usb0/host_addr
    if [ ! -L "configs/c.1/rndis.usb0" ]; then
        ln -s functions/rndis.usb0 configs/c.1/
    fi
}

bind_udc()
{
    cd "$GADGET"
    [ -n "$(cat UDC 2>/dev/null)" ] && return 0

    used="$(cat /sys/kernel/config/usb_gadget/*/UDC 2>/dev/null || true)"
    case "$used" in
    *"$PREFERRED_UDC"*) ;;
    *)
        if [ -e "/sys/class/udc/$PREFERRED_UDC" ]; then
            log "binding to UDC $PREFERRED_UDC"
            echo "$PREFERRED_UDC" > UDC
            return 0
        fi
        ;;
    esac

    # Preferred port taken or absent: fall back to any other free UDC.
    for u in /sys/class/udc/*; do
        name="${u##*/}"
        case "$used" in
        *"$name"*) ;;
        *)
            log "binding to UDC $name"
            echo "$name" > UDC
            return 0
            ;;
        esac
    done

    log "no free UDC"
    return 1
}

ensure_iface()
{
    [ -e "/sys/class/net/$IFACE" ] || return 0

    ip link set "$IFACE" up 2>/dev/null || true
    if ! ip addr show "$IFACE" 2>/dev/null | grep -q "$ADDR"; then
        ip addr add "$ADDR/$PREFIXLEN" dev "$IFACE" 2>/dev/null || true
    fi
}

report_iface()
{
    log "$IFACE up at $ADDR/$PREFIXLEN"
    # From sysfs: this image's busybox 'ip' does not support '-s'.
    for f in carrier operstate; do
        [ -r "/sys/class/net/$IFACE/$f" ] &&
            log "$IFACE $f: $(cat "/sys/class/net/$IFACE/$f" 2>/dev/null)"
    done
    for f in rx_packets rx_bytes tx_packets tx_bytes; do
        [ -r "/sys/class/net/$IFACE/statistics/$f" ] &&
            log "$IFACE $f: $(cat "/sys/class/net/$IFACE/statistics/$f" 2>/dev/null)"
    done
}

if [ ! -d /sys/kernel/config/usb_gadget ] || [ ! -d /sys/class/udc ]; then
    log "no configfs USB gadget support or no UDC; nothing to do"
    exit 0
fi

create_gadget
bind_udc || exit 0

i=0
while [ ! -e "/sys/class/net/$IFACE" ] && [ "$i" -lt 20 ]; do
    sleep 1
    i=$((i + 1))
done
ensure_iface
report_iface

# Keep the interface up and addressed. It can be dropped by a host reboot or a
# USB re-enumeration, and a one-shot setup would never restore it -- which is
# exactly what happened in the first revision of this probe.
while true; do
    sleep "$MAINTAIN_INTERVAL"
    if [ -n "$(cat "$GADGET/UDC" 2>/dev/null)" ]; then
        ensure_iface
    else
        bind_udc 2>/dev/null || true
    fi
done
