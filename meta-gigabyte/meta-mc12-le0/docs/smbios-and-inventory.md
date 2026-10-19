# SMBIOS and inventory on the MC12-LE0

How this board's host BIOS hands its SMBIOS tables to the BMC, and how those
tables become OpenBMC inventory and Redfish.

The short version: the BIOS does **not** speak Intel's MDR v2 or the generic
`smbios-ipmi-blob` protocol. It holds an AMI-proprietary conversation over
in-band IPMI, and the table bytes themselves travel through a shared DRAM
window rather than through IPMI payloads. `ami-oem-probe` implements that
conversation, copies the window out, and re-emits the table in the shape
`smbios-mdr` will parse.

```
host BIOS                                     BMC (OpenBMC)
---------                                     -------------

                                              [1] ami-oem-probe
                                                  /usr/lib/ipmid-providers/
                                                  libami-oem-probe.so

  NetFn 0x2E, IANA 0x003C0A
  cmd 0x21, body [0A]      ---------------->  op 0x0A  -->  reply, 5 bytes:
                                                            u32 LE 0x9E000000, 01
                                                            (SendIpmiSystemInfo reads
                                                             this at reply offset 3 as
                                                             its DMA destination, so it
                                                             must not be an echo)

  cmd 0x20, body [24 00]   ---------------->  op 0x24  -->  SCU write path opened

    host DMAs the SMBIOS set  ------------->  [2] 0x9E000000, 16 MiB
                                                  the DRAM window: reserved in the
                                                  DTS, read back through
                                                  /dev/aspeed-lpc-ctrl

  cmd 0x20
  body [0B A0 12 7E 12 00 00] ------------>  op 0x0B  -->  [3] /tmp/ami-smbios.bin
                                                  and     [4] publishSmbiosToInventory
                                                              --> /var/lib/smbios/smbios2
                                                              --> AgentSynchronizeData

                                              [5] smbios-mdr (smbiosmdrv2app)
                                                  re-parses smbios2 and owns
                                                  /xyz/openbmc_project/inventory/system/
                                                    chassis/motherboard/{bios,cpu0,pcieslot*}

                                              [6] dimm-inventory (separate daemon)
                                                  owns the DIMM objects, built from the
                                                  SPD EEPROMs and enriched from the
                                                  same smbios2 file

                                              [7] bmcweb  -->  Redfish
```

Everything below is either the wire protocol, the window, the bridge, or one of
the four consumers. Sources:

| Piece | File |
|---|---|
| The AMI OEM provider | `recipes-phosphor/ipmi/ami-oem-probe/main.cpp` |
| D-Bus → smbios-mdr inventory | `recipes-phosphor/smbios/smbios-mdr_%.bbappend` (config) |
| SPD daemon + Type 17 reader | `recipes-gigabyte/dimm-inventory/files/{main,spd,smbios}.{cpp,hpp}` |
| DRAM window + lpc-ctrl | `recipes-kernel/linux/linux-aspeed/gigabyte-mc12-le0.dts` |
| Both binaries in the image | `recipes-gigabyte/packagegroups/packagegroup-gigabyte-apps.bb` |

---

## 1. Why the standard OpenBMC path does not apply

Upstream expects one of:

* **Intel MDR v2** — the host PECI/DMI agent hands tables over. Absent here
  (AMD host, no MDR v2 engine).
* **`smbios-ipmi-blob`** — the host uploads the table as an IPMI blob; the
  `smbiosstore` handler writes `/var/lib/smbios/smbios2` and pokes
  `xyz.openbmc_project.Smbios.MDR_V2`. This board's BIOS never does that.
* **`cpuinfo` / `cpuinfo-peci`** — reads Intel PIROM over PECI. AMD host.

The board instead ships AMI's MegaRAC stack (`libgbt.so`, `libgbtmisc.so`) which
implements its own in-band protocol. That stack is absent from an OpenBMC image,
which is exactly why the host's tables went nowhere until `ami-oem-probe`
implemented the BMC end of it.

Two facts shape everything:

* **Nothing is sent at POST over the network.** The BIOS only talks in-band
  IPMI (KCS, I/O `0xCA2`, DTS `&kcs3`). No HTTP, no Redfish session of its own,
  until after its IPMI handshake succeeds.
* **Bulk data does not travel in IPMI payloads.** Only small control messages
  do. The tables live in a DRAM window the host writes directly.

