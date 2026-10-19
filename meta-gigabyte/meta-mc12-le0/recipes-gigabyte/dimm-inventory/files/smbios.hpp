#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace smbios
{

// One SMBIOS Type 17 (Memory Device) entry, to the extent this daemon needs
// it. Everything else Type 17 carries (rank, width, ECC, voltages) is already
// decoded from the SPD EEPROM by spd.cpp and is not read here.
//
// The two fields worth coming to SMBIOS for are exactly the two the SPDs on
// this board cannot supply:
//
//   deviceLocator         the board silkscreen name, e.g. "DIMM_P0_A1"
//   configuredSpeedMhz    the speed the system actually trains at, from Type
//                         17 offset 0x20 ("Configured Memory Speed"), e.g.
//                         3600 -- distinct from offset 0x15 ("Speed"), which
//                         on this board reports the 2133 MT/s JEDEC default
//                         the module falls back to.
struct MemoryDevice
{
    std::string deviceLocator;
    std::string bankLocator;
    std::string partNumber;
    uint32_t capacityMiB{};
    uint16_t configuredSpeedMhz{};
};

// Reads the SMBIOS transport file that smbios-mdr keeps (/var/lib/smbios/
// smbios2): a 10-byte MDRSMBIOSHeader followed by an SMBIOS entry point and
// the raw structure table, as written either by ami-oem-probe (this board's
// AMI push) or by smbios-mdr's own IPMI blob handler.
//
// Returns the Type 17 entries in the order the host BIOS emitted them, or an
// empty vector if the file is absent or does not parse -- which is the normal
// state before the host has POSTed, and must therefore not be an error.
std::vector<MemoryDevice> readMemoryDevices(const std::string& path);

} // namespace smbios
