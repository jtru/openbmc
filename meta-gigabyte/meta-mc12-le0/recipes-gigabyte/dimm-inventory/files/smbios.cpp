// SPDX-License-Identifier: Apache-2.0
//
// Minimal SMBIOS Type 17 reader, used only to borrow the two Memory fields the
// SPD EEPROMs cannot provide. See smbios.hpp.

#include "smbios.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace smbios
{

namespace
{

// MDRSMBIOSHeader is packed: dirVer(u8), mdrType(u8), timestamp(u32),
// dataSize(u32).
constexpr size_t mdrHeaderSize = 10;
constexpr size_t mdrDataSizeOffset = 6;

constexpr uint8_t typeMemoryDevice = 17;
constexpr uint8_t typeEndOfTable = 0x7f;

// String table entries are conventionally space padded; the brand string on
// this board arrives with a long run of trailing spaces.
void trimTrailing(std::string& value)
{
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\0'))
    {
        value.pop_back();
    }
}

uint16_t rd16(const std::vector<uint8_t>& b, size_t off)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(b[off]) |
                                 static_cast<uint16_t>(b[off + 1]) << 8);
}

uint32_t rd32(const std::vector<uint8_t>& b, size_t off)
{
    return static_cast<uint32_t>(b[off]) |
           static_cast<uint32_t>(b[off + 1]) << 8 |
           static_cast<uint32_t>(b[off + 2]) << 16 |
           static_cast<uint32_t>(b[off + 3]) << 24;
}

uint64_t rd64(const std::vector<uint8_t>& b, size_t off)
{
    return static_cast<uint64_t>(rd32(b, off)) |
           static_cast<uint64_t>(rd32(b, off + 4)) << 32;
}

// SMBIOS string fields are 1-based indexes into the structure's string set.
// Zero, or an index past the end, means "no string".
std::string stringAt(const std::vector<std::string>& strings, uint8_t index)
{
    if (index == 0 || index > strings.size())
    {
        return {};
    }
    std::string value = strings[index - 1];
    trimTrailing(value);
    return value;
}

// Locate the structure table within the SMBIOS blob.
//
// Field offsets differ between the two entry points and are easy to get wrong:
// the 64-bit "_SM3_" form carries its entry point length at 0x06 and its
// structure table address at 0x10, while the legacy "_SM_" form uses 0x05 and
// 0x18. The address is a hardware address in the specification, but both
// writers that feed this file (ami-oem-probe and smbios-mdr's IPMI blob
// handler) store an offset into the same blob instead, which is what
// smbios-mdr's own smbiosSkipEntryPoint() accepts. Anything out of range is
// therefore treated as the table simply following the entry point.
bool findStructureTable(const std::vector<uint8_t>& body, size_t& tableOff)
{
    // The entry point always sits at the very start of the payload.
    constexpr size_t scanLimit = 0x20;

    for (size_t i = 0; i + 0x18 <= body.size() && i < scanLimit; ++i)
    {
        const bool ep30 = std::memcmp(&body[i], "_SM3_", 5) == 0;
        const bool ep21 = std::memcmp(&body[i], "_SM_", 4) == 0;
        if (!ep30 && !ep21)
        {
            continue;
        }

        const size_t lenOff = ep30 ? 6 : 5;
        const size_t epLen = body[i + lenOff];
        if (epLen < 0x10 || i + epLen > body.size())
        {
            continue;
        }

        // The anchor is four or five ordinary ASCII characters, so it can
        // occur as text; the entry point checksum is what makes a match
        // trustworthy. It is chosen so the entry point sums to zero.
        uint8_t sum = 0;
        for (size_t b = i; b < i + epLen; ++b)
        {
            sum = static_cast<uint8_t>(sum + body[b]);
        }
        if (sum != 0)
        {
            continue;
        }

        size_t addr = 0;
        if (ep30)
        {
            addr = static_cast<size_t>(rd64(body, i + 0x10));
        }
        else if (i + 0x1c <= body.size())
        {
            addr = rd32(body, i + 0x18);
        }

        tableOff = (addr >= i + epLen && addr < body.size()) ? addr
                                                             : i + epLen;
        return true;
    }
    return false;
}

} // namespace

std::vector<MemoryDevice> readMemoryDevices(const std::string& path)
{
    std::vector<MemoryDevice> result;

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in)
    {
        return result;
    }
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    if (file.size() <= mdrHeaderSize + 8)
    {
        return result;
    }

    const uint32_t dataSize = rd32(file, mdrDataSizeOffset);
    const size_t bodyEnd = std::min(file.size(), mdrHeaderSize + dataSize);
    if (bodyEnd <= mdrHeaderSize + 8)
    {
        return result;
    }
    const std::vector<uint8_t> body(file.begin() + mdrHeaderSize,
                                    file.begin() + bodyEnd);

    size_t off = 0;
    if (!findStructureTable(body, off))
    {
        return result;
    }

    while (off + 4 <= body.size())
    {
        const uint8_t type = body[off];
        const uint8_t len = body[off + 1];
        if (type == typeEndOfTable || len < 4)
        {
            break;
        }

        // The fixed part is followed by the string set: NUL separated strings
        // closed by one extra NUL, so the set ends at the first double NUL and
        // the next structure begins right after it. This is not the same as
        // walking strings until a double NUL is *reached*: each string carries
        // its own lone terminator, and a structure with no strings at all is
        // simply two NULs with nothing between them.
        const size_t strStart = off + len;
        size_t strEnd = strStart;
        while (strEnd + 1 < body.size() &&
               !(body[strEnd] == 0 && body[strEnd + 1] == 0))
        {
            ++strEnd;
        }
        if (strEnd + 1 >= body.size())
        {
            break; // no set terminator: truncated structure
        }

        if (type == typeMemoryDevice && len >= 0x22 && off + len <= body.size())
        {
            std::vector<std::string> strings;
            for (size_t s = strStart; s < strEnd;)
            {
                size_t e = s;
                while (e < strEnd && body[e] != 0)
                {
                    ++e;
                }
                strings.emplace_back(body.begin() + s, body.begin() + e);
                s = e + 1;
            }

            const uint16_t size = rd16(body, off + 0x0c);
            if (size != 0)
            {
                uint32_t capacityMiB = size;
                if (size == 0xffff)
                {
                    // Extended size, in MiB.
                    capacityMiB = rd32(body, off + 0x1c) & 0x7fffffffU;
                }
                else if ((size & 0x8000) != 0)
                {
                    capacityMiB = (size & 0x7fffU) / 1024U;
                }

                MemoryDevice dev;
                dev.deviceLocator = stringAt(strings, body[off + 0x10]);
                dev.bankLocator = stringAt(strings, body[off + 0x11]);
                dev.partNumber = stringAt(strings, body[off + 0x1a]);
                dev.capacityMiB = capacityMiB;
                dev.configuredSpeedMhz = rd16(body, off + 0x20);
                result.push_back(std::move(dev));
            }
        }

        off = strEnd + 2;
    }

    return result;
}

} // namespace smbios
