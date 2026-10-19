// SPDX-License-Identifier: Apache-2.0
//
// AMI in-band OEM IPMI provider for the Gigabyte MC12-LE0.
//
// Why this exists
// ---------------
// The host BIOS never hands its SMBIOS tables to this BMC over Intel's
// MDR v2, so smbios-mdr receives nothing and Redfish reports no CPU. What
// the BIOS does instead is hold an AMI-proprietary conversation with the
// BMC. Two channels are involved:
//
//   NetFn 0x2E (OEM) with a 3-byte IANA -- the "inventory/host-interface"
//   data channel. Reverse-engineering the stock MegaRAC libs shows the first
//   payload byte selects an operation:
//
//     0x0A            GetShmAddr: hands the BIOS the DRAM window its bulk
//                     transfer must target. SendIpmiSystemInfo reads the four
//                     bytes at wire offset 3 and DMAs its blob there, so this
//                     one has to be answered with the address, not echoed.
//     0x0B / 0x13     SMBIOS transfer: the tables are already in the window;
//                     body[1..2] is the length. This is what feeds the
//                     inventory -- see publishSmbiosToInventory.
//     0x24            SetWriteProtect: opens/closes the window (SCU writes).
//     0x52            post a Redfish request (e.g. "Systems/Self/Bios")
//     0x0C            a generic slot handler
//
//   NetFn 0x32 -- the control channel. Two of its commands are recovered
//   from the stock firmware and answered for real:
//
//     0x3D  AMIGetBMCInterfaceStatus -- "is the Redfish host interface ready?"
//           req {01 00 idx} -> rsp {07 07 07}. This is the gate: an empty
//           success reads as "not ready", CmdInvalidCommand as "unsupported",
//           and in both cases the BIOS skips the Redfish session entirely.
//     0x5D  AMIGeneratePassword -- hands the BIOS credentials for that
//           session: rsp {ulen, plen, user, pass}. Modes 4/8 acknowledge a
//           session teardown.
//
//   Every other NetFn 0x32 command is answered CmdInvalidCommand -- the same
//   completion code the BIOS already gets when nothing is registered.
//
// On the stock image the bulk data itself does NOT travel in IPMI payloads:
// it lives in a shared-memory window (libgbt.so / libgbtmisc.so:
// shm_handle_request, SmbiosInterfaceSaveToFile) that the BMC maps with
// /dev/mem and that the host writes through an AST window. This provider runs
// the same SCU handshake stock does (see endisShareMemory) and reads that
// window out of the reserved carve through /dev/aspeed-lpc-ctrl, which the
// DTS points at it -- not through /dev/mem, so STRICT_DEVMEM stays on.
//
// What this provider does
// -----------------------
// This is now the port's implementation of that protocol, not just an
// observer of it:
//
//   * It claims the two IANAs the host uses on NetFn 0x2E (exactly those two,
//     not NetFn 0x2E as a whole -- that is ipmid's generic OEM netfn) and
//     answers them in the shape stock does: the window address for op 0x0A,
//     the 5-byte acknowledgement for op 0x0B, and an echo (the convention
//     every Gigabyte netfn-0x2E handler follows) for everything else.
//   * On the op-0x0B announce it republishes the pushed tables to smbios-mdr,
//     which is what puts BIOS version, CPU and PCIe slots into
//     /xyz/openbmc_project/inventory and, from there, into Redfish. See
//     publishSmbiosToInventory for why the tables have to be re-emitted
//     before smbios-mdr will parse them.
//   * It claims NetFn 0x32 so its commands can be logged too, and answers
//     them CmdInvalidCommand -- the same completion code the BIOS already
//     gets when nothing is registered. Answering CmdSuccess instead makes
//     POST take minutes longer for no benefit (see handleNetfnOemTwoCommand).
//   * Every request, on either channel, is written to /tmp/ami-probe.log with
//     its full body. journald on the AST2500 is slow and lossy under load,
//     and an unbounded grep of it times out.
//
// Do not remove this provider: with stock AMI firmware absent, it is the only
// thing that hands the host's SMBIOS tables to smbios-mdr on this board. The
// recipe, the .so and the /tmp capture paths keep their bring-up "probe"
// names.

#include <ipmid/api.hpp>
#include <ipmid/api-types.hpp>
#include <ipmid/filter.hpp>
#include <ipmid/handler.hpp>
#include <ipmid/message.hpp>
#include <ipmid/message/types.hpp>

#include <phosphor-logging/lg2.hpp>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{

// The two OEM IANAs the MC12-LE0's BIOS addresses us with on NetFn 0x2E. The
// framework packs the 3 wire bytes little-endian (oem::toOemNumber in
// ipmid/oemrouter.hpp), so:
//
//   host sends 0A 3C 00  ->  0x003C0A   every command except 0xCC
//   host sends 5E 2B 00  ->  0x002B5E   the 0xCC entries
constexpr ipmi::Iana ianaInventory = 0x003C0A;
constexpr ipmi::Iana ianaSecondary = 0x002B5E;

// Command numbers observed from the host BIOS on NetFn 0x2E.
constexpr uint8_t cmdAmiAllocateMemory = 0x20;
constexpr uint8_t cmdAmiFreeMemory = 0x21;
constexpr uint8_t cmdAmiReadMemory = 0x30;
constexpr uint8_t cmdAmiCopyMemory = 0x32;
constexpr uint8_t cmdAmiCompareMemory = 0x33;
constexpr uint8_t cmdAmiAddExtendSelEntries = 0xCC;

// Command numbers observed from the host BIOS on NetFn 0x32. These were
// recovered from the stock libraries:
//
//   0x3D  AMIGetBMCInterfaceStatus, libipmimsghndlr.so:0x34944
//         (g_AMI_CmdHndlr entry 80). Validates req {01 00 idx<=7} and writes
//         the CC plus three status bytes, all 0x07 -- i.e. every host
//         interface is up.
//   0x5D  AMIGeneratePassword, libipmiamioemrestiface.so:0x3564
//         (g_RESTInterface_CmdHndlr entry {0x5D, 0x04, AMIGeneratePassword}).
//         Request is a single mode byte. Modes 1/2 provision the firmware /
//         kernel host-interface account and answer
//         {CC, ulen, plen, username, password}; modes 4/8 tear a session down
//         and answer the CC alone.
constexpr uint8_t cmd32Unnamed3D = 0x3D;
constexpr uint8_t cmd32HostInterface = 0x5D;
constexpr uint8_t cmd32Query = 0xAB;

// 0x3D: the stock handler answers rsp[1..3] = 07 07 07 for any valid request.
constexpr uint8_t ifaceStatusReady = 0x07;
constexpr size_t ifaceStatusBytes = 3;

// 0x5D: the credentials handed back to the BIOS. The stock firmware generates
// a random password and provisions an AMI redis account for it; this probe
// returns an account that already exists on this BMC instead, so the BIOS's
// HTTPS login works with no account plumbing. This is the bench root account
// -- replace it with a dedicated, least-privilege account before this leaves
// the bench, and do not treat the probe as a finished implementation.
constexpr std::string_view hostIfUser = "root";
constexpr std::string_view hostIfPassword = "0penBmc";

// Bytes of IANA the framework consumes before dispatching to us.
constexpr size_t ianaBytes = 3;

// A memory-write pass would carry a whole table, and neither journald nor a
// single log line wants that. Everything seen on these channels so far is
// well under this.
constexpr size_t maxLoggedBody = 192;

