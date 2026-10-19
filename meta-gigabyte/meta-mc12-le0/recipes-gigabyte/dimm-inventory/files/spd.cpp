#include "spd.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace dimm
{

namespace
{

constexpr size_t spdSize = 512;
constexpr uint8_t spdMagic = 0x23;   // byte 0
constexpr uint8_t ddr4KeyByte = 0x0C; // byte 2

// SDRAM density per die, JEDEC DDR4 SPD byte 4 bits 3:0, in megabits.
// 256Mb .. 32Gb covers every part we are likely to meet.
constexpr std::array<uint32_t, 8> densityMb = {
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
};

// Number of dies per package from byte 5 bits 6:5:
// 0 = not specified, 1 = SDP (1 die), 2 = DDP (2 dies), 3 = QDP (4 dies).
constexpr std::array<uint8_t, 4> diesPerPackage = {1, 1, 2, 4};

std::string trimAscii(const uint8_t* p, size_t len)
{
    std::string s(reinterpret_cast<const char*>(p), len);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' '))
    {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() &&
           (s[start] == '\0' || s[start] == ' '))
    {
        ++start;
    }
    return s.substr(start);
}

std::string toHex(uint32_t v, int width)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%0*x", width, v);
    return buf;
}

// JEDEC JEP106 module manufacturer ID (SPD bytes 320-321: continuation count,
// then the code itself). Only entries we have actually observed on this board
// are listed; anything else is reported as a raw hex ID rather than guessed.
std::string manufacturerName(uint8_t continuation, uint8_t code)
{
    const uint16_t id = static_cast<uint16_t>((continuation << 8) | code);
    switch (id)
    {
        case 0x029E:
            // Verified: Corsair CMK32GX4M2D3600C18 on i2c-5.
            return "Corsair";
        default:
            return toHex(id, 4);
    }
}

ModuleType moduleTypeFromByte(uint8_t b, std::string& str)
{
    switch (b)
    {
        case 0x01:
            str = "RDIMM";
            return ModuleType::RDIMM;
        case 0x02:
            str = "UDIMM";
            return ModuleType::UDIMM;
        case 0x03:
            str = "SO-DIMM";
            return ModuleType::SODIMM;
        case 0x04:
            str = "LRDIMM";
            return ModuleType::LRDIMM;
        case 0x05:
            str = "Mini-RDIMM";
            return ModuleType::MiniRDIMM;
        case 0x06:
            str = "Mini-UDIMM";
            return ModuleType::MiniUDIMM;
        case 0x07:
            str = "Mini-LRDIMM";
            return ModuleType::MiniLRDIMM;
        default:
            str = toHex(b, 2);
            return ModuleType::Other;
    }
}

} // namespace

uint16_t crc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
        {
            if (crc & 0x8000)
            {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            }
            else
            {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

std::optional<SpdInfo> decodeDdr4Spd(const std::vector<uint8_t>& spd)
{
    if (spd.size() < spdSize)
    {
        return std::nullopt;
    }
    if (spd[0] != spdMagic || spd[2] != ddr4KeyByte)
    {
        return std::nullopt;
    }

    // JEDEC DDR4 defines CRC16 over bytes 0-125 (stored at 126) and over
    // bytes 128-253 (stored at 254), both little-endian.
    const uint16_t crcLow = static_cast<uint16_t>(spd[126] | (spd[127] << 8));
    const uint16_t crcHigh = static_cast<uint16_t>(spd[254] | (spd[255] << 8));
    if (crc16(spd.data(), 126) != crcLow ||
        crc16(spd.data() + 128, 126) != crcHigh)
    {
        return std::nullopt;
    }

    SpdInfo info;
    info.dimmType = "DDR4";
    info.moduleType = moduleTypeFromByte(spd[3], info.moduleTypeStr);

    const uint8_t densityIndex = spd[4] & 0x0F;
    if (densityIndex >= densityMb.size())
    {
        return std::nullopt;
    }
    const uint32_t density = densityMb[densityIndex];
    const uint8_t dies = diesPerPackage[(spd[5] >> 5) & 0x03];

    // Encoding: value + 1 ranks (0 => 1 rank).
    info.rankCount = static_cast<uint8_t>(((spd[12] >> 3) & 0x07) + 1);

    // Byte 13: bits 2:0 is the primary bus width as 8 bits << value, but bits
    // 4:3 is a bus width *extension* count (0 = none, 1 = 8 bits, 2 = 16
    // bits), not a shift -- a non-ECC module reads 0 there and must decode to
    // 0 extra bits, not 8.
    info.busWidthBits = static_cast<uint8_t>(8u << (spd[13] & 0x07));
    info.eccWidthBits = static_cast<uint8_t>(8u * ((spd[13] >> 3) & 0x03));
    info.deviceWidthBits = static_cast<uint8_t>(8u << (spd[14] & 0x07));

    if (info.deviceWidthBits == 0 || info.busWidthBits == 0)
    {
        return std::nullopt;
    }
    const uint32_t devicesPerRank = info.busWidthBits / info.deviceWidthBits;
    if (devicesPerRank == 0)
    {
        return std::nullopt;
    }

    // Total module capacity in megabits, then to MiB. The density table above
    // is already binary (16384 == 16 Gb), so this yields the marketed "16 GB".
    const uint64_t totalMb = static_cast<uint64_t>(density) * dies *
                             info.rankCount * devicesPerRank;
    info.capacityMiB = static_cast<uint32_t>(totalMb / 8);

    info.partNumber = trimAscii(spd.data() + 329, 20);
    info.manufacturer = manufacturerName(spd[320], spd[321]);

    const uint32_t serial = static_cast<uint32_t>(spd[325]) |
                            (static_cast<uint32_t>(spd[326]) << 8) |
                            (static_cast<uint32_t>(spd[327]) << 16) |
                            (static_cast<uint32_t>(spd[328]) << 24);
    // Many consumer modules leave the serial unprogrammed; say so rather than
    // publishing "0x00000000" as if it were a real serial.
    info.serialNumber = (serial == 0) ? "" : toHex(serial, 8);

    return info;
}

} // namespace dimm
