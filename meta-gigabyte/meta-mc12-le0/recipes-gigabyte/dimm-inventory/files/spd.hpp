#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dimm
{

// Module type, JEDEC DDR4 SPD byte 3.
enum class ModuleType
{
    RDIMM,
    UDIMM,
    SODIMM,
    LRDIMM,
    MiniRDIMM,
    MiniUDIMM,
    MiniLRDIMM,
    Other,
    Unknown,
};

// Everything we publish is derived from fields that have been checked against
// a known DIMM (Corsair CMK32GX4M2D3600C18, 16 GiB, DDR4-3600).
struct SpdInfo
{
    std::string dimmType;      // "DDR4"
    ModuleType moduleType{ModuleType::Unknown};
    std::string moduleTypeStr; // "UDIMM"

    uint32_t capacityMiB{};
    uint8_t rankCount{};
    uint8_t busWidthBits{};   // module data bus width, e.g. 64
    uint8_t eccWidthBits{};   // 0 for non-ECC UDIMM
    uint8_t deviceWidthBits{}; // per-device width, e.g. 8

    std::string partNumber;
    std::string manufacturer;
    std::string serialNumber;
};

// Decodes a 512-byte DDR4 SPD image.
//
// Returns nullopt unless the image is a DDR4 SPD (byte 0 == 0x23, byte 2 ==
// 0x0C) AND both JEDEC CRC16 fields check out. The CRC check is what makes
// this safe to gate inventory on: a slot with no DIMM, or a device that is
// not really an SPD, will not produce an object.
//
// NOTE: deliberately does not report operating speed. The timing region of
// the SPDs on this board does not decode to a sane JEDEC bin, so exposing
// OperatingSpeedMhz from SPD would publish a wrong number; the configured
// speed is taken from SMBIOS Type 17 instead, see smbios.hpp.
std::optional<SpdInfo> decodeDdr4Spd(const std::vector<uint8_t>& spd);

// CRC-16/XMODEM (poly 0x1021, init 0x0000), as used by JEDEC DDR4 SPD.
uint16_t crc16(const uint8_t* data, size_t len);

} // namespace dimm
