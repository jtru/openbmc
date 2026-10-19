// SPDX-License-Identifier: Apache-2.0
//
// Reads DDR4 SPD EEPROMs (ee1004) on the BMC's I2C buses and publishes each
// populated DIMM as an inventory object, so bmcweb exposes it at
// Redfish /redfish/v1/Systems/system/Memory and webui-vue shows it under
// Hardware status -> Memory.
//
// Interface and enum names below are matched verbatim by bmcweb
// (redfish-core/lib/memory.hpp), so they are written out rather than
// generated from phosphor-dbus-interfaces.

#include "smbios.hpp"
#include "spd.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace
{

constexpr const char* serviceName = "xyz.openbmc_project.DimmInventory";
constexpr const char* inventoryRoot = "/xyz/openbmc_project/inventory";

constexpr const char* itemIface = "xyz.openbmc_project.Inventory.Item";
constexpr const char* dimmIface = "xyz.openbmc_project.Inventory.Item.Dimm";
constexpr const char* assetIface =
    "xyz.openbmc_project.Inventory.Decorator.Asset";
// bmcweb surfaces this as Location.PartLocation.ServiceLabel -- which is where
// a board silkscreen name belongs. Note that MemoryDeviceLocator, which this
// daemon also publishes, is not read by bmcweb at all.
constexpr const char* locationCodeIface =
    "xyz.openbmc_project.Inventory.Decorator.LocationCode";

constexpr const char* enumPrefix =
    "xyz.openbmc_project.Inventory.Item.Dimm.";

constexpr size_t spdBytes = 512;

// smbios-mdr's transport file: a 10-byte MDRSMBIOSHeader followed by the
// SMBIOS entry point and structure table. ami-oem-probe writes it on every
// SMBIOS push from the host, and smbios-mdr's own blob handler writes the same
// file, so this is the single place the host's tables can be read from.
constexpr const char* smbiosTablePath = "/var/lib/smbios/smbios2";

// The host pushes its tables during POST, which normally happens after this
// daemon has started and published what the SPDs alone can tell us. Polling
// the transport file's mtime is enough to notice the push and republish with
// the SMBIOS-only fields filled in; nothing here is latency sensitive.
constexpr std::chrono::seconds smbiosPollInterval{10};

using InterfacePtr = std::shared_ptr<sdbusplus::asio::dbus_interface>;

std::optional<std::string> readSmallFile(const fs::path& path)
{
    std::ifstream in(path.c_str());
    if (!in)
    {
        return std::nullopt;
    }
    std::string value;
    std::getline(in, value);
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
    {
        value.pop_back();
    }
    return value;
}

std::optional<std::vector<uint8_t>> readSpd(const fs::path& path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::vector<uint8_t> buf(spdBytes);
    in.read(reinterpret_cast<char*>(buf.data()),
            static_cast<std::streamsize>(spdBytes));
    if (in.gcount() != static_cast<std::streamsize>(spdBytes))
    {
        return std::nullopt;
    }
    return buf;
}

// D-Bus object paths only allow [A-Za-z0-9_], but kernel device names look
// like "5-0052", so the dash has to go.
std::string sanitizePathElement(const std::string& in)
{
    std::string out = in;
    for (char& c : out)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
        {
            c = '_';
        }
    }
    return out;
}

std::string formFactorEnum(dimm::ModuleType type)
{
    const std::string p = std::string(enumPrefix) + "FormFactor.";
    switch (type)
    {
        case dimm::ModuleType::RDIMM:
            return p + "RDIMM";
        case dimm::ModuleType::UDIMM:
            return p + "UDIMM";
        case dimm::ModuleType::SODIMM:
            return p + "SO_DIMM";
        case dimm::ModuleType::LRDIMM:
            return p + "LRDIMM";
        case dimm::ModuleType::MiniRDIMM:
            return p + "Mini_RDIMM";
        case dimm::ModuleType::MiniUDIMM:
            return p + "Mini_UDIMM";
        default:
            // No matching phosphor-dbus-interfaces value (e.g. Mini-LRDIMM);
            // better to omit the property than to publish a wrong one.
            return {};
    }
}