// --- how to answer, from Gigabyte's own handlers ---------------------------
//
// Gigabyte's netfn-0x2E handlers live in libgbt.so and were decompiled with
// Ghidra. Every one has the same shape -- validate the exact request length,
// then copy request fields into FIXED response offsets:
//
//     handler_2e01(req, len, rsp):  len==3     -> rsp[0..2] = req[0..2]
//     handler_2e06:                 len==1     -> rsp[0]    = req[0]
//     handler_2e13:                 len==5     -> rsp[0..4] = req[0..4]
//     handler_2e14:                 len==0x12  -> rsp[0..1] = req[0..1], then 16 bytes
//     handler_2ec8:                 len==0x11  -> memcpy(rsp + 0x16, req, 2)
//
// Two things follow, and they are why the 0x2E responder looks the way it
// does:
//
//   1. There is NO status byte, NO length field and NO checksum in the
//      handler. The framework prepends the 3-byte IANA to whatever the handler
//      wrote. So responseSuccess(bytes) is already the correct envelope -- an
//      earlier revision invented a status/length/checksum block, which was
//      fabricating structure the protocol does not have.
//   2. The handlers ECHO request fields. So echoing the request is far better
//      grounded than inventing a handle or an envelope.
//
// Errors, for reference: handlers return 199 on a length mismatch, and
// check_set_iana returns 0 on an IANA match / 0xCC on a mismatch.
constexpr size_t maxEchoBytes = 64;

// COMPARE_MEMORY answer: 0x01 means "not equal". Returning success-with-no-data
// risks the BIOS concluding the BMC already holds the dataset and skipping the
// push we are trying to observe. Flip this to an empty reply if the exchange
// turns out to stall *at* the compare instead of after it.
constexpr uint8_t compareNotEqual = 0x01;

// AMI's name for this command *number*, taken from its netfn-0x32 constant
// table. These numbers arrive on netfn 0x2E under Gigabyte's IANA (0x003C0A),
// and identical numbers mean different things on different netfns -- the two
// AMIYAFU* handlers proved that. So these are labels of convenience only, and
// the trailing '?' is deliberate: do not read them as established.
constexpr const char* commandName(uint8_t cmd)
{
    switch (cmd)
    {
        case cmdAmiAllocateMemory:
            return "ALLOCATE_MEMORY?";
        case cmdAmiFreeMemory:
            return "FREE_MEMORY?";
        case cmdAmiReadMemory:
            return "READ_MEMORY?";
        case cmdAmiCopyMemory:
            return "COPY_MEMORY?";
        case cmdAmiCompareMemory:
            return "COMPARE_MEMORY?";
        case cmdAmiAddExtendSelEntries:
            return "ADD_EXTEND_SEL_ENTRIES?";
        default:
            return "(unmapped)";
    }
}

// Labels for the NetFn 0x32 commands. 0x3D and 0x5D are now identified from
// the stock dispatcher tables; 0xAB is still unlabelled.
constexpr const char* commandName32(uint8_t cmd)
{
    switch (cmd)
    {
        case cmd32Unnamed3D:
            return "GET_BMC_INTERFACE_STATUS";
        case cmd32HostInterface:
            return "GENERATE_PASSWORD";
        case cmd32Query:
            return "NETFN32_QUERY_AB?";
        default:
            return "(unmapped)";
    }
}

std::string hexDump(const std::vector<uint8_t>& data)
{
    const size_t shown = std::min(data.size(), maxLoggedBody);
    std::string out;
    out.reserve(shown * 3 + 16);

    char tmp[4];
    for (size_t i = 0; i < shown; i++)
    {
        std::snprintf(tmp, sizeof(tmp), "%02X ",
                      static_cast<unsigned>(data[i]));
        out += tmp;
    }
    if (shown < data.size())
    {
        out += "... (";
        out += std::to_string(data.size() - shown);
        out += " more)";
    }
    return out;
}

// Fixed-width hex for the capture lines. std::to_string() is decimal, and an
// earlier revision of this file printed it after a literal "0x" -- which made
// cmd 0x3D read as "cmd=0x61" and every line look like a different protocol.
// Keep the capture in the same base as the lg2::hex journal fields.
std::string toHex(uint32_t value, int width)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*X", width, value);
    return buf;
}

// --- durable capture -------------------------------------------------------
//
// journald is the wrong tool for this on an AST2500: the exchange is bursty,
// the daemon is slow, and a full-journal grep times out. Append every request
// to a plain file as well, with a timestamp, so a capture is just a `cat`.
// Opened per write on purpose -- the provider has no lifecycle of its own and
// must not hold an fd across a daemon restart.
constexpr const char* captureLogPath = "/tmp/ami-probe.log";

std::string timestamp()
{
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
        return "?";
    }

    std::tm tm{};
    localtime_r(&ts.tv_sec, &tm);

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);

    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03ld", buf, ts.tv_nsec / 1000000);
    return out;
}

void appendCapture(const std::string& line)
{
    int fd = ::open(captureLogPath,
                    O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        return;
    }

    std::string out = line;
    out += '\n';
    // Best effort: a failed capture write must never affect the IPMI reply.
    ssize_t ignored = ::write(fd, out.data(), out.size());
    (void)ignored;
    ::close(fd);
}

// Both SMBIOS entry-point anchors start with the same three bytes, so this
// catches the 32-bit '_SM_' and 64-bit '_SM3_' forms alike. Seeing one here
// means the tables we are after just moved through this command.
bool containsSmbiosAnchor(const std::vector<uint8_t>& body)
{
    static constexpr std::array<uint8_t, 3> anchor = {'_', 'S', 'M'};
    return std::search(body.begin(), body.end(), anchor.begin(),
                       anchor.end()) != body.end();
}

// --- host-push window capture -----------------------------------------------
//
// See the call site in handleAmiOemCommand() for the protocol background.
// captureHostWindow() maps /dev/aspeed-lpc-ctrl (which the DTS points at the
// reserved 16 MiB host-push window at 0x9e000000) and copies the bytes the
// BIOS stored there to /tmp, so the data survives the boot even though the
// stock flow would clear the window once the BMC had parsed it.
//
// Never fatal: if the map fails the reply still goes out.
//
// The window is a live device that the host may keep writing while we read;
// the driver maps it with pgprot_noncached, so every read is honest, and we
// accept that the snapshot may miss a byte if the BIOS writes mid-read.
constexpr const char* hostWindowDev = "/dev/aspeed-lpc-ctrl";
constexpr size_t hostWindowBytes = 16 * 1024 * 1024;
constexpr size_t maxWindowCapture = 1024 * 1024; // stock clamps at 1 MiB too
constexpr const char* smbiosWindowPath = "/tmp/ami-smbios.bin";
constexpr const char* redfishWindowPath = "/tmp/ami-redfish.bin";

// The BMC-side window base we hand back in the stock GetShmAddr reply. It is
// the 16 MiB immediately below the host's VGA framebuffer (0x9f000000), which
// is exactly what stock's mem=480M command-line derivation yields: stock
// parses "mem=" out of /proc/cmdline and returns (mem << 20) + 0x80000000.
// Handing back the framebuffer address instead makes the VGA engine overwrite
// every SMBIOS push with screen (or blank-screen zero) data.
constexpr uint32_t shmWindowPhys = 0x9E000000u;

