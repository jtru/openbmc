# SMBIOS gives us CPU identity, which is otherwise unobtainable on this board:
# the brand string and part number live in SMBIOS Type 4, and there is no
# BMC-side alternative for AMD (smbios-mdr's cpuinfo/cpuinfo-peci options read
# Intel PIROM/PECI, and APML/SB-TSI only expose thermals and power).
#
# smbios-ipmi-blob is the generic "host uploads the table as an IPMI blob"
# transport: its handler writes /var/lib/smbios/smbios2 and tells smbios-mdr to
# re-read it. This board's BIOS does not speak that protocol -- it pushes
# through AMI's NetFn 0x2E shared-window conversation instead, which
# ami-oem-probe receives and republishes through the very same file and D-Bus
# call. The option stays on because it costs nothing and is the correct door
# for a BIOS that does speak blobs (which also explains why DIMM/TPM data has
# to arrive some other way: see below).
#
# DIMM inventory comes from the SPD EEPROMs (dimm-inventory), not from SMBIOS,
# so smbios-no-dimm stays set and smbios-mdr publishes no Dimm objects of its
# own -- otherwise every slot would appear twice in Redfish Memory. Type 17 does
# hold two things the SPDs cannot supply, the trained speed and the board
# silkscreen locator, and dimm-inventory reads exactly those out of
# /var/lib/smbios/smbios2 and merges them in. That keeps the accurate half of
# each source (the SPDs have the real manufacturer, serial and module type; Type
# 17 has the speed and the slot name) without duplicating objects. See
# dimm-inventory's smbios.cpp.
PACKAGECONFIG:append = " \
    smbios-ipmi-blob \
    smbios-no-dimm \
"