// SPD byte 13 bits 4:3 is the ECC bus extension width: 0, 8 or 16 bits.
std::string eccEnum(uint8_t eccWidthBits)
{
    const std::string p = std::string(enumPrefix) + "Ecc.";
    if (eccWidthBits == 0)
    {
        return p + "NoECC";
    }
    if (eccWidthBits == 8)
    {
        return p + "SingleBitECC";
    }
    return p + "MultiBitECC";
}

// Publishes one DIMM. `smbiosDev` is the matching SMBIOS Type 17 entry when the
// host has pushed its tables, and nullptr otherwise; it carries the board
// silkscreen name and the trained speed, neither of which is in the SPD.
void publishDimm(sdbusplus::asio::object_server& server,
                 const std::string& deviceName, uint8_t bus, uint8_t address,
                 const dimm::SpdInfo& info,
                 const smbios::MemoryDevice* smbiosDev,
                 std::vector<InterfacePtr>& keepAlive)
{
    // Last path element becomes the Redfish "Id" of the Memory resource.
    const std::string path = std::string(inventoryRoot) +
                             "/system/chassis/motherboard/dimm_" +
                             sanitizePathElement(deviceName);
    // What we can say without SMBIOS: which EEPROM this actually is.
    const std::string i2cLocator = [&] {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "i2c-%u 0x%02x",
                      static_cast<unsigned>(bus),
                      static_cast<unsigned>(address));
        return std::string(buf);
    }();
    // What the board calls it, when the host has told us.
    const std::string smbiosLocator =
        (smbiosDev != nullptr) ? smbiosDev->deviceLocator : std::string{};
    const std::string locator =
        smbiosLocator.empty() ? i2cLocator : smbiosLocator;

    auto item = server.add_interface(path, itemIface);
    // The silkscreen name already reads as a DIMM label, so only the i2c
    // fallback gets the noun in front of it.
    item->register_property("PrettyName", smbiosLocator.empty()
                                              ? "DIMM " + i2cLocator
                                              : smbiosLocator);
    item->register_property("Present", true);
    item->initialize();
    keepAlive.push_back(item);

    auto dimmIfaceObj = server.add_interface(path, dimmIface);
    // bmcweb derives CapacityMiB by shifting this right by 10, so it must be
    // kilobytes. Both this and MemoryAttributes are PDI type 'size', which is
    // C++ size_t -- 32-bit on this arm1176jzs target, so D-Bus signature 'u'.
    // Publishing uint64_t ('t') makes unpackPropertiesNoThrow fail and bmcweb
    // answers every Memory request with Base.1.19.InternalError.
    dimmIfaceObj->register_property(
        "MemorySizeInKB", static_cast<uint32_t>(info.capacityMiB) * 1024);
    dimmIfaceObj->register_property(
        "MemoryDataWidth", static_cast<uint16_t>(info.busWidthBits));
    dimmIfaceObj->register_property(
        "MemoryTotalWidth",
        static_cast<uint16_t>(info.busWidthBits + info.eccWidthBits));
    dimmIfaceObj->register_property(
        "MemoryAttributes", static_cast<uint32_t>(info.rankCount));
    dimmIfaceObj->register_property("MemoryType",
                                    std::string(enumPrefix) + "DeviceType.DDR4");
    dimmIfaceObj->register_property(
        "MemoryMedia", std::string(enumPrefix) + "MemoryTech.DRAM");
    dimmIfaceObj->register_property("MemoryDeviceLocator", locator);
    // The trained speed: the one number the SPD cannot supply, because its
    // timing region does not decode to a sane JEDEC bin (see spd.hpp). Omitted
    // rather than guessed when the host has not pushed its tables yet -- an
    // absent OperatingSpeedMhz is honest, a zero is not.
    if (smbiosDev != nullptr && smbiosDev->configuredSpeedMhz != 0)
    {
        dimmIfaceObj->register_property("MemoryConfiguredSpeedInMhz",
                                        smbiosDev->configuredSpeedMhz);
    }

    const std::string formFactor = formFactorEnum(info.moduleType);
    if (!formFactor.empty())
    {
        dimmIfaceObj->register_property("FormFactor", formFactor);
    }
    dimmIfaceObj->register_property("ECC", eccEnum(info.eccWidthBits));
    dimmIfaceObj->initialize();
    keepAlive.push_back(dimmIfaceObj);

    auto asset = server.add_interface(path, assetIface);
    if (!info.partNumber.empty())
    {
        asset->register_property("PartNumber", info.partNumber);
    }
    if (!info.manufacturer.empty())
    {
        asset->register_property("Manufacturer", info.manufacturer);
    }
    if (!info.serialNumber.empty())
    {
        asset->register_property("SerialNumber", info.serialNumber);
    }
    asset->initialize();
    keepAlive.push_back(asset);

    if (!smbiosLocator.empty())
    {
        auto location = server.add_interface(path, locationCodeIface);
        location->register_property("LocationCode", smbiosLocator);
        location->initialize();
        keepAlive.push_back(location);
    }

    // No Association.Definitions here on purpose. bmcweb's Memory collection
    // finds DIMMs with getSubTree("/xyz/openbmc_project/inventory", depth 0,
    // {Inventory.Item.Dimm}) -- it does not need an association, and adding a
    // "contained_by" pointing at a parent that entity-manager has not actually
    // created would dangle. If you want the DIMMs nested in the webui-vue
    // inventory tree, add one here targeting the Board object entity-manager
    // creates (check `busctl tree xyz.openbmc_project.EntityManager`).

    // rankCount is a uint8_t: stream it as a number, or rank 1 shows up as a
    // raw 0x01 byte and journald stores the whole line as "blob data".
    std::cerr << "dimm-inventory: " << path << " -> " << info.partNumber
              << " (" << info.capacityMiB << " MiB, "
              << static_cast<unsigned>(info.rankCount) << " rank, "
              << info.moduleTypeStr << ")";
    if (smbiosDev != nullptr)
    {
        std::cerr << " [" << smbiosLocator << ", "
                  << smbiosDev->configuredSpeedMhz << " MT/s from SMBIOS]";
    }
    std::cerr << "\n";
}