// Shared-window opcodes, the first payload byte after the IANA.
constexpr uint8_t opSmbiosTransfer = 0x0B;
constexpr uint8_t opRedfishPost = 0x52;
// Stock _SetWriteProtect_: body[1] is the mode (0 = enable the share window,
// 1 = disable it). See endisShareMemory().
constexpr uint8_t opSetWriteProtect = 0x24;
// Stock _GetShmAddr_ is reached two ways. The netfn-0x2E op table in
// libgbt.so (file offset 0x82ed4) maps wire op 0x0A on cmd 0x21 (the "get"
// half) straight at it, and the separate subcmd_set_tbl maps the op-0x50
// family at it too. SendIpmiSystemInfo is the caller that matters: it sends
// op 0x0A with no arguments and then copies the four bytes at response
// offset 3 into the SMBIOS push's DRAM destination register, so a probe that
// echoes 0x0A back leaves the host DMA-ing its blob to a garbage address.
constexpr uint8_t opShmAddrQuery = 0x0A;
constexpr uint8_t opGetShmAddr = 0x50;

// One mapping for the whole process; every capture re-reads it defensively.
// pgprot_noncached in the driver keeps every read honest.
struct HostWindowMap
{
    int fd{-1};
    uint8_t* base{static_cast<uint8_t*>(MAP_FAILED)};
};

HostWindowMap& hostWindowMap()
{
    static HostWindowMap m;
    if (m.fd < 0)
    {
        m.fd = ::open(hostWindowDev, O_RDONLY | O_CLOEXEC);
        if (m.fd < 0)
        {
            return m;
        }
        m.base = static_cast<uint8_t*>(
            ::mmap(nullptr, hostWindowBytes, PROT_READ, MAP_SHARED, m.fd, 0));
        if (m.base == MAP_FAILED)
        {
            ::close(m.fd);
            m.fd = -1;
        }
    }
    return m;
}

// Scan a captured window for the SMBIOS entry-point anchors and report how
// much of it is non-zero, so a single log line answers "did the host write
// anything, and where do the tables start?" without needing a helper tool
// pushed back on after every reboot.
void scanCapturedWindow(const uint8_t* data, size_t len,
                        const std::string& tag = {})
{
    len = std::min(std::max<size_t>(len, 0x10000), maxWindowCapture);
    size_t nonZero = 0;
    for (size_t i = 0; i < len; ++i)
    {
        if (data[i] != 0)
        {
            ++nonZero;
        }
    }

    std::string hits;
    for (const char* anchor : {"_SM3_", "_SM_", "_DMI_"})
    {
        const size_t n = std::strlen(anchor);
        for (size_t i = 0; i + n <= len; ++i)
        {
            if (std::memcmp(data + i, anchor, n) == 0)
            {
                if (!hits.empty())
                {
                    hits += ',';
                }
                hits += std::string(anchor) + "@0x" + toHex(i, 6);
            }
        }
    }
    appendCapture(timestamp() + " WINDOW-SCAN " +
                  (tag.empty() ? std::string() : tag + " ") + "len=0x" +
                  toHex(len, 6) + " nonZero=" + std::to_string(nonZero) +
                  " anchors=" + (hits.empty() ? std::string("none") : hits));
}

// Snapshot of the window's first 64 KiB, taken at every netfn-0x2E op. If the
// host's push were to arrive a moment after the announcing op (posted PCIe
// writes, or a BIOS that writes second), the lines around op 0x0B would show
// nonZero/anchors going from 0 to something; if they never do, the write is
// not reaching this window at all.
void logWindowState(const std::string& where)
{
    HostWindowMap& m = hostWindowMap();
    if (m.base == MAP_FAILED)
    {
        return;
    }
    scanCapturedWindow(m.base, 0x10000, where);
}

void captureHostWindow(size_t len, const char* path, uint8_t op)
{
    if (len == 0 || len > hostWindowBytes)
    {
        return;
    }
    HostWindowMap& m = hostWindowMap();
    if (m.base == MAP_FAILED)
    {
        appendCapture(timestamp() + " WINDOW-MAP-FAIL op=0x" + toHex(op, 2) +
                      " len=" + std::to_string(len));
        return;
    }

    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        return;
    }
    ssize_t written = ::write(fd, m.base, len);
    (void)::fsync(fd);
    ::close(fd);

    scanCapturedWindow(m.base, len);

    appendCapture(timestamp() + " WINDOW-CAPTURE op=0x" + toHex(op, 2) +
                  " len=" + std::to_string(written) + " -> " + path +
                  " first8=[" + hexDump(std::vector<uint8_t>(m.base,
                                                              m.base + 8)) +
                  "]");
    lg2::info("AMI OEM PROBE: window capture op={OP} bytes={LEN} to {PATH}",
              "OP", lg2::hex, op, "LEN", written, "PATH", path);
}

// --- OpenBMC inventory bridge ------------------------------------------------
//
// The bytes the host pushes are a plain SMBIOS table set: a 32-bit '_SM_'
// entry point followed by the structure table. That is not the shape
// smbios-mdr parses. Its smbiosSkipEntryPoint() only steps over the 64-bit
// '_SM3_' entry point, and checkSMBIOSVersion() insists on finding one of the
// two anchors first, so an '_SM_' table gets walked as though the entry point
// were a structure and yields nothing.
//
// So re-emit the structure table behind a synthetic '_SM3_' entry point whose
// structure-table address is that table's own offset (the parser treats any
// value below its 32 KiB storage bound as an offset into the buffer), then
// hand the result to smbios-mdr through the door its own IPMI blob handler
// (the smbiosstore module) uses: write /var/lib/smbios/smbios2 as
// [MDRSMBIOSHeader][data] and call AgentSynchronizeData on
// xyz.openbmc_project.Smbios.MDR_V2. That service re-parses the table and
// republishes /xyz/openbmc_project/inventory (BIOS version, CPU, PCIe slots,
// ...).
//
// Never fatal: the IPMI reply the BIOS is waiting for must not depend on the
// inventory daemon.
constexpr const char* smbiosTableDir = "/var/lib/smbios";
constexpr const char* smbiosTablePath = "/var/lib/smbios/smbios2";
constexpr const char* mdrV2Service = "xyz.openbmc_project.Smbios.MDR_V2";
constexpr const char* mdrV2Interface = "xyz.openbmc_project.Smbios.MDR_V2";
constexpr const char* mdrV2ObjectPath = "/xyz/openbmc_project/Smbios/MDR_V2";
constexpr uint8_t mdrDirVersion = 1; // smbios_mdrv2.hpp mdrDirVersion
constexpr uint8_t mdrTypeII = 2;     // smbios_mdrv2.hpp mdrTypeII
constexpr size_t smbiosTableStorageSize = 64 * 1024;
constexpr size_t smbiosEntryPoint30Len = 0x18;
// AgentSynchronizeData makes smbios-mdr re-parse the table and rebuild the
// inventory. Give it a few seconds, but never let a wedged daemon hold the
// ipmid request path open.
constexpr uint64_t mdrSyncTimeoutUs = 5'000'000;

uint16_t rdLe16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rdLe32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t rdLe64(const uint8_t* p)
{
    return static_cast<uint64_t>(rdLe32(p)) |
           (static_cast<uint64_t>(rdLe32(p + 4)) << 32);
}

void appendLe32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