---

## 2. The two OEM IPMI channels

`ami-oem-probe` registers handlers on two netfns. Requests are observed with
channel `15` (`0x0F`, the IPMI "system interface", i.e. KCS) and privilege `4`
(Administrator).

### 2.1 NetFn `0x2E` — the data channel

The OEM IANA is part of the request. Two are used:

| Wire IANA bytes | Value | Used for |
|---|---|---|
| `0A 3C 00` | `0x003C0A` | every command except `0xCC` |
| `5E 2B 00` | `0x002B5E` | `0xCC` (SEL) |

The framework packs those three bytes little-endian into the IANA number, so the
handler sees `0x003C0A` / `0x002B5E`. Both IANAs are claimed with a **wildcard
command**, because the full command set is not known — anything new is captured
rather than rejected.

Two layers of selection exist on this netfn, and they are easy to confuse:

* the **IPMI command** (`ctx->cmd`) — the AMI "get/set" pair, plus four others;
* the **op** — the **first payload byte after the IANA** (`body[0]`).

| cmd | Meaning (empirical) | Observed bodies |
|---|---|---|
| `0x20` | submit / set: `body[0]` is the op, rest are arguments | `[24 00]`, `[0B A0 12 7E 12 00 00]`, `[52 01 00 00 00 01 "Systems/Self/Bios"]` |
| `0x21` | query / get: `body[0]` is the op, usually no arguments | `[0A]`, `[0D]`, `[18]`, `[3C]` |
| `0x30` | read memory back (no body observed) | `[]` |
| `0x32` | copy memory region | `FILE0\0FILE0\0…` |
| `0x33` | compare memory region | `[00]`, `[01]`, `[02]`, `[03]`, each followed by `FILE0\0 …` |
| `0x11` | unmapped, one byte | `[16]`, `[01]` |
| `0xCC` | on the `0x002B5E` IANA | `[0C 02]` |

Ops:

| op | Name (from stock's op table) | Direction |
|---|---|---|
| `0x0A` | `_GetShmAddr_`, query form | BIOS → BMC |
| `0x50` | `_GetShmAddr_`, submit form (`[50 94 63 00 00 01]`) | BIOS → BMC |
| `0x24` | `_SetWriteProtect_`, `body[1]` = 0 open / 1 close | BIOS → BMC |
| `0x0B` | SMBIOS transfer announce (stock's table lists `0x13` alongside it; only `0x0B` observed) | BIOS → BMC |
| `0x52` | post a Redfish request (`body[5]` = mode, `body[6..]` = path) | BIOS → BMC |
| `0x0C` | generic slot handler | BIOS → BMC |

#### Replies

There is **no status byte, no length field and no checksum** in the response
envelope. The framework prepends the 3-byte IANA to whatever the handler wrote,
so `ipmi::responseSuccess(bytes)` is already the correct envelope. This was
established by decompiling Gigabyte's own netfn-`0x2E` handlers, whose only
shape is: validate the exact request length, then copy request fields into fixed
response offsets (`handler_2e01` → `rsp[0..2] = req[0..2]`, `handler_2e13` →
`rsp[0..4] = req[0..4]`, and so on). They echo. So echoing is grounded, and
inventing a handle or a header block is not.

Errors: those handlers return `199` on a length mismatch, and AMI's IANA check
returns `0xCC`.

The non-echo replies, all recovered from the stock libraries:

| Request | Reply | Why it cannot be an echo |
|---|---|---|
| op `0x0A` (any length), or op `0x50` with ≥6 bytes | 5 bytes `[u32 LE 0x9E000000][0x01]` | `SendIpmiSystemInfo` reads the four bytes at **response offset 3** and uses them as the DRAM destination for its DMA. Echoing `0x0A` makes the host write its table to a garbage address — this was the bug that produced an all-zero window. `_GetShmAddr_` also re-asserts the write path before answering. |
| op `0x0B` / `0x13` with ≥5 bytes | 5 bytes `[body[3], body[4], 0, 0, 0]` | `SmbiosInterfaceLoadShm` stores its out-parameter (the payload length, `body[3..4]`) verbatim into the reply. |
| op `0x52` with ≥6 bytes | 9 bytes `[body[1..4]][body[5]][00 00 00][01]` | `post_redfish_transfer` replies with its written-byte count, the mode, and `1`. |
| cmd `0x20` (default) | echo, but only the first 4 bytes | The body carries a buffer *name* (`Systems/Self/Bios`); the leading id is what gets echoed, not the name. |
| cmd `0x21`, `0x32`, `0x11`, `0xCC` | empty acknowledgement | These are pure notifications. |
| cmd `0x30` | `len` zero bytes, `len` from `body[8..11]`, capped at 240 | Only needed if a BIOS revision reads a buffer back; 240 is the KCS response bound. |
| cmd `0x33` | `0x01` ("not equal") | Deliberate: answering "equal" risks the BIOS deciding the BMC already holds the dataset and skipping the push we want. |
| anything else | echo (`body` ≤ 64 bytes) | The Gigabyte convention. |

### 2.2 NetFn `0x32` — the control channel

This is AMI's REST/host-interface control channel. Only two commands are
implemented against the stock handlers; everything else answers
`ccInvalidCommand`, which is what the BIOS already tolerates when nothing is
registered.

| cmd | Stock handler | Behaviour |
|---|---|---|
| `0x3D` | `AMIGetBMCInterfaceStatus` (`libipmimsghndlr.so:0x34944`) | Request must be `{01, 00, idx≤7}`; any other shape gets CC `0xCC` (AMI's choice, not `0xC1`). Reply payload = `07 07 07` — "every host interface is up". |
| `0x5D` | `AMIGeneratePassword` (`libipmiamioemrestiface.so:0x3564`) | Request is one mode byte. Modes `0x01`/`0x02` (firmware / kernel session) reply `{ulen, plen, username, password}`. Modes `0x04`/`0x08` (teardown) reply empty. Anything else → `0xCC`. |
| `0xAB` | unknown | `ccInvalidCommand`. |

`0x3D` is the gate. An **empty success** reads to the BIOS as "not ready" (it
polled every 2 s for minutes when that was tried), and `CmdInvalidCommand` makes
it give up on the host interface immediately. Only the correct `07 07 07` body
lets POST continue to `0x5D` and open its own HTTPS session to
`169.254.0.17`.

> **Security note.** `0x5D` currently hands back the bench root account
> (`root` / `0penBmc`, hardcoded in `main.cpp`). Stock generates a random
> password and provisions a dedicated account. Replace this with a
> least-privilege account before the port leaves the bench.

### 2.3 A real POST transcript

From `/tmp/ami-probe.log`, one host power-cycle (timestamps trimmed). The host
runs this whole sequence on channel `0x0F`, privilege Administrator:

```
cmd=0x11 body=[16]                              unmapped
cmd=0x21 body=[0A]                              op 0x0A  <- GetShmAddr (query)
cmd=0x20 body=[52 01 00 00 00 01 53 79 ... ]    op 0x52  <- post Redfish "Systems/Self/Bios"
        WINDOW-CAPTURE op=0x52 len=32768 -> /tmp/ami-redfish.bin
cmd=0x20 body=[24 00]                           op 0x24  <- open the share window
cmd=0x30 body=[]                                read back
cmd=0x11 body=[01]                              unmapped
cmd=0x21 body=[3C]                              op 0x3C
cmd=0xCC body=[0C 02]                           IANA 0x002B5E, SEL
netfn 0x32 cmd=0xAB
cmd=0x21 body=[18]                              op 0x18
netfn 0x32 cmd=0x5D body=[01]  -> reply [04 07 'root' '0penBmc']
netfn 0x32 cmd=0x5D body=[02]  -> reply [04 07 'root' '0penBmc']
cmd=0x21 body=[0A]                              op 0x0A
cmd=0x20 body=[0C 00 00 00 00]                  op 0x0C  <- generic slot handler
cmd=0x20 body=[24 01]                           op 0x24  <- close the window
cmd=0x21 body=[0A]                              op 0x0A
cmd=0x21 body=[0D]                              op 0x0D
cmd=0x20 body=[50 94 63 00 00 01]               op 0x50  <- GetShmAddr (submit)
cmd=0x21 body=[0A]                              op 0x0A
cmd=0x20 body=[0B A0 12 7E 12 00 00]            op 0x0B  <- SMBIOS: totalLen=0x12A0=4768,
                                                          payloadLen=0x127E=4734
        WINDOW-CAPTURE op=0x0B len=4768 -> /tmp/ami-smbios.bin
        SMBIOS-BRIDGE op=0x0B ep=0x000000 table=3431 data=3455 wrote=1 synced=1
cmd=0x33 body=[00 FILE0\0 FILE0\0 ...]           compare, answered 0x01 (not equal)
cmd=0x33 body=[01 ...] [02 ...] [03 ...]         three more regions
cmd=0x32 body=[FILE0\0 FILE0\0 ...]              copy
```

Two op-`0x2E` details worth keeping in mind when reading a capture:

* The window is opened (`0x24 00`) around the transfer and closed (`0x24 01`)
  after it. The probe re-asserts the open at every `0x0A`/`0x50` and mirrors
  `0x24`, so the two are consistent no matter which order a BIOS revision uses.
* `0x0A` appears many times and carries **no arguments** — it must not be gated
  on a body length.

---

## 3. The shared-memory window

### 3.1 Where it is

| | |
|---|---|
| Physical base | `0x9E000000` |
| Size | 16 MiB |
| BMC DRAM | `0x80000000` + 480 MiB usable |
| Just above it | `0x9F000000`, 16 MiB — the host VGA framebuffer |

The address is not arbitrary. Stock's `shm_get_addrinfo()` parses `mem=` out of
`/proc/cmdline` and returns `(mem << 20) + 0x80000000`; with the stock
`mem=480M` that is exactly `0x9E000000`. On this port there is no `mem=`
parameter — the DTS reserves the two 16 MiB regions with `no-map`, which is what
leaves the kernel 480 MiB of usable DRAM (`MemTotal` ≈ 466 MiB after the
kernel's own use).

Putting the window at `0x9F000000` instead — which this board's DTS originally
did — means the BIOS pushes into the framebuffer and the VGA engine repaints it
with screen (or blank-screen zero) data before the BMC can read it.

```dts
reserved-memory {
        vga_memory: framebuffer@9f000000 {
                no-map;
                reg = <0x9f000000 0x01000000>;
        };

        hostif_memory: buffer@9e000000 {
                no-map;
                reg = <0x9e000000 0x01000000>;
        };
};

&lpc_ctrl {
        status = "okay";
        memory-region = <&hostif_memory>;
};
```

`&lpc_ctrl` is what turns the reservation into a userspace mapping: the
`aspeed-lpc-ctrl` driver maps the region with `pgprot_noncached`, and the probe
reads it through `/dev/aspeed-lpc-ctrl` (major 10, minor 258) with a single
read-only `mmap`.

> **`/dev/mem` is not used for the window.** It is only used for one 0x200-byte
> SCU register window (§3.2), which is not RAM and therefore passes Linux's
> strict `/dev/mem` check. `CONFIG_STRICT_DEVMEM=y` in
> `mc12-le0.cfg`. Note the *currently flashed* BMC kernel predates that change
> (`/proc/config.gz` shows it unset) — after flashing, re-confirm the SCU writes
> still take effect, which the capture log reports on every op.

The probe captures at most 1 MiB per push (stock clamps the same way), and
snapshots the first 64 KiB of the window at *every* netfn-`0x2E` op, so a
capture shows whether the host's write arrives before, with, or after the
announcement.

### 3.2 Opening the write path (the SCU handshake)

The host's write path has a gate that stock toggles in `endis_share_memory()`.
`ami-oem-probe` runs the same sequence verbatim (`endisShareMemory()`), at
provider load and again at every `0x0A`/`0x50`/`0x24`:

```
open /dev/mem, mmap 0x200 at SCU 0x1E6E2000
SCU00  (0x1E6E2000) = 0x1688A8A8        write the unlock key
platform id = u16 at SCU + 0x66
  if enable && (pid == 0x139 || pid == 0x13B)   SCU40 |= 0x1000
  if !enable   { SCU180 &= ~0x2;  SCU40 |= 0x1000 }
  else         { SCU180 |=  0x2;  if pid not special  SCU40 &= ~0x1000 }
SCU00 = 0                               re-lock
```

* `SCU180` bit 1 is `SCU180_ENP2A`.
* `SCU40` bit 12 is a companion bit stock only raises for the two special
  platform ids above. This board reads `pid = 0x0000`, so it is the *non*
  special branch: enable sets `SCU180 |= 2` and clears `SCU40` bit 12.

Observed on this board:

```
SCU enable  op=0x24 pid=0000 special=0 2c=00200400 40=000000C1 180=000C007B
SCU disable op=0x24 pid=0000 special=0 2c=00200400 40=000010C1 180=000C0079
```

Two traps that an earlier revision fell into: writing `SCU180`/`SCU40` without
first writing the `SCU00` unlock key (the writes are simply discarded), and
setting both bits unconditionally (wrong side of stock's platform-conditional
branch).

`SCU2C` (the per-region gates; DRAM is bit 25) comes up open from u-boot and is
left alone. `aspeed-p2a-ctrl` is deliberately **not** bound — its probe sets
every region bit and blocks the host's VGA BAR.

---

## 4. The bridge into smbios-mdr

This is the part that turns a raw push into OpenBMC inventory.

### 4.1 What the host actually pushes

A **32-bit `_SM_` entry point followed by the structure table**, in the window.
The captured set begins:

```
5F 53 4D 5F  E2  1F  03 03   ...      "_SM_", checksum 0xE2, EP length 0x1F,
                                                    SMBIOS version 3.3
```

which is a 31-byte (`0x1F`) 32-bit entry point, so the table starts at offset
`0x1F`, with its length at EP offset `0x16`.

### 4.2 Why smbios-mdr cannot eat it directly

`smbios-mdr`'s `smbiosSkipEntryPoint()` only steps over the 64-bit `_SM3_`
entry point, and `checkSMBIOSVersion()` insists on finding one of the two
anchors first. Handed a `_SM_` set, it walks the entry point as though it were a
structure and yields nothing.

### 4.3 The re-emission

`publishSmbiosToInventory()` (`main.cpp`) rebuilds the blob:

1. **Find the entry point.** `_SM3_` first (it is never a substring of `_SM_`),
   then `_SM_`.
2. **Locate the table.** 64-bit form: table address is a u64 at EP `0x10`,
   version at `0x07`/`0x08`. 32-bit form: EP length at `0x05`, version at
   `0x06`/`0x07`, table length a u16 at `0x16`, table right after the entry
   point.
3. **Synthesise a 24-byte `_SM3_` entry point** whose structure-table address is
   the table's own offset inside the same buffer (`0x18`). The parser treats any
   address below its 32 KiB storage bound as an offset, which is what makes this
   work. Version is copied through, or forced to `3.3` when the host reported
   less (the parser accepts 3.0–3.8 and the structure table is
   version-independent for the types it reads). Max table size is set to
   `0x80000000`.
4. **Fix the entry point checksum** so the 24 bytes sum to zero mod 256.
5. **Write the transport file** and poke the daemon.

The synthetic entry point, field by field:

| Offset | Bytes | Value |
|---|---|---|
| `0x00` | 5 | `_SM3_` |
| `0x05` | 1 | checksum, `0 - sum(other 23 bytes)` |
| `0x06` | 1 | entry point length, `0x18` |
| `0x07` / `0x08` | 1+1 | major / minor (copied, or `3.3`) |
| `0x09` | 1 | doc revision `0x01` |
| `0x0A` | 1 | entry point revision `0x01` |
| `0x0C` | 4 | structure table max size, `0x80000000` |
| `0x10` | 8 | structure table address = `0x18` (an offset, not a hardware address) |

### 4.4 The transport file

`/var/lib/smbios/smbios2`, the same file and layout `smbios-mdr`'s own IPMI blob
handler (`smbiosstore`) writes:

| Field | Size | Value |
|---|---|---|
| `dirVer` | 1 | `1` |
| `mdrType` | 1 | `2` (Type II) |
| `timestamp` | 4 | `time(nullptr)`, LE |
| `dataSize` | 4 | size of the data that follows, LE |
| data | — | the synthetic `_SM3_` entry point + structure table |

For the captured table that is `10 + 24 + 3431 = 3465` bytes.

Then the daemon is told to re-read it:

```
bus.call(new_method_call("xyz.openbmc_project.Smbios.MDR_V2",
                         "/xyz/openbmc_project/Smbios/MDR_V2",
                         "xyz.openbmc_project.Smbios.MDR_V2",
                         "AgentSynchronizeData"))
```

with a 5 s timeout — a wedged daemon must never hold the IPMI request path open.

Both steps are best-effort: the reply the BIOS is waiting for must not depend on
the inventory daemon. The capture records what happened:

```
SMBIOS-BRIDGE op=0x0B ep=0x000000 table=3431 data=3455 wrote=1 synced=1
```

and `smbios-mdr` then logs

```
SMBIOS VERSION - 3.3
VERSION INFO - BIOS - F20
```

---

## 5. Where the data lands

### 5.1 smbios-mdr

`smbiosmdrv2app` re-parses `/var/lib/smbios/smbios2` and owns
`xyz.openbmc_project.Smbios.MDR_V2`:

```
/xyz/openbmc_project/inventory/system/chassis/motherboard/
├── bios        Common.UUID + Inventory.Decorator.Revision.Version ("F20")
├── cpu0        Asset(Manufacturer, PartNumber, SerialNumber) + Cpu.{Id,
│               MaxSpeedInMhz, CoreCount, ThreadCount, Socket, Family,
│               EffectiveFamily, EffectiveModel, Step, Microcode} +
│               Revision.Version (the brand string) + LocationCode
└── pcieslot0 … pcieslot6
                LocationCode ("PCIEX4_A", "M2M", …) + PCIeSlot.{Lanes,
                Generation, SlotType, HotPluggable} + Present
```

`System::version` also pushes the BIOS version into the software inventory
object `/xyz/openbmc_project/software/bios_active`
(`xyz.openbmc_project.Software.Version.Version`).

It is built with `PACKAGECONFIG:append = " smbios-ipmi-blob smbios-no-dimm "`
(`smbios-mdr_%.bbappend`), i.e. `-Dsmbios-ipmi-blob=enabled -Ddimm-dbus=disabled`.
The blob option stays on because it is the correct door for a BIOS that speaks
blobs and costs nothing; the DIMM option is off because DIMM inventory comes from
the SPDs (see §6) and two publishers would show every slot twice in Redfish.

### 5.2 What Redfish shows

| Redfish | Source |
|---|---|
| `/Systems/system` `ProcessorSummary`, `MemorySummary`, `PowerState` | aggregation over the objects above |
| `/Systems/system/Processors/cpu0` | `cpu0` |
| `/Systems/system/Memory/…` | the DIMM objects from §6 |
| `/Systems/system/Bios` | a static resource; the active image link points at `bios_active` |

Known gaps, all understood and none of them a transport problem:

* `Manufacturer` / `Model` / `SerialNumber` / `SKU` on `/Systems/system` and
  `/Chassis/…` are **not** from SMBIOS. They come from entity-manager and the
  FRU, and this port has no `xyz.openbmc_project.Inventory.Item.System` object
  (only entity-manager's `Inventory.Item.Board` at
  `/inventory/system/board/Gigabyte_MC12_LE0_00`). `smbios-mdr`'s `System` class
  only ever publishes a UUID and a version.
* `BiosVersion` needs SMBIOS **Type 45**, which this BIOS does not emit. The
  version `F20` is still on D-Bus and in `FirmwareInventory/bios_active`.
* `cpu0.Model` is empty and its `PartNumber`/`SerialNumber` read `"Unknown"`:
  the BIOS fills Type 4's optional strings that way, and `smbios-mdr` never maps
  the brand string into `Asset.Model` (it exposes it as `Revision.Version`, which
  Redfish renders as `Processor.Version`).
* Type 43 (TPM 2.0) *is* in the table but is not published — `smbios-mdr`'s
  `tpm-dbus` option is off.
* Because there is no `Inventory.Item.System` anchor, the daemon logs
  `Failed to get system motherboard dbus path. Setting up a match rule` at every
  sync and anchors at `/inventory/system` instead. Redfish is unaffected (it
  finds the objects through the mapper); the only visible effect is that
  `cpu0` / `bios` / `pcieslot*` carry an empty `Associations` property.
* The 7 PCIe slot objects exist on D-Bus but are not surfaced anywhere:
  `pcie_slots.hpp`'s `requestRoutesPCIeSlots()` is never called in this
  `bmcweb`, so there is no `/redfish/v1/Chassis/<id>/PCIeSlots` route at all
  (`/Systems/system/PCIeSlots` is a stale URI and 404s too), and
  `/Systems/system/PCIeDevices` exists but is empty because SMBIOS reports
  *slots*, never *devices*. The shipped `webui-vue` (8538dca185) has no PCIe
  page either — its Hardware Status section is Inventory + Sensors only, and
  "pcie" appears nowhere in the bundle. Seeing these in a UI would take a
  `bmcweb` patch, chassis associations on the slot objects, and a UI patch, for
  data that is already readable on D-Bus.

---

## 6. DIMMs: the SPD + SMBIOS hybrid

DIMM inventory has two possible sources and each is wrong on its own here:

| Source | Gets right | Gets wrong |
|---|---|---|
| Type 17 via `smbios-mdr` | silkscreen locator, trained speed | `BaseModuleType` (these UDIMMs read as RDIMM), `Manufacturer: Unknown`, `SerialNumber: 00000000`, and `Health: Critical` + `DataWidthBits: 65535` on empty slots |
| SPD EEPROMs | manufacturer, serial, module type, rank, width, ECC, capacity | nothing about the slot name or the trained speed |

So `dimm-inventory` (own daemon, `xyz.openbmc_project.DimmInventory`) owns the
objects and reads the SPDs, and it *borrows* exactly the two fields SMBIOS has
and the SPD does not.

**SPD side.** `ee1004` EEPROMs at `0x50`–`0x53` on i2c-5 (DTS
`dimma0spd@50`…`dimmb1spd@53`). `spd.cpp` decodes DDR4 SPD and gates on the
JEDEC CRC16, so an unpopulated slot or a bad read produces no inventory entry
rather than a bogus one. Devices are enumerated from
`/sys/bus/i2c/devices` and sorted by `(bus, address)`, so pairing is
deterministic across restarts.

**SMBIOS side.** `smbios.cpp` reads the *same* `/var/lib/smbios/smbios2` and
returns the Type 17 entries. Fields used:

| Type 17 offset | Use |
|---|---|
| `0x0C` (u16) | size in MiB; `0x8000` bit set → value is KB, `/1024`; `0xFFFF` → extended u32 at `0x1C` (MiB). Size 0 = empty slot, skipped |
| `0x10` | device locator string index → `"DIMM_P0_A1"` |
| `0x11` | bank locator string index |
| `0x1A` | part number string index |
| `0x20` | configured (trained) speed in MHz → `OperatingSpeedMhz` |

Note `0x15` ("Speed") is **not** used: on this board it reports `2133`, the JEDEC
fallback, while `0x20` reports the trained `3600`.

The structure walk is the subtle part: the fixed part is followed by a string
set, and the set ends **at** the first double NUL — each string carries its own
lone terminator, so a structure with no strings is simply two NULs and the next
structure begins at `strEnd + 2`.

**Matching.** `matchMemoryDevice()` claims at most one Type 17 entry per SPD:
part number first (case-insensitive, and only when both sides have one), then
capacity alone, in BIOS emission order versus SPD `(bus, address)` order. Both
orders are bank order, which matters because the two DIMMs here are identical.

**Why it republishes instead of updating properties.**
`dimm-inventory` starts before the host POSTs, and a BMC reboot does not imply a
host POST, so it publishes what the SPDs alone can tell it, then polls
`/var/lib/smbios/smbios2`'s mtime every 10 s and rebuilds every object when it
changes (remove + re-add: a property cannot be *added* to an interface after
`initialize()`, and the property set differs between enriched and not).

Resulting Redfish per DIMM: `BaseModuleType UDIMM`, `BusWidthBits 64`,
`CapacityMiB 16384`, `DataWidthBits 64`, `ErrorCorrection NoECC`,
`Location.PartLocation.ServiceLabel DIMM_P0_A1` / `DIMM_P0_B1`,
`Manufacturer Corsair`, `MemoryDeviceType DDR4`, `OperatingSpeedMhz 3600`,
`PartNumber CMK32GX4M2D3600C18`, `RankCount 1`, `Status OK/Enabled`.

---

## 7. BMC firmware version (the "N/A" fix)

Separate from SMBIOS, but on the same IPMI path: the BIOS's "BIOS Information"
page takes "BMC Firmware Version" from the App netfn `0x06` cmd `0x01`
**Get Device ID** payload, and ipmid's own App handlers log nothing — a wrong or
mistimed answer is invisible from the BMC.

`handleGetDeviceIdShadow()` registers `(0x06, 0x01)` at `prioOemBase`, which
supplants the core handler on every channel including KCS, so every reply lands
in the capture log. It re-reads the same
`/usr/share/ipmi-providers/dev_id.json` the core handler uses, and differs in one
way: it skips the live D-Bus "BMC ready" check and serves the file's
`availability` fallback. When the state manager is Ready — the steady state once
the boot-completion issue was fixed — the two replies are byte-identical.

With `id` `0x20`, revision `1`, and firmware minor `0x40` (BCD 1.40) the BIOS
displays **1.40** instead of `N/A`:

```
netfn=0x06 cmd=0x01 reply=[20 01 01 40 02 ...]
```

---

## 8. Instrumentation: `/tmp/ami-probe.log`

journald on an AST2500 is slow and lossy under a bursty POST, and a full-journal
grep times out. So every event is appended to a plain file, one line each,
timestamped `YYYY-MM-DDTHH:MM:SS.mmm`:

| Line prefix | Contents |
|---|---|
| `FILTER` | every IPMI command on every channel, including ones the provider does not claim (App `0x06/0x04`, Storage `0x0A`, KCS whitelist rejects) |
| `netfn=0x2e` | full request: IANA, cmd, label, channel, privilege, length, body hex |
| `netfn=0x32` | ditto, plus a `reply=[…]` line per answered command |
| `netfn=0x06` | the Get Device ID shadow's reply bytes |
| `SCU-state` / `SCU enable` / `SCU disable` | the gate state the BIOS saw at each op |
| `WINDOW-SCAN` | first 64 KiB of the window: non-zero byte count and any `_SM3_`/`_SM_`/`_DMI_` anchor offsets |
| `WINDOW-CAPTURE` | a push was latched, to which path and how many bytes |
| `SMBIOS-BRIDGE` | entry point offset, table length, file size, `wrote=`, `synced=` |

Useful one-liners:

```sh
grep SMBIOS-BRIDGE /tmp/ami-probe.log          # did a push reach the inventory?
grep WINDOW-SCAN    /tmp/ami-probe.log | tail  # is the host writing at all?
grep "netfn=0x2e"   /tmp/ami-probe.log         # the whole OEM conversation
grep FILTER         /tmp/ami-probe.log         # every command, claimed or not
```

Captures land next to it: `/tmp/ami-smbios.bin` (the pushed SMBIOS set) and
`/tmp/ami-redfish.bin` (an `0x52` Redfish dataset). Both are cleared on reboot —
`/` is an overlay whose writable layer is in RAM.

---

## 9. Verifying a host POST end to end

After a host power-cycle (not a BMC reboot), on the BMC:

```sh
# 1. Did the host push, and did the bridge take it?
grep -E "WINDOW-CAPTURE|SMBIOS-BRIDGE" /tmp/ami-probe.log | tail

# 2. Did smbios-mdr parse it?
journalctl -u smbios-mdrv2.service | tail
ls -l /var/lib/smbios/smbios2

# 3. Is the inventory there?
busctl tree xyz.openbmc_project.Smbios.MDR_V2
busctl tree xyz.openbmc_project.DimmInventory

# 4. And through Redfish?
curl -sk -u root:0penBmc https://<bmc>/redfish/v1/Systems/system
curl -sk -u root:0penBmc https://<bmc>/redfish/v1/Systems/system/Processors/cpu0
curl -sk -u root:0penBmc https://<bmc>/redfish/v1/Systems/system/Memory
```

An all-zero `WINDOW-CAPTURE` with `anchors=none` in the following `WINDOW-SCAN`
means the host wrote somewhere else — check the op-`0x0A` reply first (§2.1); that
was the cause the one time it happened.

---

## 10. Limitations and things deliberately not done

* The provider **never writes BMC memory or flash**, apart from the SCU
  register window in §3.2. Everything else it does with a request is log it and
  answer synthetically.
* Op `0x52` (post a Redfish request, e.g. `Systems/Self/Bios`) is captured to
  `/tmp/ami-redfish.bin` but **not acted on** — no local Redfish request is
  issued. The observed bodies carry a u32 length of 1 alongside a full path
  string, so the capture floors the read at 32 KiB rather than trusting it.
* Unknown netfn-`0x2E` commands are echoed, unknown netfn-`0x32` commands answer
  `ccInvalidCommand`. Both are what the BIOS already handles when nothing is
  registered.
* `0x5D` hands out the bench root account (§2.2).
* The provider cannot be removed: with AMI's stack absent, it is the only thing
  that gets this host's SMBIOS tables to `smbios-mdr`. Despite the name, it is
  the port's implementation, not an observer.
* Bring-up notes that are *not* protocol: the host-PID `hidg0` USB-vhub quirk on
  the KVM gadget, and the LAN IPMI cipher-suite issue, are unrelated to this
  path and are not covered here.