// Locates the SMBIOS Type 17 entry describing the same module as `info` and
// marks it claimed, so that the next DIMM cannot take it too.
//
// Part number is tried first. It cannot separate the two DIMMs on this board,
// because they are identical, so the fallback matches on capacity alone and
// hands the entries out in the order the BIOS emitted them (bank order, i.e.
// A1 before B1) to the EEPROMs in bus-address order (which is also bank order,
// since the SPD addresses here run by bank rather than by channel). That keeps
// the pairing stable and consistent with how the host's own tools associate
// SPDs with DMI slots.
const smbios::MemoryDevice* matchMemoryDevice(
    const std::vector<smbios::MemoryDevice>& devices,
    std::vector<bool>& claimed, const dimm::SpdInfo& info)
{
    const auto samePart = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size())
        {
            return false;
        }
        return std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::toupper(static_cast<unsigned char>(x)) ==
                   std::toupper(static_cast<unsigned char>(y));
        });
    };

    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (claimed[i] || devices[i].capacityMiB != info.capacityMiB)
        {
            continue;
        }
        if (!info.partNumber.empty() && !devices[i].partNumber.empty() &&
            samePart(info.partNumber, devices[i].partNumber))
        {
            claimed[i] = true;
            return &devices[i];
        }
    }
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (!claimed[i] && devices[i].capacityMiB == info.capacityMiB)
        {
            claimed[i] = true;
            return &devices[i];
        }
    }
    return nullptr;
}

struct SpdDevice
{
    std::string deviceName;
    uint8_t bus{};
    uint8_t address{};
    dimm::SpdInfo info;
};