// Hand the pushed SMBIOS table set to smbios-mdr. Returns quietly on anything
// it does not recognise; the call site does not care either way.
void publishSmbiosToInventory(const uint8_t* data, size_t len, uint8_t op)
{
    if (data == nullptr || len < smbiosEntryPoint30Len)
    {
        return;
    }

    // Check '_SM3_' first: it is never a substring of the 32-bit anchor.
    size_t ep = std::string::npos;
    bool epIs64 = false;
    for (size_t i = 0; i + 5 <= len; ++i)
    {
        if (std::memcmp(data + i, "_SM3_", 5) == 0)
        {
            ep = i;
            epIs64 = true;
            break;
        }
    }
    if (ep == std::string::npos)
    {
        for (size_t i = 0; i + 4 <= len; ++i)
        {
            if (std::memcmp(data + i, "_SM_", 4) == 0)
            {
                ep = i;
                break;
            }
        }
    }
    if (ep == std::string::npos)
    {
        return;
    }

    size_t tableStart = 0;
    size_t tableLen = 0;
    uint8_t major = 3;
    uint8_t minor = 3;
    if (epIs64)
    {
        // 64-bit entry point: structure table address at 0x10, version at 7/8.
        tableStart = ep + static_cast<size_t>(rdLe64(data + ep + 0x10));
        tableLen = len > tableStart ? len - tableStart : 0;
        major = data[ep + 7];
        minor = data[ep + 8];
    }
    else
    {
        // 32-bit entry point: version at 6/7, entry point length at 5, and the
        // structure table length at 0x16. The table follows the entry point.
        const uint8_t epLen = data[ep + 5];
        tableStart = ep + (epLen >= 0x1f && epLen <= 0x20 ? epLen : 0x1f);
        tableLen = rdLe16(data + ep + 0x16);
        major = data[ep + 6];
        minor = data[ep + 7];
    }
    if (tableStart >= len || tableLen == 0)
    {
        return;
    }
    tableLen = std::min(tableLen, len - tableStart);
    tableLen =
        std::min(tableLen, smbiosTableStorageSize - smbiosEntryPoint30Len);
    if (major < 3)
    {
        // supportedSMBIOSVersions in smbios_mdrv2.hpp covers 3.0-3.8 only, but
        // the structure table is version-independent for the types it reads.
        major = 3;
        minor = 3;
    }

    // Synthetic 64-bit entry point, so smbiosSkipEntryPoint() steps over it.
    std::vector<uint8_t> out(smbiosEntryPoint30Len, 0x00);
    std::memcpy(out.data(), "_SM3_", 5);
    out[6] = static_cast<uint8_t>(smbiosEntryPoint30Len); // entry point length
    out[7] = major;
    out[8] = minor;
    out[9] = 0x01;  // doc revision
    out[10] = 0x01; // entry point revision
    constexpr uint32_t maxTableSize = 0x80000000u;
    constexpr uint64_t tableAddr = smbiosEntryPoint30Len;
    for (int i = 0; i < 4; ++i)
    {
        out[0x0c + i] = static_cast<uint8_t>(maxTableSize >> (8 * i));
    }
    for (int i = 0; i < 8; ++i)
    {
        out[0x10 + i] = static_cast<uint8_t>(tableAddr >> (8 * i));
    }
    out.insert(out.end(), data + tableStart, data + tableStart + tableLen);

    // The entry point's own 24 bytes must sum to zero mod 256.
    uint8_t sum = 0;
    for (size_t i = 0; i < smbiosEntryPoint30Len; ++i)
    {
        sum = static_cast<uint8_t>(sum + out[i]);
    }
    out[5] = static_cast<uint8_t>(0 - sum);

    if (::access(smbiosTableDir, F_OK) != 0)
    {
        (void)::mkdir(smbiosTableDir, 0755);
    }

    std::vector<uint8_t> file;
    file.reserve(sizeof(uint32_t) * 2 + sizeof(mdrDirVersion) +
                 sizeof(mdrTypeII) + out.size());
    file.push_back(mdrDirVersion);
    file.push_back(mdrTypeII);
    appendLe32(file, static_cast<uint32_t>(std::time(nullptr)));
    appendLe32(file, static_cast<uint32_t>(out.size()));
    file.insert(file.end(), out.begin(), out.end());

    bool wrote = false;
    const int fd = ::open(smbiosTablePath,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0)
    {
        const ssize_t n = ::write(fd, file.data(), file.size());
        (void)::fsync(fd);
        ::close(fd);
        wrote = (n == static_cast<ssize_t>(file.size()));
    }

    bool synced = false;
    if (wrote)
    {
        try
        {
            sdbusplus::bus_t bus(ipmid_get_sd_bus_connection());
            auto method = bus.new_method_call(mdrV2Service, mdrV2ObjectPath,
                                              mdrV2Interface,
                                              "AgentSynchronizeData");
            auto reply = bus.call(method, mdrSyncTimeoutUs);
            reply.read(synced);
        }
        catch (const sdbusplus::exception_t& e)
        {
            lg2::error("AMI OEM PROBE: AgentSynchronizeData failed: {ERR}",
                       "ERR", e.what());
        }
    }

    appendCapture(timestamp() + " SMBIOS-BRIDGE op=0x" + toHex(op, 2) +
                  " ep=0x" + toHex(static_cast<uint32_t>(ep), 6) + " table=" +
                  std::to_string(tableLen) + " data=" +
                  std::to_string(out.size()) + " wrote=" +
                  (wrote ? "1" : "0") + " synced=" + (synced ? "1" : "0"));
    lg2::info("AMI OEM PROBE: inventory bridge op={OP} ep={EP} table={TABLE} "
              "bytes={N} wrote={WROTE} synced={SYNC}",
              "OP", lg2::hex, static_cast<uint32_t>(op), "EP", lg2::hex,
              static_cast<uint32_t>(ep), "TABLE", tableLen, "N", out.size(),
              "WROTE", wrote, "SYNC", synced);
}

// --- host write-path enable (stock endis_share_memory) ----------------------
//
// The BIOS reaches BMC DRAM through the platform's PCIe/LPC write path, whose
// gate the stock firmware toggles in endis_share_memory(). Every stock caller
// that matters -- _GetShmAddr_ (op 0x50), _SetWriteProtect_ (op 0x24),
// _SetShmSmbiosReady_ -- invokes it, and the reconstructed sequence is:
//
//   open /dev/mem, mmap 0x200 at SCU 0x1e6e2000
//   SCU00 (0x1e6e2000) = 0x1688a8a8       <- SCU register UNLOCK key
//   platform id = u16 at SCU + 0x66
//     if (id == 0x139 || id == 0x13b) && enable    SCU40 |= 0x1000
//   if (disable) { SCU180 &= ~0x2;  SCU40 |= 0x1000; }
//   else         { SCU180 |=  0x2;  if (id not special) SCU40 &= ~0x1000; }
//   SCU00 = 0                              <- re-lock
//
// The previous revision set SCU180 bit1 and SCU40 bit12 unconditionally and
// never unlocked SCU00 first, so the writes were discarded and SCU40 ended up
// on the wrong side of stock's platform-conditional branch. SCU180 bit1 is
// SCU180_ENP2A (the bit aspeed-p2a-ctrl names); SCU40 bit12 is the companion
// bit stock only raises for the two special platform ids above.
//
// SCU2C's own region gates (DRAM = bit 25) come up open from u-boot and stay
// untouched: this image deliberately does not probe aspeed-p2a-ctrl, whose
// probe sets every region bit and blocks the host VGA BAR.
//
// /dev/mem exists in the stock rootfs but not in ours, so create the node on
// first use; the kernel has CONFIG_DEVMEM=y and non-RAM ranges pass its strict
// check.
constexpr off_t scuBase = 0x1e6e2000;
constexpr size_t scuSpan = 0x200;
constexpr const char* memDev = "/dev/mem";

constexpr size_t scuOffUnlock = 0x00;   // SCU00: 0x1688a8a8 unlocks
constexpr size_t scuOffMisc2c = 0x2c;   // SCU2C region gates (log only)
constexpr size_t scuOffCtrl40 = 0x40;   // bit12: stock companion bit
constexpr size_t scuOffPcie180 = 0x180; // bit1: SCU180_ENP2A
constexpr size_t scuOffPlatform = 0x66; // u16 platform/board id

constexpr uint32_t scuUnlockKey = 0x1688A8A8u;
constexpr uint32_t scu180EnP2A = 0x00000002u;
constexpr uint32_t scu40Shadow = 0x00001000u;

// The two "special" platform ids stock singles out in the same +0x66 check it
// uses before raising SCU40 bit12 (GetOEMPlatformID in libgbtmisc.so compares
// the identical pair, 0x139/0x13b).
constexpr uint16_t platformIdSpecialA = 0x0139;
constexpr uint16_t platformIdSpecialB = 0x013B;

int openMemDevice()
{
    int fd = ::open(memDev, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd >= 0)
    {
        return fd;
    }
    (void)::mknod(memDev, S_IFCHR | 0600, ::makedev(1, 1));
    return ::open(memDev, O_RDWR | O_SYNC | O_CLOEXEC);
}

// What a share-memory toggle observed and left behind, for the log.
struct ScuSnapshot
{
    bool mapped{false};
    uint16_t platformId{0};
    bool special{false};
    uint32_t scu2c{0};
    uint32_t scu40{0};
    uint32_t scu180{0};
};

// Runs stock endis_share_memory() verbatim: fresh map, unlock, the branch,
// lock, unmap. `enable` is stock's parameter (true = the BIOS may write).
ScuSnapshot endisShareMemory(bool enable)
{
    ScuSnapshot s;
    const int fd = openMemDevice();
    if (fd < 0)
    {
        return s;
    }
    void* map = ::mmap(nullptr, scuSpan, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, scuBase);
    if (map == MAP_FAILED)
    {
        ::close(fd);
        return s;
    }
    volatile uint8_t* b = static_cast<volatile uint8_t*>(map);
    volatile uint32_t* r = reinterpret_cast<volatile uint32_t*>(map);

    s.platformId = *reinterpret_cast<volatile uint16_t*>(b + scuOffPlatform);
    s.special = (s.platformId == platformIdSpecialA ||
                 s.platformId == platformIdSpecialB);

    r[scuOffUnlock / 4] = scuUnlockKey; // unlock
    if (enable && s.special)
    {
        r[scuOffCtrl40 / 4] |= scu40Shadow;
    }
    if (!enable)
    {
        r[scuOffPcie180 / 4] &= ~scu180EnP2A;
        r[scuOffCtrl40 / 4] |= scu40Shadow;
    }
    else
    {
        r[scuOffPcie180 / 4] |= scu180EnP2A;
        if (!s.special)
        {
            r[scuOffCtrl40 / 4] &= ~scu40Shadow;
        }
    }
    r[scuOffUnlock / 4] = 0; // re-lock

    s.scu2c = r[scuOffMisc2c / 4];
    s.scu40 = r[scuOffCtrl40 / 4];
    s.scu180 = r[scuOffPcie180 / 4];
    s.mapped = true;
    ::munmap(map, scuSpan);
    ::close(fd);
    return s;
}

void enableHostPushPath()
{
    const ScuSnapshot s = endisShareMemory(true);
    if (!s.mapped)
    {
        lg2::error("AMI OEM PROBE: cannot map SCU for write-path enable");
        return;
    }
    lg2::info("AMI OEM PROBE: host push path enabled (pid={PID} special={SP} "
              "SCU180={H180} SCU40={H40} SCU2C={H2C})",
              "PID", lg2::hex, s.platformId, "SP", s.special ? 1 : 0, "H180",
              lg2::hex, s.scu180, "H40", lg2::hex, s.scu40, "H2C", lg2::hex,
              s.scu2c);
}

// Read-only SCU dump, taken around every netfn-0x2E op so the log shows the
// gate state the BIOS actually saw at each step.
void logScuState(const std::string& where)
{
    const int fd = openMemDevice();
    if (fd < 0)
    {
        return;
    }
    void* map = ::mmap(nullptr, scuSpan, PROT_READ, MAP_SHARED, fd, scuBase);
    if (map == MAP_FAILED)
    {
        ::close(fd);
        return;
    }
    volatile uint8_t* b = static_cast<volatile uint8_t*>(map);
    volatile uint32_t* r = reinterpret_cast<volatile uint32_t*>(map);
    appendCapture(timestamp() + " SCU-state " + where + " 00=" +
                  toHex(r[scuOffUnlock / 4], 8) + " 2c=" +
                  toHex(r[scuOffMisc2c / 4], 8) + " 40=" +
                  toHex(r[scuOffCtrl40 / 4], 8) + " 180=" +
                  toHex(r[scuOffPcie180 / 4], 8) + " pid=" +
                  toHex(*reinterpret_cast<volatile uint16_t*>(b + scuOffPlatform),
                        4));
    ::munmap(map, scuSpan);
    ::close(fd);
}