// Enumerates the SPD EEPROMs and (re)publishes every populated DIMM, borrowing
// the silkscreen name and the trained speed from SMBIOS Type 17 when the host
// has pushed its tables.
void publishAll(sdbusplus::asio::object_server& server,
                std::vector<InterfacePtr>& published)
{
    // Everything previously published is dropped first: properties cannot be
    // added to an interface after initialize(), and the property set differs
    // between "the host has pushed" and "the host has not".
    for (const auto& iface : published)
    {
        server.remove_interface(iface);
    }
    published.clear();

    std::vector<SpdDevice> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/bus/i2c/devices", ec))
    {
        if (!entry.is_directory())
        {
            continue;
        }
        const fs::path dir = entry.path();
        auto name = readSmallFile(dir / "name");
        if (!name || *name != "ee1004")
        {
            continue;
        }

        const std::string deviceName = dir.filename().string();
        const auto dash = deviceName.find('-');
        if (dash == std::string::npos)
        {
            continue;
        }

        auto raw = readSpd(dir / "eeprom");
        if (!raw)
        {
            std::cerr << "dimm-inventory: " << dir << ": cannot read SPD\n";
            continue;
        }

        auto info = dimm::decodeDdr4Spd(*raw);
        if (!info)
        {
            // Unpopulated slot, or not a valid DDR4 SPD (bad CRC). Skipping is
            // the point of the CRC check: no DIMM should not mean a bogus
            // inventory entry.
            std::cerr << "dimm-inventory: " << dir
                      << ": not a valid DDR4 SPD, skipping\n";
            continue;
        }

        found.push_back(SpdDevice{
            deviceName,
            static_cast<uint8_t>(
                std::stoul(deviceName.substr(0, dash), nullptr, 10)),
            static_cast<uint8_t>(
                std::stoul(deviceName.substr(dash + 1), nullptr, 16)),
            std::move(*info)});
    }

    // Directory order is not guaranteed; sorting keeps the SMBIOS pairing
    // deterministic across restarts.
    std::sort(found.begin(), found.end(),
              [](const SpdDevice& a, const SpdDevice& b) {
                  return std::tie(a.bus, a.address) <
                         std::tie(b.bus, b.address);
              });

    const auto devices = smbios::readMemoryDevices(smbiosTablePath);
    std::vector<bool> claimed(devices.size(), false);

    unsigned enriched = 0;
    for (const auto& dev : found)
    {
        const smbios::MemoryDevice* match =
            matchMemoryDevice(devices, claimed, dev.info);
        if (match != nullptr)
        {
            ++enriched;
        }
        publishDimm(server, dev.deviceName, dev.bus, dev.address, dev.info,
                    match, published);
    }

    std::cerr << "dimm-inventory: published " << found.size() << " DIMM(s), "
              << enriched << " matched to " << devices.size()
              << " populated SMBIOS Type 17 entr(ies)\n";
}

} // namespace

int main()
{
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    conn->request_name(serviceName);
    sdbusplus::asio::object_server server(conn);

    // Inventory.Item.interface.yaml requires anything implementing
    // Inventory.Item to provide an ObjectManager on this path.
    server.add_manager(inventoryRoot);

    std::vector<InterfacePtr> published;

    const auto tableStamp = [] {
        std::error_code error;
        const auto time = fs::last_write_time(smbiosTablePath, error);
        if (error)
        {
            return std::string{"absent"};
        }
        return std::to_string(time.time_since_epoch().count());
    };

    // The host pushes its SMBIOS tables during POST, which is normally after
    // this daemon has started -- and a BMC reboot does not imply a host POST.
    // So publish what the SPDs alone can tell us now, then republish whenever
    // the transport file appears or changes.
    std::string lastStamp = tableStamp();
    publishAll(server, published);

    boost::asio::steady_timer timer(io);
    std::function<void()> poll = [&]() {
        timer.expires_after(smbiosPollInterval);
        timer.async_wait([&](const boost::system::error_code& error) {
            if (error)
            {
                return;
            }
            std::string stamp = tableStamp();
            if (stamp != lastStamp)
            {
                std::cerr << "dimm-inventory: host SMBIOS tables changed "
                             "(now "
                          << ((stamp == "absent") ? "absent" : "present")
                          << "), republishing\n";
                lastStamp = stamp;
                publishAll(server, published);
            }
            poll();
        });
    };
    poll();

    io.run();
    return 0;
}