ipmi::RspType<std::vector<uint8_t>> handleAmiOemCommand(
    ipmi::message::Request::ptr request, const std::vector<uint8_t>& body)
{
    const ipmi::Context::ptr ctx = request->ctx;
    const uint8_t cmd = ctx->cmd;

    // The framework already consumed the 3-byte IANA before dispatching to
    // us, but Payload::data()/size() report the whole buffer rather than the
    // cursor-relative remainder (ipmid-new.cpp executeIpmiOemCommand unpacks
    // from a cursor), so the IANA is still readable here and `body` is
    // exactly that buffer shifted by ianaBytes.
    const uint8_t* raw = request->payload.data();
    const size_t rawLen = request->payload.size();
    ipmi::Iana iana = 0;
    if (rawLen >= ianaBytes)
    {
        iana = static_cast<ipmi::Iana>(raw[0] | (raw[1] << 8) |
                                       (raw[2] << 16));
    }

    lg2::info("AMI OEM PROBE: IANA={IANA} Cmd={CMD} {NAME} channel={CHANNEL} "
              "priv={PRIV} bodyLen={LEN} body=[{BODY}]",
              "IANA", lg2::hex, iana, "CMD", lg2::hex, cmd, "NAME",
              commandName(cmd), "CHANNEL", ctx->channel, "PRIV",
              static_cast<unsigned>(ctx->priv), "LEN", body.size(), "BODY",
              hexDump(body));

    appendCapture(timestamp() + " netfn=0x2e iana=0x" +
                  toHex(iana, 6) + " cmd=0x" + toHex(cmd, 2) + " name=" +
                  commandName(cmd) + " chan=" +
                  std::to_string(static_cast<unsigned>(ctx->channel)) +
                  " priv=" + std::to_string(static_cast<unsigned>(ctx->priv)) +
                  " len=" + std::to_string(body.size()) + " body=[" +
                  hexDump(body) + "]");

    logScuState("cmd=0x" + toHex(cmd, 2));
    logWindowState("cmd=0x" + toHex(cmd, 2));

    if (containsSmbiosAnchor(body))
    {
        lg2::error("AMI OEM PROBE: SMBIOS anchor in Cmd={CMD} body - the "
                   "tables are moving; capture this body before deciding "
                   "what to do with it",
                   "CMD", lg2::hex, cmd);
    }

    // --- host-push window capture ---------------------------------------------
    // The BIOS does not carry its bulk payloads in these IPMI messages. It
    // writes them into the host-visible window just below the VGA framebuffer
    // (0x9e000000, 16 MiB, mapped for us via /dev/aspeed-lpc-ctrl now that the
    // DTS gives lpc-ctrl a memory-region) and then announces it over IPMI:
    //
    //   op 0x0B/0x13  SMBIOS transfer,  body[1..2] = LE u16 data length
    //   op 0x52       Redfish post --   body[1..4] LE u32 length, body[5] mode,
    //                                   body[6..]  = Redfish path ("Systems/
    //                                   Self/Bios", "/redfish/v1/Registries/
    //                                   BiosAttributeRegistry.json", ...)
    //
    // Stock libgbtmisc.so mmap()s this window inside the same IPMI command and
    // then does NOT clean up -- so latching the bytes here is exactly when the
    // stock firmware would have consumed them. Replies are unchanged: every
    // op we have seen so far has been accepted as-is.
    if (!body.empty() && iana == ianaInventory)
    {
        const uint8_t op = body[0];
        size_t windowLen = 0;
        const char* capturePath = nullptr;
        if (op == opSmbiosTransfer && body.size() >= 3)
        {
            windowLen = static_cast<size_t>(body[1]) |
                        (static_cast<size_t>(body[2]) << 8);
            capturePath = smbiosWindowPath;
        }
        else if (op == opRedfishPost && body.size() >= 5)
        {
            windowLen = static_cast<size_t>(body[1]) |
                        (static_cast<size_t>(body[2]) << 8) |
                        (static_cast<size_t>(body[3]) << 16) |
                        (static_cast<size_t>(body[4]) << 24);
            // The observed bodies carry a u32 length of 1 yet a full Redfish
            // path string, which the stock code cannot consume with 1 byte;
            // keep a floor amount so the register/dataset payload behind it
            // (tens of KB) is not truncated.
            if (body.size() >= 7)
            {
                windowLen = std::max<size_t>(windowLen, 0x8000);
            }
            capturePath = redfishWindowPath;
        }
        if (windowLen > 0)
        {
            if (windowLen > maxWindowCapture)
            {
                windowLen = maxWindowCapture;
            }
            captureHostWindow(windowLen, capturePath, op);
            if (op == opSmbiosTransfer)
            {
                // The push is complete by the time it is announced, so publish
                // it from here rather than waiting for a later command that a
                // given BIOS revision may never send.
                HostWindowMap& win = hostWindowMap();
                if (win.base != MAP_FAILED)
                {
                    publishSmbiosToInventory(win.base, windowLen, op);
                }
            }
        }
    }

    // Answer, per command.
    //
    // The default is to ECHO the request body, because echoing request fields
    // is the one pattern every Gigabyte netfn-0x2E handler we have decompiled
    // follows (see the convention note above the constants). An earlier
    // revision invented a 4-byte handle instead -- structure with no evidence
    // behind it, which is exactly what the decompiled handlers do not do.
    //
    // Still synthetic either way: this provider never touches BMC memory and
    // writes nothing to flash.
    std::vector<uint8_t> reply;
    if (body.size() <= maxEchoBytes)
    {
        reply = body;
    }

    switch (cmd)
    {
        case cmdAmiAllocateMemory: // 0x20
            // Carries the buffer name (e.g. "Systems/Self/Bios"). Echo only the
            // leading id, not the whole body, so the name is not handed back
            // as if it were part of an id.
            reply.assign(body.begin(),
                         body.begin() + std::min<size_t>(4, body.size()));
            break;
        case cmdAmiFreeMemory:          // 0x21
        case cmdAmiCopyMemory:          // 0x32
        case 0x11:                      // observed, unmapped
        case cmdAmiAddExtendSelEntries: // 0xCC
            // Plain acknowledgements.
            reply.clear();
            break;
        case cmdAmiReadMemory: // 0x30
            // Only needed if the BIOS reads a buffer back. Honour a plausible
            // little-endian length at offset 8 and answer with zeroes.
            // IPMI over KCS cannot carry more than ~240 bytes in one response,
            // so that is a protocol bound, not a tuning knob.
            if (body.size() >= 12)
            {
                constexpr uint32_t maxReadBack = 240;
                const uint32_t len = static_cast<uint32_t>(body[8]) |
                                     (static_cast<uint32_t>(body[9]) << 8) |
                                     (static_cast<uint32_t>(body[10]) << 16) |
                                     (static_cast<uint32_t>(body[11]) << 24);
                if (len > 0 && len <= maxReadBack)
                {
                    reply.assign(len, 0x00);
                }
            }
            break;
        case cmdAmiCompareMemory: // 0x33
            // Deliberately "not equal" -- see compareNotEqual above.
            reply.assign(1, compareNotEqual);
            break;
        default:
            // Keep the echo.
            break;
    }

    // --- stock-shaped replies for the window-anchored ops -------------------
    //
    // Echo was enough to keep the BIOS from retrying, but the transferred
    // data never appeared: decompiles of the stock libgbtmisc.so handlers
    // pin the exact reply bytes, and they are not echoes.
    //
    //   op 0x50/0x51  _GetShmAddr_: runs endis_share_memory(1) (the SCU
    //                 write-path enable we now set at init) and replies
    //                 5 bytes = [u32 LE window phys base][1]. This is how
    //                 the BIOS learns where to push.
    //   op 0x0B/0x13  SmbiosInterfaceLoadShm stores its out param verbatim
    //                 from the request's second u16 (disassembly at
    //                 0x52d70 writes [fp-44], the stack arg, straight to
    //                 the reply pointer), so the reply is 5 bytes =
    //                 [body[3], body[4], 0, 0, 0].
    //   op 0x52       post_redfish_transfer replies 9 bytes = [u32 count
    //                 written to its capture file][body[5]][3 unused][1];
    //                 the stock count was the request's own u32.
    if (!body.empty() && iana == ianaInventory)
    {
        const uint8_t op = body[0];
        std::vector<uint8_t> stock;
        auto logToggle = [&](const ScuSnapshot& s, const char* what) {
            appendCapture(timestamp() + " SCU " + what + " op=0x" +
                          toHex(op, 2) + " pid=" + toHex(s.platformId, 4) +
                          " special=" + std::to_string(s.special ? 1 : 0) +
                          " 2c=" + toHex(s.scu2c, 8) + " 40=" +
                          toHex(s.scu40, 8) + " 180=" + toHex(s.scu180, 8));
        };
        // Wire op 0x0A (cmd 0x21) is _GetShmAddr_ in the stock op table, the
        // same handler the op-0x50 family reaches, and it carries no arguments
        // -- so it must not be gated on a body length the way the op-0x50 form
        // is. It is the handshake SendIpmiSystemInfo uses to learn its DRAM
        // destination, and the reply layout is identical.
        if (op == opShmAddrQuery || (op == opGetShmAddr && body.size() >= 6))
        {
            // Stock _GetShmAddr_ re-asserts the write path (endis_share_memory
            // (1)) before it answers, so the enable happens at the handshake
            // itself, not only at init.
            const ScuSnapshot s = endisShareMemory(true);
            logToggle(s, "enable");
            // _GetShmAddr_ writes *param_3 = address and *(param_3 + 1) = 1 (a
            // plain constant, not anything from the request) and reports the
            // reply length as 5.
            stock = {static_cast<uint8_t>(shmWindowPhys),
                     static_cast<uint8_t>(shmWindowPhys >> 8),
                     static_cast<uint8_t>(shmWindowPhys >> 16),
                     static_cast<uint8_t>(shmWindowPhys >> 24), 0x01};
        }
        else if (op == opSetWriteProtect && body.size() >= 2)
        {
            // Stock _SetWriteProtect_(arg): arg 0 -> endis_share_memory(1)
            // (BIOS may write the window), arg 1 -> endis_share_memory(0)
            // (window closed). Mirror it; the reply stays the echo.
            const bool enable = (body[1] == 0x00);
            logToggle(endisShareMemory(enable), enable ? "enable" : "disable");
        }
        else if (op == opSmbiosTransfer && body.size() >= 5)
        {
            stock = {body[3], body[4], 0x00, 0x00, 0x00};
        }
        else if (op == opRedfishPost && body.size() >= 6)
        {
            stock = {body[1], body[2], body[3], body[4], body[5],
                     0x00, 0x00, 0x00, 0x01};
        }
        if (!stock.empty())
        {
            lg2::info("AMI OEM PROBE: stock reply op=0x{OP} bytes={N}",
                      "OP", lg2::hex, static_cast<uint32_t>(op), "N",
                      stock.size());
            reply = std::move(stock);
        }
    }

    return ipmi::responseSuccess(reply);
}

// --- all-command capture filter ---------------------------------------------
//
// ipmid runs registered filters ahead of every command handler, on every
// channel, and passes them through unless the filter returns an error CC. This
// one only logs: it gives a complete transcript of what the host asked during
// POST -- including commands the provider does not claim, like the App
// self-test (0x06/0x04) and the Storage (0x0A) reads the AMI HII drivers
// issue, which never appear in the per-command captures above.
ipmi::Cc logAllIpmiCommands(ipmi::message::Request::ptr request)
{
    appendCapture(timestamp() + " FILTER netfn=0x" +
                  toHex(static_cast<uint32_t>(request->ctx->netFn), 2) +
                  " cmd=0x" + toHex(static_cast<uint32_t>(request->ctx->cmd), 2) +
                  " chan=" +
                  std::to_string(static_cast<unsigned>(request->ctx->channel)) +
                  " priv=" +
                  std::to_string(static_cast<unsigned>(request->ctx->priv)) +
                  " len=" +
                  std::to_string(request->payload.raw.size()) + " body=[" +
                  hexDump(std::vector<uint8_t>(request->payload.raw.begin(),
                                              request->payload.raw.end())) + "]");
    return ipmi::ccSuccess;
}

// NetFn 0x06 (App) fn 0x01 -- Get Device ID shadow. See the block at the
// handler for why it is registered.
constexpr const char* devIdJsonPath = "/usr/share/ipmi-providers/dev_id.json";

// Minimal integer lookup for the known-shape dev_id.json (no JSON dependency).
// Numbers only; booleans and nested objects are intended to be absent.
int jsonInt(const std::string& text, const std::string& key, int def)
{
    const std::string pat = "\"" + key + "\"";
    size_t pos = text.find(pat);
    if (pos == std::string::npos)
    {
        return def;
    }
    pos = text.find(':', pos + pat.size());
    if (pos == std::string::npos)
    {
        return def;
    }
    return std::strtol(text.c_str() + pos + 1, nullptr, 0);
}

ipmi::RspType<uint8_t,        // Device ID
              uint8_t,        // Device Revision
              uint8_t,        // Firmware revision major
              uint8_t,        // Firmware revision minor (IPMI-spec BCD form)
              uint8_t,        // IPMI version
              uint8_t,        // Additional device support
              uint24_t,             // MFG ID
              uint16_t,       // Product ID
              uint32_t        // AUX info
              >
    handleGetDeviceIdShadow(ipmi::Context::ptr ctx)
// The BIOS's BMC-status collector and its "BIOS Information" page take the
// "BMC Firmware Version" display from the Get Device ID payload, and ipmid's
// App handlers log nothing -- a wrong or mistimed answer there is invisible
// from the BMC. Registering (0x06,0x01) at prioOemBase supplants the core
// handler (prioOpenBmcBase), so every Get Device ID on every channel -- KCS
// included -- appears in /tmp/ami-probe.log with its exact reply bytes.
//
// Deliberate differences from ipmiAppGetDeviceId in apphandler.cpp:
//   * the live D-Bus "BMC ready" availability check is skipped and the
//     file's "availability" fallback is served. When the state manager is
//     Ready (the steady state after the boot-completion fix) the two replies
//     are byte-identical; removing this provider restores the stock check.
//   * values are re-read from the same JSON file the core handler uses, so
//     the two stay in sync by construction.
{
    int id = 32;
    int revision = 1;
    int addn = 1;
    int manuf = 0;
    int prod = 0;
    int aux = 0;
    int availability = 1;
    int fw[] = {0, 0};

    std::ifstream file(devIdJsonPath);
    std::string text((std::istream_iterator<char>(file)),
                     std::istream_iterator<char>());
    if (!text.empty())
    {
        id = jsonInt(text, "id", id);
        revision = jsonInt(text, "revision", revision);
        addn = jsonInt(text, "addn_dev_support", addn);
        manuf = jsonInt(text, "manuf_id", manuf);
        prod = jsonInt(text, "prod_id", prod);
        aux = jsonInt(text, "aux", aux);
        fw[0] = jsonInt(text, "major", fw[0]);
        fw[1] = jsonInt(text, "minor", fw[1]);
        availability = (text.find("\"availability\": true") !=
                                std::string::npos ||
                            text.find("\"availability\":true") !=
                                std::string::npos)
                           ? 1
                           : jsonInt(text, "availability", availability);
    }

    const uint8_t id8 = static_cast<uint8_t>(id);
    const uint8_t revision8 = static_cast<uint8_t>(revision);
    uint8_t fw0 = static_cast<uint8_t>(fw[0]);
    const uint8_t fw1 = static_cast<uint8_t>(fw[1]);
    if (!availability)
    {
        // Mirror the IPMI-spec "device not available" bit the core handler
        // sets when the state manager is not Ready.
        fw0 |= 0x80;
    }

    const uint8_t reply[] = {id8,         revision8, fw0, fw1,  0x02, // IPMI 2.0
                             static_cast<uint8_t>(addn),
                             static_cast<uint8_t>(manuf),
                             static_cast<uint8_t>(manuf >> 8),
                             static_cast<uint8_t>(manuf >> 16),
                             static_cast<uint8_t>(prod),
                             static_cast<uint8_t>(prod >> 8),
                             static_cast<uint8_t>(aux),
                             static_cast<uint8_t>(aux >> 8),
                             static_cast<uint8_t>(aux >> 16),
                             static_cast<uint8_t>(aux >> 24)};

    appendCapture(timestamp() + " netfn=0x06 cmd=0x01 chan=" +
                  std::to_string(static_cast<unsigned>(ctx->channel)) +
                  " priv=" + std::to_string(
                                 static_cast<unsigned>(ctx->priv)) +
                  " reply=[" + hexDump(std::vector<uint8_t>(
                                   reply, reply + sizeof(reply))) + "]");
    lg2::info("AMI OEM PROBE: Get Device ID shadow channel={CHANNEL} "
              "fw={FW0}.{FW1_HEX} replyLen={LEN}",
              "CHANNEL", ctx->channel, "FW0", static_cast<unsigned>(fw0),
              "FW1_HEX", lg2::hex, static_cast<unsigned>(fw1), "LEN",
              sizeof(reply));

    uint24_t manufId = 0;
    manufId = static_cast<uint32_t>(manuf);
    return ipmi::responseSuccess(
        id8, revision8, fw0, fw1, reply[4],
        static_cast<uint8_t>(addn), manufId, static_cast<uint16_t>(prod),
        static_cast<uint32_t>(aux));
}

// NetFn 0x32 is AMI's REST/host-interface control channel. Two commands are
// now implemented against the stock handlers (see the constants above);
// everything else stays unhandled, which is what the BIOS already tolerates.
//
// Why this matters: the BIOS only starts its Redfish session if 0x3D answers
// with the three status bytes. An empty success was tried on 2026-10-16 and
// made the BIOS poll every 2 s for minutes (it reads as "not ready"), while
// CmdInvalidCommand makes it give up on the host interface immediately. With
// the correct body it should proceed to 0x5D, fetch credentials and open
// HTTPS to 169.254.0.17 -- which bmcweb already serves on this board.
ipmi::RspType<std::vector<uint8_t>> handleNetfnOemTwoCommand(
    ipmi::message::Request::ptr request, const std::vector<uint8_t>& body)
{
    const ipmi::Context::ptr ctx = request->ctx;
    const uint8_t cmd = ctx->cmd;

    lg2::info("AMI OEM PROBE NETFN32: Cmd={CMD} {NAME} channel={CHANNEL} "
              "priv={PRIV} bodyLen={LEN} body=[{BODY}]",
              "CMD", lg2::hex, cmd, "NAME", commandName32(cmd), "CHANNEL",
              ctx->channel, "PRIV", static_cast<unsigned>(ctx->priv), "LEN",
              body.size(), "BODY", hexDump(body));

    appendCapture(timestamp() + " netfn=0x32 cmd=0x" + toHex(cmd, 2) +
                  " name=" + commandName32(cmd) + " chan=" +
                  std::to_string(static_cast<unsigned>(ctx->channel)) +
                  " priv=" + std::to_string(static_cast<unsigned>(ctx->priv)) +
                  " len=" + std::to_string(body.size()) + " body=[" +
                  hexDump(body) + "]");

    // Error replies carry no payload, so they cannot use responseInvalidCommand
    // (that returns RspType<>, a different tuple type than this handler's).
    auto errorRsp = [](ipmi::Cc cc) {
        return ipmi::RspType<std::vector<uint8_t>>{cc, std::nullopt};
    };

    std::vector<uint8_t> reply;
    switch (cmd)
    {
        case cmd32Unnamed3D: // 0x3D AMIGetBMCInterfaceStatus
            // Stock reads req[0], req[1], req[2] and rejects req[0] != 1,
            // req[1] != 0 or req[2] > 7. The reply is the CC followed by three
            // 0x07 status bytes; ipmid adds the CC, so the payload is 07 07 07.
            if (body.size() < 3 || body[0] != 0x01 || body[1] != 0x00 ||
                body[2] > 0x07)
            {
                // AMI uses 0xCC ("invalid data field") for this, not 0xC1.
                return errorRsp(static_cast<ipmi::Cc>(0xCC));
            }
            reply.assign(ifaceStatusBytes, ifaceStatusReady);
            break;

        case cmd32HostInterface: // 0x5D AMIGeneratePassword
            if (body.size() != 1)
            {
                return errorRsp(ipmi::ccReqDataLenInvalid);
            }
            switch (body[0])
            {
                case 0x01: // firmware host-interface session
                case 0x02: // kernel/OS host-interface session
                    // Stock answers {CC, ulen, plen, username, password}; the
                    // CC is added by ipmid. Same credentials for both modes is
                    // deliberate for the first experiment.
                    reply.reserve(2 + hostIfUser.size() +
                                  hostIfPassword.size());
                    reply.push_back(static_cast<uint8_t>(hostIfUser.size()));
                    reply.push_back(
                        static_cast<uint8_t>(hostIfPassword.size()));
                    reply.insert(reply.end(), hostIfUser.begin(),
                                 hostIfUser.end());
                    reply.insert(reply.end(), hostIfPassword.begin(),
                                 hostIfPassword.end());
                    break;
                case 0x04: // tear down the firmware session
                case 0x08: // tear down the kernel session
                    break;
                default:
                    return errorRsp(static_cast<ipmi::Cc>(0xCC));
            }
            break;

        default:
            return errorRsp(ipmi::ccInvalidCommand);
    }

    appendCapture(timestamp() + " netfn=0x32 cmd=0x" + toHex(cmd, 2) +
                  " reply=[" + hexDump(reply) + "]");
    return ipmi::responseSuccess(reply);
}

void registerAmiOemProbe() __attribute__((constructor));

void registerAmiOemProbe()
{
    // Bring the host write path up before the first request can arrive.
    enableHostPushPath();

    for (const ipmi::Iana iana : {ianaInventory, ianaSecondary})
    {
        // Wildcard command: every command under this IANA lands here, which
        // is the point -- we do not know the full command set yet.
        //
        // Privilege::Callback rather than the usual User: this is a capture
        // instrument, and a request rejected on privilege teaches us
        // nothing. Nothing dangerous is done with what arrives (the body is
        // logged and the answer is synthetic), and the privilege the host
        // actually uses is logged per request, so this can be tightened once
        // we know what it is.
        ipmi::registerOemHandler(
            ipmi::prioOemBase, iana, ipmi::cmdWildcard,
            ipmi::Privilege::Callback,
            [](ipmi::message::Request::ptr request,
               const std::vector<uint8_t>& body) {
                return handleAmiOemCommand(request, body);
            });
    }

    // Capture filter: pass-through observer of every IPMI command (see its
    // comment). Registered at prioOemBase so it precedes the whitelist filter
    // (which sorts lower) and therefore sees even rejected commands.
    ipmi::registerFilter(ipmi::prioOemBase,
                         [](ipmi::message::Request::ptr request) {
                             return logAllIpmiCommands(request);
                         });

    // App 0x06 fn 0x01 Get Device ID shadow + capture (see handler comment).
    // The reply replicates the core handler byte for byte whenever the state
    // manager reports Ready, which is now the steady state.
    ipmi::registerHandler(ipmi::prioOemBase, ipmi::netFnApp,
                          ipmi::app::cmdGetDeviceId, ipmi::Privilege::User,
                          handleGetDeviceIdShadow);

    // NetFn 0x32, as concrete commands plus a wildcard. Registering the exact
    // command numbers is documentation of what the host actually sent;
    // executeIpmiCommandCommon (ipmid-new.cpp) prefers an exact match and only
    // falls back to the wildcard, so anything new is still captured.
    for (const uint8_t cmd :
         {cmd32Unnamed3D, cmd32HostInterface, cmd32Query, ipmi::cmdWildcard})
    {
        ipmi::registerHandler(
            ipmi::prioOemBase, ipmi::netFnOemTwo, cmd,
            ipmi::Privilege::Callback,
            [](ipmi::message::Request::ptr request,
               const std::vector<uint8_t>& body) {
                return handleNetfnOemTwoCommand(request, body);
            });
    }

    lg2::info("AMI OEM PROBE: registered NetFn 0x2E handlers for IANA "
              "{IANA1}/{IANA2} and NetFn 0x32 handlers; capturing to {LOG}",
              "IANA1", lg2::hex, ianaInventory, "IANA2", lg2::hex,
              ianaSecondary, "LOG", captureLogPath);
}

} // namespace
