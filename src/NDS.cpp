/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <assert.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <string>
#include "NDS.h"
#include "NSMLGameRAMRollback.h"
#include "ARM.h"
#include "NDSCart.h"
#include "GBACart.h"
#include "DMA.h"
#include "FIFO.h"
#include "GPU.h"
#include "SPU.h"
#include "SPI.h"
#include "RTC.h"
#include "Wifi.h"
#include "AREngine.h"
#include "Platform.h"
#include "FreeBIOS.h"
#include "Args.h"
#include "version.h"

#include "DSi.h"
#include "DSi_SPI_TSC.h"
#include "DSi_NWifi.h"
#include "DSi_Camera.h"
#include "DSi_DSP.h"
#include "ARMJIT.h"
#include "ARMJIT_Memory.h"

namespace melonDS
{
using namespace Platform;

const s32 kMaxIterationCycles = 64;
const s32 kIterationCycleMargin = 8;

static void TraceNSMLIPC9Send(NDS& nds, u32 value)
{
    struct Config
    {
        bool Checked = false;
        FILE* File = nullptr;
    };
    static Config cfg;
    if (!cfg.Checked)
    {
        cfg.Checked = true;
        if (const char* path = getenv("MELONDS_NSML_IPC9_SEND_LOG"))
        {
            cfg.File = fopen(path, "w");
            if (cfg.File)
            {
                fprintf(cfg.File,
                    "frame,gameFrame,historyEnabled,historyIndex,historyCount,historyTarget,"
                    "arm9Pc,value,tag,error,payload,"
                    "word0,word1,word2,word3,word4,word5,word6,word7\n");
            }
        }
    }
    if (!cfg.File)
        return;

    constexpr u32 historyEnabledAddr = 0x02001ACC;
    constexpr u32 historyIndexAddr = 0x02001AD0;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyTargetAddr = 0x02001AD8;
    constexpr u32 gameFrameAddr = 0x0208B668;
    const u32 payload = value >> 6;
    std::array<u32, 8> words {};
    if (payload >= 0x02000000 && payload <= 0x023FFFE0)
    {
        for (u32 index = 0; index < words.size(); index++)
            words[index] = nds.ARM9Read32(payload + index * sizeof(u32));
    }
    fprintf(cfg.File,
        "%u,%u,%u,%u,%u,%u,%08X,%08X,%u,%u,%08X,"
        "%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
        nds.NumFrames,
        nds.ARM9Read32(gameFrameAddr),
        nds.ARM9Read32(historyEnabledAddr),
        nds.ARM9Read32(historyIndexAddr),
        nds.ARM9Read32(historyCountAddr),
        nds.ARM9Read32(historyTargetAddr),
        nds.ARM9.R[15], value, value & 0x1F, (value >> 5) & 0x1, payload,
        words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
}

static void TraceNSMLSoundCommandList(NDS& nds, u32 value)
{
    if ((value & 0x1F) != 7)
        return;

    struct Config
    {
        bool Checked = false;
        FILE* File = nullptr;
    };
    static Config cfg;
    if (!cfg.Checked)
    {
        cfg.Checked = true;
        if (const char* path = getenv("MELONDS_NSML_SND_COMMAND_LOG"))
        {
            cfg.File = fopen(path, "w");
            if (cfg.File)
            {
                fprintf(cfg.File,
                    "frame,gameFrame,historyEnabled,historyIndex,historyCount,historyTarget,"
                    "listAddress,commandIndex,commandAddress,id,arg0,arg1,arg2,arg3,rollbackTransaction\n");
            }
        }
    }
    if (!cfg.File)
        return;

    constexpr u32 historyEnabledAddr = 0x02001ACC;
    constexpr u32 historyIndexAddr = 0x02001AD0;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyTargetAddr = 0x02001AD8;
    constexpr u32 gameFrameAddr = 0x0208B668;
    constexpr u32 commandPoolStart = 0x020948E0;
    constexpr u32 commandSize = 24;
    constexpr u32 commandCapacity = 256;
    constexpr u32 commandPoolEnd = commandPoolStart + commandSize * commandCapacity;

    const u32 listAddress = value >> 6;
    u32 command = listAddress;
    u32 index = 0;
    while (command >= commandPoolStart && command < commandPoolEnd &&
        (command - commandPoolStart) % commandSize == 0 && index < commandCapacity)
    {
        const u32 next = nds.ARM9Read32(command);
        fprintf(cfg.File,
            "%u,%u,%u,%u,%u,%u,%08X,%u,%08X,%u,%08X,%08X,%08X,%08X,%u\n",
            nds.NumFrames,
            nds.ARM9Read32(gameFrameAddr),
            nds.ARM9Read32(historyEnabledAddr),
            nds.ARM9Read32(historyIndexAddr),
            nds.ARM9Read32(historyCountAddr),
            nds.ARM9Read32(historyTargetAddr),
            listAddress, index, command, nds.ARM9Read32(command + 4),
            nds.ARM9Read32(command + 8), nds.ARM9Read32(command + 12),
            nds.ARM9Read32(command + 16), nds.ARM9Read32(command + 20),
            nds.IsNSMLGameRAMRollbackTransactionInFlight() ? 1u : 0u);
        index++;
        if (next == 0)
            break;
        if (next == command || next < commandPoolStart || next >= commandPoolEnd ||
            (next - commandPoolStart) % commandSize != 0)
            break;
        command = next;
    }
}

static void TraceNSMLIPC7Send(NDS& nds, u32 value)
{
    struct Config
    {
        bool Checked = false;
        FILE* File = nullptr;
    };
    static Config cfg;
    if (!cfg.Checked)
    {
        cfg.Checked = true;
        if (const char* path = getenv("MELONDS_NSML_IPC7_SEND_LOG"))
        {
            cfg.File = fopen(path, "w");
            if (cfg.File)
            {
                fprintf(cfg.File,
                    "frame,gameFrame,historyEnabled,historyIndex,historyCount,historyTarget,"
                    "arm7Pc,value,tag,error,payload\n");
            }
        }
    }
    if (!cfg.File)
        return;

    constexpr u32 historyEnabledAddr = 0x02001ACC;
    constexpr u32 historyIndexAddr = 0x02001AD0;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyTargetAddr = 0x02001AD8;
    constexpr u32 gameFrameAddr = 0x0208B668;
    fprintf(cfg.File, "%u,%u,%u,%u,%u,%u,%08X,%08X,%u,%u,%08X\n",
        nds.NumFrames,
        nds.ARM9Read32(gameFrameAddr),
        nds.ARM9Read32(historyEnabledAddr),
        nds.ARM9Read32(historyIndexAddr),
        nds.ARM9Read32(historyCountAddr),
        nds.ARM9Read32(historyTargetAddr),
        nds.ARM7.R[15], value, value & 0x1F, (value >> 5) & 0x1, value >> 6);
}

bool TraceNSMLWatchWrite(NDS* nds, const char* cpu, u32 pc, u32 addr, u32 width, u32 val)
{
    struct WatchConfig
    {
        bool Checked = false;
        bool Enabled = false;
        u32 Addr = 0;
        u32 Len = 4;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
    };

    static WatchConfig cfg;
    if (!cfg.Checked)
    {
        cfg.Checked = true;
        if (const char* watchAddr = getenv("MELONDS_NSML_WATCH_ADDR"))
        {
            char* end = nullptr;
            cfg.Addr = static_cast<u32>(strtoul(watchAddr, &end, 0));
            cfg.Enabled = end && *end == '\0';
        }
        if (const char* watchLen = getenv("MELONDS_NSML_WATCH_LEN"))
            cfg.Len = static_cast<u32>(strtoul(watchLen, nullptr, 0));
        if (cfg.Len == 0) cfg.Len = 4;
        if (const char* startFrame = getenv("MELONDS_NSML_WATCH_START_FRAME"))
            cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
        if (const char* endFrame = getenv("MELONDS_NSML_WATCH_END_FRAME"))
            cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
    }

    if (!cfg.Enabled || !nds) return false;
    if (nds->NumFrames < cfg.StartFrame || nds->NumFrames > cfg.EndFrame) return false;

    const u32 writeOffset = addr & nds->MainRAMMask;
    const u32 watchOffset = cfg.Addr & nds->MainRAMMask;
    const u32 writeEnd = writeOffset + width - 1;
    const u32 watchEnd = watchOffset + cfg.Len - 1;
    if (writeEnd < watchOffset || writeOffset > watchEnd) return false;

    const u32 lr = !strcmp(cpu, "ARM9") ? nds->ARM9.R[14] : nds->ARM7.R[14];
    printf("NSMB Watch: nds=%p frame=%u cpu=%s pc=%08X lr=%08X addr=%08X offset=%06X width=%u val=%08X\n",
        static_cast<void*>(nds),
        nds->NumFrames,
        cpu,
        pc,
        lr,
        addr,
        writeOffset,
        width,
        val);
    return true;
}

static bool NSMLWatchWriteMaybeEnabled()
{
#ifdef NSML_CORE_RUNTIME_HOOKS_DISABLED
    return false;
#else
    static const bool enabled = getenv("MELONDS_NSML_WATCH_ADDR") != nullptr;
    return enabled;
#endif
}

static bool NSMLRollbackSkipJITReset()
{
    static const bool enabled = getenv("MELONDS_NSML_ROLLBACK_SKIP_JIT_RESET") != nullptr;
    return enabled;
}

// timing notes
//
// * this implementation is technically wrong for VRAM
//   each bank is considered a separate region
//   but this would only matter in specific VRAM->VRAM DMA transfers or
//   when running code in VRAM, which is way unlikely
//
// bus/basedelay/nspenalty
//
// bus types:
// * 0 / 32-bit: nothing special
// * 1 / 16-bit: 32-bit accesses split into two 16-bit accesses, second is always sequential
// * 2 / 8-bit/GBARAM: (presumably) split into multiple 8-bit accesses?
// * 3 / ARM9 internal: cache/TCM
//
// ARM9 always gets 3c nonseq penalty when using the bus (except for mainRAM where the penalty is 7c)
// /!\ 3c penalty doesn't apply to DMA!
//
// ARM7 only gets nonseq penalty when accessing mainRAM (7c as for ARM9)
//
// timings for GBA slot and wifi are set up at runtime

thread_local NDS* NDS::Current = nullptr;

NDS::NDS() noexcept :
    NDS(
        NDSArgs {
            std::make_unique<ARM9BIOSImage>(FreeBIOSGetNtrArm9()),
            std::make_unique<ARM7BIOSImage>(FreeBIOSGetNtrArm7()),
            Firmware(0),
        }
    )
{
}

NDS::NDS(NDSArgs&& args, int type, void* userdata) noexcept :
    ConsoleType(type),
    UserData(userdata),
    ARM7BIOS(*args.ARM7BIOS),
    ARM9BIOS(*args.ARM9BIOS),
    ARM7BIOSNative(CRC32(ARM7BIOS.data(), ARM7BIOS.size()) == ARM7BIOSCRC32),
    ARM9BIOSNative(CRC32(ARM9BIOS.data(), ARM9BIOS.size()) == ARM9BIOSCRC32),
    JIT(*this, args.JIT),
    SPU(*this, args.BitDepth, args.Interpolation, args.OutputSampleRate),
    Mic(*this),
    GPU(*this, std::move(args.Renderer)),
    SPI(*this, std::move(args.Firmware)),
    RTC(*this),
    Wifi(*this),
    NDSCartSlot(*this, 0, nullptr),
    GBACartSlot(*this, nullptr),
    AREngine(*this),
    ARM9(*this, args.GDB, args.JIT.has_value()),
    ARM7(*this, args.GDB, args.JIT.has_value()),
#ifdef GDBSTUB_ENABLED
    EnableGDBStub(args.GDB.has_value()),
#endif
#ifdef JIT_ENABLED
    EnableJIT(args.JIT.has_value()),
#endif
    DMAs {
        DMA(0, 0, *this),
        DMA(0, 1, *this),
        DMA(0, 2, *this),
        DMA(0, 3, *this),
        DMA(1, 0, *this),
        DMA(1, 1, *this),
        DMA(1, 2, *this),
        DMA(1, 3, *this),
    }
{
    NDSCartSlots[0] = &NDSCartSlot;
    NDSCartSlots[1] = nullptr;

    RegisterEventFuncs(Event_Div, this, {MakeEventThunk(NDS, DivDone)});
    RegisterEventFuncs(Event_Sqrt, this, {MakeEventThunk(NDS, SqrtDone)});

    MainRAM = JIT.Memory.GetMainRAM();
    SharedWRAM = JIT.Memory.GetSharedWRAM();
    ARM7WRAM = JIT.Memory.GetARM7WRAM();
}

NDS::~NDS() noexcept
{
    UnregisterEventFuncs(Event_Div);
    UnregisterEventFuncs(Event_Sqrt);
    // The destructor for each component is automatically called by the compiler
}


void NDS::SetARM9RegionTimings(u32 addrstart, u32 addrend, u32 region, int buswidth, int nonseq, int seq)
{
    addrstart >>= 2;
    addrend   >>= 2;

    int N16, S16, N32, S32, cpuN;
    N16 = nonseq;
    S16 = seq;
    if (buswidth == 16)
    {
        N32 = N16 + S16;
        S32 = S16 + S16;
    }
    else
    {
        N32 = N16;
        S32 = S16;
    }

    // nonseq accesses on the CPU get a 3-cycle penalty for all regions except main RAM
    cpuN = (region == Mem9_MainRAM) ? 0 : 3;

    for (u32 i = addrstart; i < addrend; i++)
    {
        // CPU timings
        ARM9MemTimings[i][0] = N16 + cpuN;
        ARM9MemTimings[i][1] = S16;
        ARM9MemTimings[i][2] = N32 + cpuN;
        ARM9MemTimings[i][3] = S32;

        // DMA timings
        ARM9MemTimings[i][4] = N16;
        ARM9MemTimings[i][5] = S16;
        ARM9MemTimings[i][6] = N32;
        ARM9MemTimings[i][7] = S32;

        ARM9Regions[i] = region;
    }

    ARM9.UpdateRegionTimings(addrstart<<2, addrend<<2);
}

void NDS::SetARM7RegionTimings(u32 addrstart, u32 addrend, u32 region, int buswidth, int nonseq, int seq)
{
    addrstart >>= 3;
    addrend   >>= 3;

    int N16, S16, N32, S32;
    N16 = nonseq;
    S16 = seq;
    if (buswidth == 16)
    {
        N32 = N16 + S16;
        S32 = S16 + S16;
    }
    else
    {
        N32 = N16;
        S32 = S16;
    }

    for (u32 i = addrstart; i < addrend; i++)
    {
        // CPU and DMA timings are the same
        ARM7MemTimings[i][0] = N16;
        ARM7MemTimings[i][1] = S16;
        ARM7MemTimings[i][2] = N32;
        ARM7MemTimings[i][3] = S32;

        ARM7Regions[i] = region;
    }
}

#ifdef JIT_ENABLED
void NDS::SetJITArgs(std::optional<JITArgs> args) noexcept
{
    if (args)
    { // If we want to turn the JIT on...
        JIT.SetJITArgs(*args);
    }
    else if (args.has_value() != EnableJIT)
    { // Else if we want to turn the JIT off, and it wasn't already off...
        JIT.Reset();
    }

    EnableJIT = args.has_value();
}
#endif

#ifdef GDBSTUB_ENABLED
void NDS::SetGdbArgs(std::optional<GDBArgs> args) noexcept
{
    ARM9.SetGdbArgs(args);
    ARM7.SetGdbArgs(args);
    EnableGDBStub = args.has_value();
}
#endif

void NDS::InitTimings()
{
    // TODO, eventually:
    // VRAM is initially unmapped. The timings should be those of void regions.
    // Similarly for any unmapped VRAM area.
    // Need to check whether supporting these timing characteristics would impact performance
    // (especially wrt VRAM mirroring and overlapping and whatnot).
    // Also, each VRAM bank is its own memory region. This would matter when DMAing from a VRAM
    // bank to another (if this is a thing) for example.

    // TODO: check in detail how WRAM works, although it seems to be one region.

    // TODO: DSi-specific timings!!

    SetARM9RegionTimings(0x00000, 0x100000, 0, 32, 1, 1); // void

    SetARM9RegionTimings(0xFFFF0, 0x100000, Mem9_BIOS,    32, 1, 1); // BIOS
    SetARM9RegionTimings(0x02000, 0x03000,  Mem9_MainRAM, 16, 8, 1);     // main RAM
    SetARM9RegionTimings(0x03000, 0x04000,  Mem9_WRAM,    32, 1, 1); // ARM9/shared WRAM
    SetARM9RegionTimings(0x04000, 0x05000,  Mem9_IO,      32, 1, 1); // IO
    SetARM9RegionTimings(0x05000, 0x06000,  Mem9_Pal,     16, 1, 1); // palette
    SetARM9RegionTimings(0x06000, 0x07000,  Mem9_VRAM,    16, 1, 1); // VRAM
    SetARM9RegionTimings(0x07000, 0x08000,  Mem9_OAM,     32, 1, 1); // OAM

    // ARM7

    SetARM7RegionTimings(0x00000, 0x100000, 0, 32, 1, 1); // void

    SetARM7RegionTimings(0x00000, 0x00010, Mem7_BIOS,    32, 1, 1); // BIOS
    SetARM7RegionTimings(0x02000, 0x03000, Mem7_MainRAM, 16, 8, 1); // main RAM
    SetARM7RegionTimings(0x03000, 0x04000, Mem7_WRAM,    32, 1, 1); // ARM7/shared WRAM
    SetARM7RegionTimings(0x04000, 0x04800, Mem7_IO,      32, 1, 1); // IO
    SetARM7RegionTimings(0x06000, 0x07000, Mem7_VRAM,    16, 1, 1); // ARM7 VRAM

    // handled later: GBA slot, wifi
}

bool NDS::NeedsDirectBoot() const
{
    // DSi/3DS firmwares aren't bootable, neither is the generated firmware
    if (!SPI.GetFirmware().IsBootable())
        return true;

    // FreeBIOS requires direct boot (it can't boot firmware)
    if (!IsLoadedARM9BIOSKnownNative() || !IsLoadedARM7BIOSKnownNative())
        return true;

    return false;
}

void NDS::SetupDirectBoot()
{
    const NDSHeader& header = NDSCartSlot.GetCart()->GetHeader();
    u32 cartid = NDSCartSlot.GetCart()->ID();
    const u8* cartrom = NDSCartSlot.GetCart()->GetROM();
    MapSharedWRAM(3);

    // Copy the Nintendo logo from the NDS ROM header to the ARM9 BIOS if using FreeBIOS
    // Games need this for DS<->GBA comm to work
    if (!IsLoadedARM9BIOSKnownNative())
    {
        memcpy(ARM9BIOS.data() + 0x20, header.NintendoLogo, 0x9C);
    }

    // setup main RAM data

    for (u32 i = 0; i < 0x170; i+=4)
    {
        u32 tmp = *(u32*)&cartrom[i];
        NDS::ARM9Write32(0x027FFE00+i, tmp);
    }

    NDS::ARM9Write32(0x027FF800, cartid);
    NDS::ARM9Write32(0x027FF804, cartid);
    NDS::ARM9Write16(0x027FF808, header.HeaderCRC16);
    NDS::ARM9Write16(0x027FF80A, header.SecureAreaCRC16);

    NDS::ARM9Write16(0x027FF850, 0x5835);

    NDS::ARM9Write32(0x027FFC00, cartid);
    NDS::ARM9Write32(0x027FFC04, cartid);
    NDS::ARM9Write16(0x027FFC08, header.HeaderCRC16);
    NDS::ARM9Write16(0x027FFC0A, header.SecureAreaCRC16);

    NDS::ARM9Write16(0x027FFC10, 0x5835);
    NDS::ARM9Write16(0x027FFC30, 0xFFFF);
    NDS::ARM9Write16(0x027FFC40, 0x0001);

    u32 arm9start = 0;

    // load the ARM9 secure area
    if (header.ARM9ROMOffset >= 0x4000 && header.ARM9ROMOffset < 0x8000)
    {
        u8 securearea[0x800];
        NDSCartSlot.DecryptSecureArea(securearea);

        for (u32 i = 0; i < 0x800; i+=4)
        {
            NDS::ARM9Write32(header.ARM9RAMAddress+i, *(u32*)&securearea[i]);
            arm9start += 4;
        }
    }

    // CHECKME: firmware seems to load this in 0x200 byte chunks

    for (u32 i = arm9start; i < header.ARM9Size; i+=4)
    {
        u32 tmp = *(u32*)&cartrom[header.ARM9ROMOffset+i];
        NDS::ARM9Write32(header.ARM9RAMAddress+i, tmp);
    }

    for (u32 i = 0; i < header.ARM7Size; i+=4)
    {
        u32 tmp = *(u32*)&cartrom[header.ARM7ROMOffset+i];
        NDS::ARM7Write32(header.ARM7RAMAddress+i, tmp);
    }

    ARM7BIOSProt = 0x1204;

    SPI.GetFirmwareMem()->SetupDirectBoot();

    ARM9.CP15Write(0x100, 0x00052078);
    ARM9.CP15Write(0x200, 0x00000042);
    ARM9.CP15Write(0x201, 0x00000042);
    ARM9.CP15Write(0x300, 0x00000002);
    ARM9.CP15Write(0x502, 0x15111011);
    ARM9.CP15Write(0x503, 0x05100011);
    ARM9.CP15Write(0x600, 0x04000033);
    ARM9.CP15Write(0x601, 0x04000033);
    ARM9.CP15Write(0x610, 0x0200002B);
    ARM9.CP15Write(0x611, 0x0200002B);
    ARM9.CP15Write(0x620, 0x00000000);
    ARM9.CP15Write(0x621, 0x00000000);
    ARM9.CP15Write(0x630, 0x08000035);
    ARM9.CP15Write(0x631, 0x08000035);
    ARM9.CP15Write(0x640, 0x0300001B);
    ARM9.CP15Write(0x641, 0x0300001B);
    ARM9.CP15Write(0x650, 0x00000000);
    ARM9.CP15Write(0x651, 0x00000000);
    ARM9.CP15Write(0x660, 0xFFFF001D);
    ARM9.CP15Write(0x661, 0xFFFF001D);
    ARM9.CP15Write(0x670, 0x027FF017);
    ARM9.CP15Write(0x671, 0x027FF017);
    ARM9.CP15Write(0x910, 0x0300000A);
    ARM9.CP15Write(0x911, 0x00000020);
}

void NDS::SetupDirectBoot(const std::string& romname)
{
    const NDSHeader& header = NDSCartSlot.GetCart()->GetHeader();
    SetupDirectBoot();

    NDSCartSlot.SetupDirectBoot(romname);

    ARM9.R[12] = header.ARM9EntryAddress;
    ARM9.R[13] = 0x03002F7C;
    ARM9.R[14] = header.ARM9EntryAddress;
    ARM9.R_IRQ[0] = 0x03003F80;
    ARM9.R_SVC[0] = 0x03003FC0;

    ARM7.R[12] = header.ARM7EntryAddress;
    ARM7.R[13] = 0x0380FD80;
    ARM7.R[14] = header.ARM7EntryAddress;
    ARM7.R_IRQ[0] = 0x0380FF80;
    ARM7.R_SVC[0] = 0x0380FFC0;

    ARM9.JumpTo(header.ARM9EntryAddress);
    ARM7.JumpTo(header.ARM7EntryAddress);

    SetExMemCnt(0, 0xE880, 0xFFFF);
    SetExMemCnt(1, 0x0080, 0x00FF);

    PostFlag9 = 0x01;
    PostFlag7 = 0x01;

    PowerControl9 = 0x820F;
    GPU.SetPowerCnt(PowerControl9);

    PowerControl7 = 0x0001;
    SPU.SetPowerCnt(PowerControl7 & 0x0001);
    Wifi.SetPowerCnt(PowerControl7 & 0x0002);

    // checkme
    RCnt = 0x8000;

    //NDSCartSlot.SetSPICnt(0x8000);
    // TODO CHECK ME
    NDSCartSlot.WriteSPICnt(0, 0x8000, 0xFFFF);
    NDSCartSlot.WriteSPICnt(1, 0x8000, 0xFFFF);

    SPU.SetBias(0x200);

    SetWifiWaitCnt(0x0030);
}

void NDS::Reset()
{
    Platform::FileHandle* f;
    u32 i;

    RunningGame = false;
    LastSysClockCycles = 0;

    // BIOS files are now loaded by the frontend

    JIT.Reset();

    if (ConsoleType == 1)
    {
        // BIOS files are now loaded by the frontend

        ARM9ClockShift = 2;
        MainRAMMask = 0xFFFFFF;
    }
    else
    {
        ARM9ClockShift = 1;
        MainRAMMask = 0x3FFFFF;
    }
    // has to be called before InitTimings
    // otherwise some PU settings are completely
    // unitialised on the first run
    ARM9.CP15Reset();

    ARM9Timestamp = 0; ARM9Target = 0;
    ARM7Timestamp = 0; ARM7Target = 0;
    SysTimestamp = 0;
    NumFrames = 0;
    NumLagFrames = 0;
    NSMLGameRAMRestorePending = false;
    NSMLGameRAMRestoreData = nullptr;
    NSMLGameRAMRestoreLength = 0;
    NSMLGameRAMRestoreOwnedBuffer.clear();
    NSMLGameRAMHistoryReachedExitGate = false;
    NSMLGameRAMReplayDisplayStartFrame = 0xFFFFFFFF;
    NSMLGameRAMCheckpoints.clear();
    NSMLNextGameRAMCheckpoint = 0;
    NSMLGameRAMCheckpointTimeline.Reset();
    NSMLGameRAMRestoreUs = 0;
    NSMLGameRAMRestoreBytes = 0;
    LagFrameFlag = false;

    InitTimings();

    memset(MainRAM, 0, MainRAMMask + 1);
    memset(SharedWRAM, 0, 0x8000);
    memset(ARM7WRAM, 0, 0x10000);

    MapSharedWRAM(0);

    // TODO FIX THOSE VALUES
    // TODO figure out what they should be
    ExMemCnt[0] = 0x6000;
    ExMemCnt[1] = 0x6000;
    SetGBASlotTimings();

    IME[0] = 0;
    IE[0] = 0;
    IF[0] = 0;
    IME[1] = 0;
    IE[1] = 0;
    IF[1] = 0;
    IE2 = 0;
    IF2 = 0;

    PostFlag9 = 0x00;
    PostFlag7 = 0x00;
    PowerControl9 = 0x0000;
    PowerControl7 = 0x0000;

    WifiWaitCnt = 0xFFFF; // temp
    SetWifiWaitCnt(0);

    ARM7BIOSProt = 0;

    IPCSync9 = 0;
    IPCSync7 = 0;
    IPCFIFOCnt9 = 0;
    IPCFIFOCnt7 = 0;
    IPCFIFO9.Clear();
    IPCFIFO7.Clear();

    DivCnt = 0;
    SqrtCnt = 0;

    ARM9.Reset();
    ARM7.Reset();

    CPUStop = 0;

    memset(Timers, 0, 8*sizeof(Timer));
    TimerCheckMask[0] = 0;
    TimerCheckMask[1] = 0;
    TimerTimestamp[0] = 0;
    TimerTimestamp[1] = 0;

    for (i = 0; i < 8; i++) DMAs[i].Reset();
    memset(DMA9Fill, 0, 4*4);

    for (i = 0; i < Event_MAX; i++)
    {
        SchedEvent& evt = SchedList[i];

        evt.Timestamp = 0;
        evt.FuncID = 0;
        evt.Param = 0;
    }
    SchedListMask = 0;

    KeyInput = 0x007F03FF;
    KeyCnt[0] = 0;
    KeyCnt[1] = 0;
    RCnt = 0;

    GPU.Reset();
    NDSCartSlot.Reset();
    GBACartSlot.Reset();
    SPU.Reset();
    Mic.Reset();
    SPI.Reset();
    RTC.Reset();
    Wifi.Reset();
}

void NDS::Start()
{
    Running = true;

    if (ConsoleType != 0)
        return;

    auto* ndscart = NDSCartSlot.GetCart();
    if (!ndscart)
        return;

    if (auto* cart = GBACartSlot.GetCart(); cart && cart->Type() == GBACart::CartType::GameSolarSensor)
    { // If we have a solar sensor cart inserted...
        auto& solarcart = *static_cast<GBACart::CartGameSolarSensor*>(cart);
        GBACart::GBAHeader& header = solarcart.GetHeader();
        if (strncmp(header.Title, GBACart::BOKTAI_STUB_TITLE, sizeof(header.Title)) == 0) {
            // If this is a stub Boktai cart (so we can use the sensor without a full ROM)...

            // ...then copy the Nintendo logo data from the NDS ROM into the stub GBA ROM.
            // Otherwise, the GBA cart won't be recognized.
            memcpy(header.NintendoLogo, ndscart->GetHeader().NintendoLogo, sizeof(header.NintendoLogo));
        }
    }
}

static const char* StopReasonName(Platform::StopReason reason)
{
    switch (reason)
    {
        case Platform::StopReason::External:
            return "External";
        case Platform::StopReason::PowerOff:
            return "PowerOff";
        case Platform::StopReason::GBAModeNotSupported:
            return "GBAModeNotSupported";
        case Platform::StopReason::BadExceptionRegion:
            return "BadExceptionRegion";
        default:
            return "Unknown";
    }
}

void NDS::Stop(Platform::StopReason reason)
{
    Platform::LogLevel level;
    switch (reason)
    {
        case Platform::StopReason::External:
        case Platform::StopReason::PowerOff:
            level = LogLevel::Info;
            break;
        case Platform::StopReason::GBAModeNotSupported:
        case Platform::StopReason::BadExceptionRegion:
            level = LogLevel::Error;
            break;
        default:
            level = LogLevel::Warn;
            break;
    }

    Log(level, "Stopping emulated console (Reason: %s)\n", StopReasonName(reason));
    Running = false;
    Platform::SignalStop(reason, UserData);
    GPU.Stop();
    SPU.Stop();
    Mic.StopAll();
}

u32 NDS::GetSavestateConfig()
{
    u32 ret = 0;

    if (ConsoleType == 1)
        ret |= SC_Console_DSi;

    return ret;
}

bool NDS::DoSavestate(Savestate* file)
{
    file->Section("NDSG");

    u32 config = GetSavestateConfig();
    if (file->Saving)
    {
        file->Var32(&config);
    }
    else
    {
        u32 config_chk;
        file->Var32(&config_chk);
        if (config_chk != config)
        {
            Log(LogLevel::Error, "savestate: Expected config word %08X, got %08X. cannot load.\n", config, config_chk);
            return false;
        }
    }

    file->VarArray(MainRAM, MainRAMMaxSize);
    file->VarArray(SharedWRAM, SharedWRAMSize);
    file->VarArray(ARM7WRAM, ARM7WRAMSize);

    //file->VarArray(ARM9BIOS, 0x1000);
    //file->VarArray(ARM7BIOS, 0x4000);

    file->VarArray(ExMemCnt, 2*sizeof(u16));

    file->Var16(&WifiWaitCnt);

    file->VarArray(IME, 2*sizeof(u32));
    file->VarArray(IE, 2*sizeof(u32));
    file->VarArray(IF, 2*sizeof(u32));
    file->Var32(&IE2);
    file->Var32(&IF2);

    file->Var8(&PostFlag9);
    file->Var8(&PostFlag7);
    file->Var16(&PowerControl9);
    file->Var16(&PowerControl7);

    file->Var16(&ARM7BIOSProt);

    file->Var16(&IPCSync9);
    file->Var16(&IPCSync7);
    file->Var16(&IPCFIFOCnt9);
    file->Var16(&IPCFIFOCnt7);
    IPCFIFO9.DoSavestate(file);
    IPCFIFO7.DoSavestate(file);

    file->Var16(&DivCnt);
    file->Var16(&SqrtCnt);

    file->Var32(&CPUStop);

    for (int i = 0; i < 8; i++)
    {
        Timer* timer = &Timers[i];

        file->Var16(&timer->Reload);
        file->Var16(&timer->Cnt);
        file->Var32(&timer->Counter);
        file->Var32(&timer->CycleShift);
    }
    file->VarArray(TimerCheckMask, 2*sizeof(u8));
    file->VarArray(TimerTimestamp, 2*sizeof(u64));

    file->VarArray(DMA9Fill, 4*sizeof(u32));

    for (int i = 0; i < Event_MAX; i++)
    {
        SchedEvent& evt = SchedList[i];

        file->Var64(&evt.Timestamp);
        file->Var32(&evt.FuncID);
        file->Var32(&evt.Param);
    }
    file->Var32(&SchedListMask);
    file->Var64(&ARM9Timestamp);
    file->Var64(&ARM9Target);
    file->Var64(&ARM7Timestamp);
    file->Var64(&ARM7Target);
    file->Var64(&SysTimestamp);
    file->Var64(&LastSysClockCycles);
    file->Var64(&FrameStartTimestamp);
    file->Var32(&NumFrames);
    file->Var32(&NumLagFrames);
    file->Bool32(&LagFrameFlag);

    // TODO: save KeyInput????
    file->VarArray(KeyCnt, 2*sizeof(u16));
    file->Var16(&RCnt);

    file->Var8(&WRAMCnt);

    file->Bool32(&RunningGame);

    if (!file->Saving)
    {
        // 'dept of redundancy dept'
        // but we do need to update the mappings
        MapSharedWRAM(WRAMCnt);

        InitTimings();
        SetGBASlotTimings();

        UpdateWifiTimings();
    }

    for (int i = 0; i < 8; i++)
        DMAs[i].DoSavestate(file);

    ARM9.DoSavestate(file);
    ARM7.DoSavestate(file);

    NDSCartSlot.DoSavestate(file);
    if (ConsoleType == 0)
        GBACartSlot.DoSavestate(file);
    GPU.DoSavestate(file);
    SPU.DoSavestate(file);
    Mic.DoSavestate(file);
    SPI.DoSavestate(file);
    RTC.DoSavestate(file);
    Wifi.DoSavestate(file);

    DoSavestateExtra(file); // Handles DSi state if applicable

    if (!file->Saving)
    {
        GPU.SetPowerCnt(PowerControl9);

        SPU.SetPowerCnt(PowerControl7 & 0x0001);
        Wifi.SetPowerCnt(PowerControl7 & 0x0002);

#ifdef JIT_ENABLED
        JIT.Reset();
#endif
    }

    file->Finish();

    return true;
}

bool NDS::DoRollbackSavestate(
    Savestate* file,
    u32 requestedMainRAMMode,
    const u8* deltaBaseMainRAM,
    u32 mainRAMPageSize,
    u32 requestedCoreSkipMask)
{
    file->Section("NDSR");

    u32 config = GetSavestateConfig();
    if (file->Saving)
    {
        file->Var32(&config);
    }
    else
    {
        u32 config_chk;
        file->Var32(&config_chk);
        if (config_chk != config)
        {
            Log(LogLevel::Error, "rollback savestate: Expected config word %08X, got %08X. cannot load.\n", config, config_chk);
            return false;
        }
    }

    u32 mainRAMLength = MainRAMMask + 1;
    file->Var32(&mainRAMLength);
    if (mainRAMLength == 0 || mainRAMLength > MainRAMMaxSize || mainRAMLength != MainRAMMask + 1)
    {
        Log(LogLevel::Error, "rollback savestate: bad main RAM length %u, expected %u\n", mainRAMLength, MainRAMMask + 1);
        return false;
    }

    u32 mainRAMMode = requestedMainRAMMode;
    file->Var32(&mainRAMMode);
    if (mainRAMMode == 0)
    {
        file->VarArray(MainRAM, mainRAMLength);
    }
    else if (mainRAMMode == 3)
    {
        // Diagnostic rollback mode: preserve all non-MainRAM core state while
        // leaving Main RAM to a game-specific snapshot layer.
    }
    else if (mainRAMMode == 1 || mainRAMMode == 2)
    {
        u32 pageSize = mainRAMPageSize;
        if (pageSize < 256 || pageSize > 4096 || (pageSize & (pageSize - 1)) != 0)
            pageSize = 4096;
        u32 pageCount = (mainRAMLength + pageSize - 1) / pageSize;
        u32 savedPageCount = 0;

        if (mainRAMMode == 2 && !deltaBaseMainRAM)
        {
            Log(LogLevel::Error, "rollback savestate: delta main RAM requested without a base\n");
            return false;
        }

        if (file->Saving)
        {
            for (u32 page = 0; page < pageCount; page++)
            {
                const u32 offset = page * pageSize;
                const u32 len = std::min(pageSize, mainRAMLength - offset);
                bool savePage = false;
                if (mainRAMMode == 1)
                {
                    for (u32 i = 0; i < len; i++)
                    {
                        if (MainRAM[offset + i] != 0)
                        {
                            savePage = true;
                            break;
                        }
                    }
                }
                else
                {
                    savePage = memcmp(MainRAM + offset, deltaBaseMainRAM + offset, len) != 0;
                }
                if (savePage)
                    savedPageCount++;
            }
        }

        u32 storedPageSize = pageSize;
        u32 storedPageCount = pageCount;
        file->Var32(&storedPageSize);
        file->Var32(&storedPageCount);
        file->Var32(&savedPageCount);
        if (!file->Saving)
        {
            pageSize = storedPageSize;
            pageCount = pageSize == 0 ? 0 : (mainRAMLength + pageSize - 1) / pageSize;
        }
        if (storedPageSize < 256
            || storedPageSize > 4096
            || (storedPageSize & (storedPageSize - 1)) != 0
            || storedPageCount != pageCount)
        {
            Log(LogLevel::Error, "rollback savestate: bad sparse main RAM layout pageSize=%u pageCount=%u\n",
                storedPageSize,
                storedPageCount);
            return false;
        }

        if (!file->Saving)
        {
            if (mainRAMMode == 1)
                memset(MainRAM, 0, mainRAMLength);
            else
                memcpy(MainRAM, deltaBaseMainRAM, mainRAMLength);
        }

        if (file->Saving)
        {
            for (u32 page = 0; page < pageCount; page++)
            {
                const u32 offset = page * pageSize;
                const u32 len = std::min(pageSize, mainRAMLength - offset);
                bool savePage = false;
                if (mainRAMMode == 1)
                {
                    for (u32 i = 0; i < len; i++)
                    {
                        if (MainRAM[offset + i] != 0)
                        {
                            savePage = true;
                            break;
                        }
                    }
                }
                else
                {
                    savePage = memcmp(MainRAM + offset, deltaBaseMainRAM + offset, len) != 0;
                }
                if (!savePage)
                    continue;

                u32 savedPage = page;
                file->Var32(&savedPage);
                file->VarArray(MainRAM + offset, len);
            }
        }
        else
        {
            for (u32 i = 0; i < savedPageCount; i++)
            {
                u32 page = 0;
                file->Var32(&page);
                if (page >= pageCount)
                {
                    Log(LogLevel::Error, "rollback savestate: sparse main RAM page %u out of %u\n",
                        page,
                        pageCount);
                    return false;
                }
                const u32 offset = page * pageSize;
                const u32 len = std::min(pageSize, mainRAMLength - offset);
                file->VarArray(MainRAM + offset, len);
            }
        }
    }
    else
    {
        Log(LogLevel::Error, "rollback savestate: unsupported main RAM mode %u\n", mainRAMMode);
        return false;
    }

    u32 coreSkipMask = requestedCoreSkipMask;
    file->Var32(&coreSkipMask);

    constexpr u32 kRollbackCoreSkipCart = 1 << 0;
    constexpr u32 kRollbackCoreSkipGPU = 1 << 1;
    constexpr u32 kRollbackCoreSkipSPU = 1 << 2;
    constexpr u32 kRollbackCoreSkipMicSpiRtc = 1 << 3;
    constexpr u32 kRollbackCoreSkipWifi = 1 << 4;

    file->VarArray(SharedWRAM, SharedWRAMSize);
    file->VarArray(ARM7WRAM, ARM7WRAMSize);

    file->VarArray(ExMemCnt, 2*sizeof(u16));

    file->Var16(&WifiWaitCnt);

    file->VarArray(IME, 2*sizeof(u32));
    file->VarArray(IE, 2*sizeof(u32));
    file->VarArray(IF, 2*sizeof(u32));
    file->Var32(&IE2);
    file->Var32(&IF2);

    file->Var8(&PostFlag9);
    file->Var8(&PostFlag7);
    file->Var16(&PowerControl9);
    file->Var16(&PowerControl7);

    file->Var16(&ARM7BIOSProt);

    file->Var16(&IPCSync9);
    file->Var16(&IPCSync7);
    file->Var16(&IPCFIFOCnt9);
    file->Var16(&IPCFIFOCnt7);
    IPCFIFO9.DoSavestate(file);
    IPCFIFO7.DoSavestate(file);

    file->Var16(&DivCnt);
    file->Var16(&SqrtCnt);

    file->Var32(&CPUStop);

    for (int i = 0; i < 8; i++)
    {
        Timer* timer = &Timers[i];

        file->Var16(&timer->Reload);
        file->Var16(&timer->Cnt);
        file->Var32(&timer->Counter);
        file->Var32(&timer->CycleShift);
    }
    file->VarArray(TimerCheckMask, 2*sizeof(u8));
    file->VarArray(TimerTimestamp, 2*sizeof(u64));

    file->VarArray(DMA9Fill, 4*sizeof(u32));

    for (int i = 0; i < Event_MAX; i++)
    {
        SchedEvent& evt = SchedList[i];

        file->Var64(&evt.Timestamp);
        file->Var32(&evt.FuncID);
        file->Var32(&evt.Param);
    }
    file->Var32(&SchedListMask);
    file->Var64(&ARM9Timestamp);
    file->Var64(&ARM9Target);
    file->Var64(&ARM7Timestamp);
    file->Var64(&ARM7Target);
    file->Var64(&SysTimestamp);
    file->Var64(&LastSysClockCycles);
    file->Var64(&FrameStartTimestamp);
    file->Var32(&NumFrames);
    file->Var32(&NumLagFrames);
    file->Bool32(&LagFrameFlag);

    file->VarArray(KeyCnt, 2*sizeof(u16));
    file->Var16(&RCnt);

    file->Var8(&WRAMCnt);

    file->Bool32(&RunningGame);

    if (!file->Saving)
    {
        MapSharedWRAM(WRAMCnt);

        InitTimings();
        SetGBASlotTimings();

        UpdateWifiTimings();
    }

    for (int i = 0; i < 8; i++)
        DMAs[i].DoSavestate(file);

    ARM9.DoSavestate(file);
    ARM7.DoSavestate(file);

    if (!(coreSkipMask & kRollbackCoreSkipCart))
    {
        NDSCartSlot.DoSavestate(file);
        if (ConsoleType == 0)
            GBACartSlot.DoSavestate(file);
    }
    if (!(coreSkipMask & kRollbackCoreSkipGPU))
        GPU.DoSavestate(file);
    if (!(coreSkipMask & kRollbackCoreSkipSPU))
        SPU.DoSavestate(file);
    if (!(coreSkipMask & kRollbackCoreSkipMicSpiRtc))
    {
        Mic.DoSavestate(file);
        SPI.DoSavestate(file);
        RTC.DoSavestate(file);
    }
    if (!(coreSkipMask & kRollbackCoreSkipWifi))
        Wifi.DoSavestate(file);

    DoSavestateExtra(file);

    if (!file->Saving)
    {
        GPU.SetPowerCnt(PowerControl9);

        SPU.SetPowerCnt(PowerControl7 & 0x0001);
        Wifi.SetPowerCnt(PowerControl7 & 0x0002);

#ifdef JIT_ENABLED
        if (!NSMLRollbackSkipJITReset())
            JIT.Reset();
#endif
    }

    file->Finish();

    return true;
}

bool NDS::DoRollbackTinyCoreSavestate(Savestate* file, u32 requestedTinyCoreFlags)
{
    file->Section("NDST");

    u32 config = GetSavestateConfig();
    if (file->Saving)
    {
        file->Var32(&config);
    }
    else
    {
        u32 config_chk;
        file->Var32(&config_chk);
        if (config_chk != config)
        {
            Log(LogLevel::Error, "rollback tiny core: Expected config word %08X, got %08X. cannot load.\n", config, config_chk);
            return false;
        }
    }

    u32 tinyCoreFlags = requestedTinyCoreFlags;
    file->Var32(&tinyCoreFlags);
    constexpr u32 kRollbackTinyCoreGPU2DTiming = 1 << 0;
    constexpr u32 kRollbackTinyCoreFullGPU = 1 << 1;
    constexpr u32 kRollbackTinyCoreSPU = 1 << 2;
    constexpr u32 kRollbackTinyCoreWifi = 1 << 3;
    constexpr u32 kRollbackTinyCoreCart = 1 << 4;
    constexpr u32 kRollbackTinyCoreMicSpiRtc = 1 << 5;
    constexpr u32 kRollbackTinyCoreGPUPaletteOAM = 1 << 6;
    constexpr u32 kRollbackTinyCoreGPUVRAM = 1 << 7;
    constexpr u32 kRollbackTinyCoreGPU3D = 1 << 8;
    constexpr u32 kRollbackTinyCoreGPU3DLight = 1 << 9;

    file->VarArray(SharedWRAM, SharedWRAMSize);
    file->VarArray(ARM7WRAM, ARM7WRAMSize);

    file->VarArray(ExMemCnt, 2*sizeof(u16));
    file->Var16(&WifiWaitCnt);

    file->VarArray(IME, 2*sizeof(u32));
    file->VarArray(IE, 2*sizeof(u32));
    file->VarArray(IF, 2*sizeof(u32));
    file->Var32(&IE2);
    file->Var32(&IF2);

    file->Var8(&PostFlag9);
    file->Var8(&PostFlag7);
    file->Var16(&PowerControl9);
    file->Var16(&PowerControl7);

    file->Var16(&ARM7BIOSProt);

    file->Var16(&IPCSync9);
    file->Var16(&IPCSync7);
    file->Var16(&IPCFIFOCnt9);
    file->Var16(&IPCFIFOCnt7);
    IPCFIFO9.DoSavestate(file);
    IPCFIFO7.DoSavestate(file);

    file->Var16(&DivCnt);
    file->Var16(&SqrtCnt);
    file->Var32(&CPUStop);

    for (int i = 0; i < 8; i++)
    {
        Timer* timer = &Timers[i];
        file->Var16(&timer->Reload);
        file->Var16(&timer->Cnt);
        file->Var32(&timer->Counter);
        file->Var32(&timer->CycleShift);
    }
    file->VarArray(TimerCheckMask, 2*sizeof(u8));
    file->VarArray(TimerTimestamp, 2*sizeof(u64));

    file->VarArray(DMA9Fill, 4*sizeof(u32));

    for (int i = 0; i < Event_MAX; i++)
    {
        SchedEvent& evt = SchedList[i];
        file->Var64(&evt.Timestamp);
        file->Var32(&evt.FuncID);
        file->Var32(&evt.Param);
    }
    file->Var32(&SchedListMask);
    file->Var64(&ARM9Timestamp);
    file->Var64(&ARM9Target);
    file->Var64(&ARM7Timestamp);
    file->Var64(&ARM7Target);
    file->Var64(&SysTimestamp);
    file->Var64(&LastSysClockCycles);
    file->Var64(&FrameStartTimestamp);
    file->Var32(&NumFrames);
    file->Var32(&NumLagFrames);
    file->Bool32(&LagFrameFlag);

    file->VarArray(KeyCnt, 2*sizeof(u16));
    file->Var16(&RCnt);
    file->Var8(&WRAMCnt);
    file->Bool32(&RunningGame);

    if (!file->Saving)
    {
        MapSharedWRAM(WRAMCnt);
        InitTimings();
        SetGBASlotTimings();
        UpdateWifiTimings();
    }

    for (int i = 0; i < 8; i++)
        DMAs[i].DoSavestate(file);

    ARM9.DoSavestate(file);
    ARM7.DoSavestate(file);

    if (tinyCoreFlags & kRollbackTinyCoreCart)
    {
        NDSCartSlot.DoSavestate(file);
        if (ConsoleType == 0)
            GBACartSlot.DoSavestate(file);
    }

    if (tinyCoreFlags & kRollbackTinyCoreFullGPU)
        GPU.DoSavestate(file);
    else if (tinyCoreFlags & (kRollbackTinyCoreGPU2DTiming
        | kRollbackTinyCoreGPUPaletteOAM
        | kRollbackTinyCoreGPUVRAM
        | kRollbackTinyCoreGPU3D
        | kRollbackTinyCoreGPU3DLight))
    {
        GPU.DoRollbackSubsetSavestate(file, tinyCoreFlags);
    }
    if (tinyCoreFlags & kRollbackTinyCoreSPU)
        SPU.DoSavestate(file);
    if (tinyCoreFlags & kRollbackTinyCoreMicSpiRtc)
    {
        Mic.DoSavestate(file);
        SPI.DoSavestate(file);
        RTC.DoSavestate(file);
    }
    if (tinyCoreFlags & kRollbackTinyCoreWifi)
        Wifi.DoSavestate(file);

    if (!file->Saving)
    {
        GPU.SetPowerCnt(PowerControl9);
        SPU.SetPowerCnt(PowerControl7 & 0x0001);
        Wifi.SetPowerCnt(PowerControl7 & 0x0002);

#ifdef JIT_ENABLED
        if (!NSMLRollbackSkipJITReset())
            JIT.Reset();
#endif
    }

    file->Finish();
    return true;
}

void NDS::SetNDSCart(std::unique_ptr<NDSCart::CartCommon>&& cart)
{
    NDSCartSlot.SetCart(std::move(cart));
    // The existing cart will always be ejected;
    // if cart is null, then that's equivalent to ejecting a cart
    // without inserting a new one.
}

void NDS::SetNDSSave(const u8* savedata, u32 savelen)
{
    if (savedata && savelen)
        NDSCartSlot.SetSaveMemory(savedata, savelen);
}

void NDS::SetGBASave(const u8* savedata, u32 savelen)
{
    if (ConsoleType == 0 && savedata && savelen)
    {
        GBACartSlot.SetSaveMemory(savedata, savelen);
    }

}

void NDS::LoadBIOS()
{
    Reset();
}

void NDS::SetARM7BIOS(const std::array<u8, ARM7BIOSSize>& bios) noexcept
{
    ARM7BIOS = bios;
    ARM7BIOSNative = CRC32(ARM7BIOS.data(), ARM7BIOS.size()) == ARM7BIOSCRC32;
}

void NDS::SetARM9BIOS(const std::array<u8, ARM9BIOSSize>& bios) noexcept
{
    ARM9BIOS = bios;
    ARM9BIOSNative = CRC32(ARM9BIOS.data(), ARM9BIOS.size()) == ARM9BIOSCRC32;
}

u64 NDS::NextTarget()
{
    u64 minEvent = UINT64_MAX;

    u32 mask = SchedListMask;
    for (int i = 0; i < Event_MAX; i++)
    {
        if (!mask) break;
        if ((mask & 0x1) && !(NSMLGameTickProbeDeferLCD && i == Event_LCD))
        {
            if (SchedList[i].Timestamp < minEvent)
                minEvent = SchedList[i].Timestamp;
        }

        mask >>= 1;
    }

    u64 max = SysTimestamp + kMaxIterationCycles;

    if (minEvent < max + kIterationCycleMargin)
        return minEvent;

    return max;
}

void NDS::RunSystem(u64 timestamp)
{
    SysTimestamp = timestamp;

    u32 mask = SchedListMask;
    for (int i = 0; i < Event_MAX; i++)
    {
        if (!mask) break;
        if ((mask & 0x1) && !(NSMLGameTickProbeDeferLCD && i == Event_LCD))
        {
            SchedEvent& evt = SchedList[i];

            if (evt.Timestamp <= SysTimestamp)
            {
                SchedListMask &= ~(1<<i);

                EventFunc func = evt.Funcs[evt.FuncID];
                func(evt.That, evt.Param);
            }
        }

        mask >>= 1;
    }
}

u64 NDS::NextTargetSleep()
{
    u64 minEvent = UINT64_MAX;

    u32 mask = SchedListMask;
    for (int i = 0; i < Event_MAX; i++)
    {
        if (!mask) break;
        if (i == Event_SPU || i == Event_RTC)
        {
            if (mask & 0x1)
            {
                if (SchedList[i].Timestamp < minEvent)
                    minEvent = SchedList[i].Timestamp;
            }
        }

        mask >>= 1;
    }

    return minEvent;
}

void NDS::RunSystemSleep(u64 timestamp)
{
    u64 offset = timestamp - SysTimestamp;
    SysTimestamp = timestamp;

    u32 mask = SchedListMask;
    for (int i = 0; i < Event_MAX; i++)
    {
        if (!mask) break;
        if (i == Event_RTC)
        {
            if (mask & 0x1)
            {
                SchedEvent& evt = SchedList[i];

                if (evt.Timestamp <= SysTimestamp)
                {
                    SchedListMask &= ~(1<<i);

                    EventFunc func = evt.Funcs[evt.FuncID];
                    func(evt.That, evt.Param);
                }
            }
        }
        else if (mask & 0x1)
        {
            if (SchedList[i].Timestamp <= SysTimestamp)
            {
                SchedList[i].Timestamp += offset;
            }
        }

        mask >>= 1;
    }
}

static void RecordNSMLRomGameTickProbeStage(NDS* nds, u32 marker)
{
    constexpr u32 activeAddr = 0x02001AC4;
    constexpr u32 historyEnabledAddr = 0x02001ACC;
    constexpr u32 historyIndexAddr = 0x02001AD0;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyTargetAddr = 0x02001AD8;
    constexpr u32 gameFrameAddr = 0x0208B668;
    constexpr u32 frameBeginMarker = 0x100;
    constexpr u32 frameEndMarker = 0x101;

    const char* stage = nullptr;
    switch (marker)
    {
    case 1: stage = "tick_begin"; break;
    case 2: stage = "input_begin"; break;
    case 3: stage = "input_end"; break;
    case 4: stage = "render_begin"; break;
    case 5: stage = "render_end"; break;
    case 6: stage = "tick_end"; break;
    case 7: stage = "delete_begin"; break;
    case 8: stage = "delete_end"; break;
    case 9: stage = "create_begin"; break;
    case 10: stage = "create_end"; break;
    case 11: stage = "gameplay_begin"; break;
    case 12: stage = "gameplay_end"; break;
    case frameBeginMarker: stage = "frame_begin"; break;
    case frameEndMarker: stage = "frame_end"; break;
    default: return;
    }

    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        bool StageEnabled = false;
        bool JitProfileEnabled = false;
        bool JitRenderProfileEnabled = false;
        bool JitRenderStateDumpEnabled = false;
        bool DeferLCD = false;
        bool DiscardIntermediate3D = false;
        std::string Role = "local";
        std::string OutputDir;
        FILE* LogFile = nullptr;
        FILE* JitProfileFile = nullptr;
        FILE* JitRenderStateFile = nullptr;
    };
    struct State
    {
        bool HasOrigin = false;
        bool LCDDeferred = false;
        u64 Sequence = 0;
        u64 LCDDeferStartTimestamp = 0;
        std::chrono::steady_clock::time_point Origin;
    };
    static Config cfg;
    static State state;
    if (!cfg.Checked)
    {
        cfg.StageEnabled = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_STAGE_TRACE") != nullptr;
        cfg.JitProfileEnabled = getenv("MELONDS_NSML_JIT_EXECUTION_PROFILE") != nullptr;
        cfg.JitRenderProfileEnabled = getenv("MELONDS_NSML_JIT_RENDER_EXECUTION_PROFILE") != nullptr;
        cfg.JitRenderStateDumpEnabled = getenv("MELONDS_NSML_JIT_RENDER_STATE_DUMP") != nullptr;
        cfg.DeferLCD = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_DEFER_LCD") != nullptr;
        cfg.DiscardIntermediate3D =
            getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_DISCARD_INTERMEDIATE_3D") != nullptr;
        cfg.Enabled = cfg.StageEnabled || cfg.JitProfileEnabled || cfg.DeferLCD ||
            cfg.DiscardIntermediate3D;
        if (const char* role = getenv("MELONDS_NSML_ROLE"))
            cfg.Role = role;
        if (const char* outputDir = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_DIR"))
            cfg.OutputDir = outputDir;
        if (cfg.StageEnabled && !cfg.OutputDir.empty())
        {
            const std::string logPath = cfg.OutputDir + "/rom-game-tick-stages-" + cfg.Role + ".csv";
            cfg.LogFile = fopen(logPath.c_str(), "w");
            if (cfg.LogFile)
            {
                fprintf(cfg.LogFile,
                    "role,sequence,stage,marker,display_frame,game_frame,history_enabled,history_index,history_count,active,wall_us,sys_timestamp,arm9_timestamp,arm7_timestamp\n");
            }
        }
        if (cfg.JitProfileEnabled && !cfg.OutputDir.empty())
        {
            const std::string profilePath = cfg.OutputDir +
                (cfg.JitRenderProfileEnabled ? "/rom-game-tick-jit-render-blocks-" :
                                               "/rom-game-tick-jit-blocks-") +
                cfg.Role + ".csv";
            cfg.JitProfileFile = fopen(profilePath.c_str(), "w");
            if (cfg.JitProfileFile)
            {
                fprintf(cfg.JitProfileFile,
                    "role,cpu,address,mode,first_instruction,block_instructions,count,dynamic_instructions\n");
            }
        }
        if (cfg.JitRenderStateDumpEnabled && !cfg.OutputDir.empty())
        {
            const std::string statePath = cfg.OutputDir + "/rom-game-tick-render-state-" + cfg.Role + ".csv";
            cfg.JitRenderStateFile = fopen(statePath.c_str(), "w");
            if (cfg.JitRenderStateFile)
            {
                fprintf(cfg.JitRenderStateFile,
                    "role,stage,display_frame,vcount,total_scanlines,back_buffer,dispstat9,dispstat7,"
                    "oam_hash,palette_hash,vram_hash,gxstat,gpu3d_timestamp,cycle_count,cmd_fifo,cmd_pipe,"
                    "cur_ram_bank,num_vertices,num_polygons,render_num_polygons,flush_request\n");
            }
        }
        cfg.Checked = true;
    }
    if (!cfg.Enabled || !nds)
        return;
    // A transaction may be cancelled after render_begin without reaching its
    // matching marker.  Close the ordered GPU batch before the next frame.
    if (cfg.DiscardIntermediate3D && marker == frameBeginMarker &&
        nds->NSMLGameTickProbeIntermediate3DBatch)
    {
        nds->GPU.GPU3D.EndNSMLDiscardedGeometryBatch();
        nds->NSMLGameTickProbeIntermediate3DBatch = false;
    }
    const u32 historyEnabled = nds->ARM9Read32(historyEnabledAddr);
    if (!historyEnabled)
        return;
    const u32 historyIndex = nds->ARM9Read32(historyIndexAddr);
    const u32 historyCount = nds->ARM9Read32(historyCountAddr);
    if (cfg.DiscardIntermediate3D)
    {
        if (marker == 4)
        {
            const bool isRollbackTarget = nds->ARM9Read32(historyTargetAddr) != 0;
            if (isRollbackTarget && historyIndex < historyCount)
            {
                nds->GPU.GPU3D.BeginNSMLDiscardedGeometryBatch();
                nds->NSMLGameTickProbeIntermediate3DBatch = true;
            }
        }
        else if ((marker == 5 || marker == 6) &&
            nds->NSMLGameTickProbeIntermediate3DBatch)
        {
            nds->GPU.GPU3D.EndNSMLDiscardedGeometryBatch();
            nds->NSMLGameTickProbeIntermediate3DBatch = false;
        }
    }
    if (cfg.DeferLCD && nds->ARM9Read32(historyTargetAddr) != 0)
    {
        if (marker == 1 && historyIndex == 0 && !state.LCDDeferred)
        {
            state.LCDDeferStartTimestamp = nds->GetSysClockCycles(0);
            nds->NSMLGameTickProbeDeferLCD = true;
            state.LCDDeferred = true;
        }
        else if (marker == 6 && historyIndex >= historyCount && state.LCDDeferred)
        {
            const u64 elapsed = nds->GetSysClockCycles(0) - state.LCDDeferStartTimestamp;
            nds->SchedList[Event_LCD].Timestamp += elapsed;
            nds->NSMLGameTickProbeDeferLCD = false;
            state.LCDDeferred = false;
        }
    }
    if (cfg.JitProfileEnabled)
    {
        if (cfg.JitRenderProfileEnabled)
        {
            if (marker == 4 && historyIndex >= historyCount)
                nds->JIT.ResetExecutionProfile();
            if (marker == 5 && historyIndex >= historyCount)
                nds->JIT.DumpExecutionProfile(cfg.JitProfileFile, cfg.Role.c_str());
        }
        else
        {
            if (marker == 11 && historyIndex == 1)
                nds->JIT.ResetExecutionProfile();
            if (marker == 12 && historyIndex >= historyCount)
                nds->JIT.DumpExecutionProfile(cfg.JitProfileFile, cfg.Role.c_str());
        }
    }
    if (cfg.JitRenderStateDumpEnabled && historyIndex >= historyCount &&
        (marker == 4 || marker == 5) && !cfg.OutputDir.empty() && nds->MainRAM)
    {
        auto hashBytes = [](const u8* data, size_t length)
        {
            u64 hash = 1469598103934665603ULL;
            for (size_t i = 0; i < length; i++)
            {
                hash ^= data[i];
                hash *= 1099511628211ULL;
            }
            return hash;
        };
        u64 vramHash = 1469598103934665603ULL;
        for (int bank = 0; bank < 9; bank++)
        {
            vramHash ^= hashBytes(nds->GPU.VRAM[bank], nds->GPU.VRAMMask[bank] + 1);
            vramHash *= 1099511628211ULL;
        }
        if (cfg.JitRenderStateFile)
        {
            const auto& gpu3d = nds->GPU.GPU3D;
            fprintf(cfg.JitRenderStateFile,
                "%s,%s,%u,%u,%u,%d,%04X,%04X,%016llX,%016llX,%016llX,%08X,%llu,%d,%u,%u,%u,%u,%u,%u,%u\n",
                cfg.Role.c_str(), marker == 4 ? "begin" : "end", nds->NumFrames,
                nds->GPU.VCount, nds->GPU.TotalScanlines, nds->GPU.GetRenderer().GetBackBufferIndex(),
                nds->GPU.DispStat[0], nds->GPU.DispStat[1],
                static_cast<unsigned long long>(hashBytes(nds->GPU.OAM, sizeof(nds->GPU.OAM))),
                static_cast<unsigned long long>(hashBytes(nds->GPU.Palette, sizeof(nds->GPU.Palette))),
                static_cast<unsigned long long>(vramHash), gpu3d.GXStat,
                static_cast<unsigned long long>(gpu3d.Timestamp), gpu3d.CycleCount,
                gpu3d.CmdFIFO.Level(), gpu3d.CmdPIPE.Level(), gpu3d.CurRAMBank,
                gpu3d.NumVertices, gpu3d.NumPolygons, gpu3d.RenderNumPolygons, gpu3d.FlushRequest);
            fflush(cfg.JitRenderStateFile);
        }
        const std::string dumpPath = cfg.OutputDir + "/rom-game-tick-render-frame" +
            std::to_string(nds->NumFrames) + "-" + (marker == 4 ? "begin-" : "end-") +
            cfg.Role + ".bin";
        if (FILE* file = fopen(dumpPath.c_str(), "wb"))
        {
            const u32 length = std::min(nds->MainRAMMask + 1, 0x400000u);
            fwrite(nds->MainRAM, 1, length, file);
            fclose(file);
        }
        const std::string oamPath = cfg.OutputDir + "/rom-game-tick-render-frame" +
            std::to_string(nds->NumFrames) + "-" + (marker == 4 ? "begin-" : "end-") +
            cfg.Role + "-oam.bin";
        if (FILE* file = fopen(oamPath.c_str(), "wb"))
        {
            fwrite(nds->GPU.OAM, 1, sizeof(nds->GPU.OAM), file);
            fclose(file);
        }
    }
    if (!cfg.StageEnabled || !cfg.LogFile)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (!state.HasOrigin)
    {
        state.Origin = now;
        state.HasOrigin = true;
    }
    const auto wallUs = std::chrono::duration_cast<std::chrono::microseconds>(now - state.Origin);
    fprintf(cfg.LogFile, "%s,%llu,%s,%u,%u,%u,%u,%u,%u,%u,%lld,%llu,%llu,%llu\n",
        cfg.Role.c_str(), static_cast<unsigned long long>(state.Sequence++), stage, marker,
        nds->NumFrames, nds->ARM9Read32(gameFrameAddr), historyEnabled,
        historyIndex, historyCount,
        nds->ARM9Read32(activeAddr), static_cast<long long>(wallUs.count()),
        static_cast<unsigned long long>(nds->GetSysClockCycles(0)),
        static_cast<unsigned long long>(nds->ARM9Timestamp),
        static_cast<unsigned long long>(nds->ARM7Timestamp));
    if (marker == frameEndMarker)
        fflush(cfg.LogFile);
}

static void HandleNSMLRomGameTickProbeFrameBoundary(NDS* nds, bool beforeFrame)
{
    constexpr u32 requestAddr = 0x02001AC0;
    constexpr u32 activeAddr = 0x02001AC4;
    constexpr u32 magicAddr = 0x02001AC8;
    constexpr u32 historyEnabledAddr = 0x02001ACC;
    constexpr u32 historyIndexAddr = 0x02001AD0;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyTargetAddr = 0x02001AD8;
    constexpr u32 historyStartFrameAddr = 0x02001ADC;
    constexpr u32 historyAddr = 0x023C1300;
    constexpr u32 scratchTickAddr = 0x023C1200;
    constexpr u32 scratchKeysAddr = 0x023C1208;
    constexpr u32 scratchPacketsAddr = 0x023C1240;
    constexpr u32 historyEntrySize = 16;
    constexpr u32 magic = 0x32505447;
    constexpr std::array<u16, 7> player0Keys = {0x010, 0x011, 0x000, 0x020, 0x001, 0x810, 0x000};
    constexpr std::array<u16, 7> player1Keys = {0x020, 0x022, 0x000, 0x010, 0x002, 0x820, 0x000};

    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        u32 StartFrame = 900;
        u32 ExtraTicks = 1;
        u32 StartOffset = 2;
        u16 BaseTick = 0x51;
        bool GameRAMRollback = false;
        bool SkipStateDumps = false;
        std::string Role = "local";
        std::string TargetRole = "host";
        std::string OutputDir;
        FILE* LogFile = nullptr;
    };
    struct State
    {
        struct GameRAMSnapshot
        {
            u32 DisplayFrame = 0;
            u32 GameFrame = 0;
            u16 Tick = 0;
            u16 Keys0 = 0;
            u16 Keys1 = 0;
            u32 Player0Metadata = 0;
            u32 Player1Metadata = 0;
            std::vector<u8> MainRAM;
        };

        bool Armed = false;
        bool AfterDumped = false;
        bool BeforeRecoveryDumped = false;
        bool Done = false;
        u32 AfterDisplayFrame = 0;
        u32 AfterGameFrame = 0;
        u32 InputSequenceHash = 2166136261u;
        bool FrameTimingActive = false;
        bool HistoryTransactionInFlight = false;
        bool NormalControlPending = false;
        u32 FrameHistoryIndex = 0;
        u32 TransactionTicks = 0;
        std::chrono::steady_clock::time_point FrameStartedAt;
        unsigned long long HistoryRunWallUs = 0;
        unsigned long long HistoryRunMaxUs = 0;
        unsigned long long LastHistoryRunWallUs = 0;
        u32 HistoryRunFrames = 0;
        unsigned long long SnapshotSaveMaxUs = 0;
        unsigned long long GameRAMRestoreUs = 0;
        u32 GameRAMRestoreBytes = 0;
        u32 GameRAMRestoreFrame = 0;
        std::vector<GameRAMSnapshot> GameRAMSnapshots;
        u32 NextGameRAMSnapshot = 0;
    };
    static Config cfg;
    static State state;
    if (!cfg.Checked)
    {
        cfg.Enabled = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_FRAME_BOUNDARY") != nullptr;
        if (const char* value = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_START_FRAME"))
            cfg.StartFrame = static_cast<u32>(strtoul(value, nullptr, 0));
        if (const char* value = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_EXTRA_TICKS"))
            cfg.ExtraTicks = std::clamp(static_cast<u32>(strtoul(value, nullptr, 0)), 1u, 7u);
        if (const char* value = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_BASE_TICK"))
            cfg.BaseTick = static_cast<u16>(strtoul(value, nullptr, 0) & 0xFFFF);
        cfg.GameRAMRollback = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_GAME_RAM_ROLLBACK") != nullptr;
        cfg.SkipStateDumps = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_SKIP_STATE_DUMPS") != nullptr;
        if (const char* value = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_START_OFFSET"))
            cfg.StartOffset = std::clamp(static_cast<u32>(strtoul(value, nullptr, 0)), 1u, 4u);
        if (const char* value = getenv("MELONDS_NSML_ROLE"))
            cfg.Role = value;
        if (const char* value = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_TARGET_ROLE"))
            cfg.TargetRole = value;
        if (const char* value = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_DIR"))
            cfg.OutputDir = value;
        if (cfg.Enabled && !cfg.OutputDir.empty())
        {
            const std::string logPath = cfg.OutputDir + "/rom-game-tick-probe-" + cfg.Role + ".csv";
            cfg.LogFile = fopen(logPath.c_str(), "w");
            if (cfg.LogFile)
            {
                fprintf(cfg.LogFile,
                    "role,frame,phase,main_ram_hash,game_frame_counter,arm9_timestamp,arm7_timestamp,request,active,extra_ticks_seen,input_sequence_hash,scratch_tick,scratch_keys0,scratch_keys1,history_enabled,history_index,history_count,history_run_wall_us,history_run_max_us,history_run_frames,last_history_run_wall_us,snapshot_save_max_us,game_ram_restore_us,game_ram_restore_bytes\n");
                fflush(cfg.LogFile);
            }
        }
        cfg.Checked = true;
    }
    if (!cfg.Enabled || !nds || !nds->MainRAM || cfg.OutputDir.empty() || state.Done)
        return;

    const bool isTarget = cfg.TargetRole == "both" || cfg.Role == cfg.TargetRole;
    auto hashMainRAM = [&]()
    {
        u64 hash = 1469598103934665603ull;
        const u32 length = std::min(nds->MainRAMMask + 1, 0x400000u);
        for (u32 offset = 0; offset < length; offset++)
        {
            hash ^= nds->MainRAM[offset];
            hash *= 1099511628211ull;
        }
        return hash;
    };
    auto dump = [&](const char* phase)
    {
        if (cfg.LogFile)
        {
            fprintf(cfg.LogFile, "%s,%u,%s,%016llX,%u,%llu,%llu,%u,%u,%u,%08X,%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%llu,%llu,%llu,%u\n",
                cfg.Role.c_str(), nds->NumFrames, phase,
                static_cast<unsigned long long>(cfg.SkipStateDumps ? 0 : hashMainRAM()),
                nds->ARM9Read32(0x0208B668),
                static_cast<unsigned long long>(nds->ARM9Timestamp),
                static_cast<unsigned long long>(nds->ARM7Timestamp),
                nds->ARM9Read32(requestAddr), nds->ARM9Read32(activeAddr),
                nds->ARM9Read32(historyIndexAddr), state.InputSequenceHash,
                nds->ARM9Read16(scratchTickAddr), nds->ARM9Read16(scratchKeysAddr),
                nds->ARM9Read16(scratchKeysAddr + 2), nds->ARM9Read32(historyEnabledAddr),
                nds->ARM9Read32(historyIndexAddr), nds->ARM9Read32(historyCountAddr),
                state.HistoryRunWallUs, state.HistoryRunMaxUs, state.HistoryRunFrames,
                state.LastHistoryRunWallUs, state.SnapshotSaveMaxUs,
                state.GameRAMRestoreUs, state.GameRAMRestoreBytes);
            fflush(cfg.LogFile);
        }
        if (cfg.SkipStateDumps)
            return;
        char filename[1024];
        snprintf(filename, sizeof(filename), "%s/rom-game-tick-probe-%s-frame%u-%s.bin",
            cfg.OutputDir.c_str(), cfg.Role.c_str(), nds->NumFrames, phase);
        if (FILE* file = fopen(filename, "wb"))
        {
            const u32 length = std::min(nds->MainRAMMask + 1, 0x400000u);
            fwrite(nds->MainRAM, 1, length, file);
            fclose(file);
        }
    };

    if (cfg.GameRAMRollback && beforeFrame && !state.Done)
    {
        const u32 length = std::min(nds->MainRAMMask + 1, 0x400000u);
        const u32 capacity = cfg.ExtraTicks + 3;
        if (state.GameRAMSnapshots.size() != capacity)
        {
            state.GameRAMSnapshots.resize(capacity);
            for (auto& snapshot : state.GameRAMSnapshots)
                snapshot.MainRAM.resize(length);
            state.NextGameRAMSnapshot = 0;
        }
        const auto saveStart = std::chrono::steady_clock::now();
        State::GameRAMSnapshot& snapshot =
            state.GameRAMSnapshots[state.NextGameRAMSnapshot];
        snapshot.DisplayFrame = nds->NumFrames;
        snapshot.GameFrame = nds->ARM9Read32(0x0208B668);
        snapshot.Tick = nds->ARM9Read16(scratchTickAddr);
        snapshot.Keys0 = nds->ARM9Read16(scratchKeysAddr);
        snapshot.Keys1 = nds->ARM9Read16(scratchKeysAddr + 2);
        snapshot.Player0Metadata = nds->ARM9Read32(scratchPacketsAddr + 4);
        snapshot.Player1Metadata = nds->ARM9Read32(scratchPacketsAddr + 0x40 + 4);
        memcpy(snapshot.MainRAM.data(), nds->MainRAM, length);
        const auto saveElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - saveStart);
        state.SnapshotSaveMaxUs = std::max(
            state.SnapshotSaveMaxUs,
            static_cast<unsigned long long>(saveElapsed.count()));
        state.NextGameRAMSnapshot =
            (state.NextGameRAMSnapshot + 1) % capacity;
    }

    if (!state.Armed && beforeFrame && nds->NumFrames >= cfg.StartFrame &&
        nds->ARM9Read32(magicAddr) == magic && nds->ARM9Read32(0x02085A18) == 9 &&
        nds->ARM9Read32(0x02085A84) == 1)
    {
        state.Armed = true;
        dump("before-renderless-ab-tick");
        struct ReplayInput
        {
            u32 DisplayFrame;
            u16 Tick;
            u16 Keys0;
            u16 Keys1;
            u32 Player0Metadata;
            u32 Player1Metadata;
        };
        std::vector<ReplayInput> replayInputs;
        bool gameRAMRestoreReady = !cfg.GameRAMRollback || !isTarget;
        if (cfg.GameRAMRollback && isTarget && nds->NumFrames >= cfg.ExtraTicks)
        {
            const u32 restoreFrame = nds->NumFrames - cfg.ExtraTicks;
            auto restoreIt = std::find_if(
                state.GameRAMSnapshots.begin(), state.GameRAMSnapshots.end(),
                [restoreFrame](const State::GameRAMSnapshot& snapshot)
                {
                    return snapshot.DisplayFrame == restoreFrame;
                });
            if (restoreIt != state.GameRAMSnapshots.end())
            {
                for (const auto& snapshot : state.GameRAMSnapshots)
                {
                    if (snapshot.DisplayFrame >= restoreFrame &&
                        snapshot.DisplayFrame <= nds->NumFrames)
                        replayInputs.push_back(
                            {snapshot.DisplayFrame, snapshot.Tick, snapshot.Keys0, snapshot.Keys1,
                                snapshot.Player0Metadata, snapshot.Player1Metadata});
                }
                std::sort(replayInputs.begin(), replayInputs.end(),
                    [](const ReplayInput& left, const ReplayInput& right)
                    {
                        return left.DisplayFrame < right.DisplayFrame;
                    });
                if (replayInputs.size() == cfg.ExtraTicks + 1 &&
                    restoreIt->MainRAM.size() == std::min(nds->MainRAMMask + 1, 0x400000u))
                {
                    nds->NSMLGameRAMRestoreData = restoreIt->MainRAM.data();
                    nds->NSMLGameRAMRestoreLength = static_cast<u32>(restoreIt->MainRAM.size());
                    nds->NSMLGameRAMRestorePending = true;
                    state.GameRAMRestoreFrame = restoreIt->GameFrame;
                    gameRAMRestoreReady = true;
                }
                else
                {
                    replayInputs.clear();
                }
            }
        }
        if (!gameRAMRestoreReady)
        {
            printf("NSMB ROM rollback: game RAM checkpoint unavailable role=%s frame=%u depth=%u\n",
                cfg.Role.c_str(), nds->NumFrames, cfg.ExtraTicks);
            fflush(stdout);
            state.Done = true;
            return;
        }
        if (cfg.GameRAMRollback && !isTarget)
        {
            state.TransactionTicks = 0;
            state.NormalControlPending = true;
            nds->ARM9Write32(historyEnabledAddr, 0);
            nds->ARM9Write32(historyIndexAddr, 0);
            nds->ARM9Write32(historyCountAddr, 0);
            nds->ARM9Write32(historyTargetAddr, 0);
            nds->ARM9Write32(activeAddr, 0);
            nds->ARM9Write32(requestAddr, 0);
            return;
        }
        state.TransactionTicks = cfg.GameRAMRollback
            ? cfg.ExtraTicks + 1
            : cfg.ExtraTicks;
        const u16 baseTick = cfg.BaseTick;
        nds->ARM9Write32(historyIndexAddr, 0);
        nds->ARM9Write32(historyCountAddr, state.TransactionTicks);
        nds->ARM9Write32(historyTargetAddr, isTarget ? 1 : 0);
        nds->ARM9Write32(historyStartFrameAddr,
            cfg.GameRAMRollback
                ? state.GameRAMRestoreFrame
                : nds->ARM9Read32(0x0208B668) + cfg.StartOffset);
        for (u32 index = 0; index < state.TransactionTicks; index++)
        {
            const bool useReplayInput = replayInputs.size() == state.TransactionTicks;
            const u16 tick = useReplayInput
                ? replayInputs[index].Tick
                : static_cast<u16>((baseTick + index) & 0xFFFF);
            const u16 keys0 = useReplayInput
                ? replayInputs[index].Keys0
                : player0Keys[index];
            const u16 keys1 = useReplayInput
                ? replayInputs[index].Keys1
                : player1Keys[index];
            const u32 player0Metadata = useReplayInput
                ? replayInputs[index].Player0Metadata
                : 0;
            const u32 player1Metadata = useReplayInput
                ? replayInputs[index].Player1Metadata
                : 0;
            const u32 entryAddr = historyAddr + index * historyEntrySize;
            nds->ARM9Write16(entryAddr, tick);
            nds->ARM9Write16(entryAddr + 2, keys0);
            nds->ARM9Write16(entryAddr + 4, keys1);
            nds->ARM9Write16(entryAddr + 6, 0);
            nds->ARM9Write32(entryAddr + 8, player0Metadata);
            nds->ARM9Write32(entryAddr + 12, player1Metadata);
            for (const u32 value : {
                     static_cast<u32>(tick), static_cast<u32>(keys0),
                     static_cast<u32>(keys1), player0Metadata, player1Metadata})
            {
                for (u32 shift = 0; shift < 32; shift += 8)
                {
                    state.InputSequenceHash ^= (value >> shift) & 0xFF;
                    state.InputSequenceHash *= 16777619u;
                }
            }
        }
        nds->ARM9Write32(historyEnabledAddr, 1);
        nds->ARM9Write32(activeAddr, 0);
        nds->ARM9Write32(requestAddr, 0);
        state.FrameTimingActive = true;
        state.FrameHistoryIndex = 0;
        state.FrameStartedAt = std::chrono::steady_clock::now();
        return;
    }
    if (!state.Armed)
        return;

    if (!beforeFrame && state.FrameTimingActive)
    {
        state.GameRAMRestoreUs = nds->NSMLGameRAMRestoreUs;
        state.GameRAMRestoreBytes = nds->NSMLGameRAMRestoreBytes;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - state.FrameStartedAt);
        const u32 historyIndex = nds->ARM9Read32(historyIndexAddr);
        const u32 historyCount = nds->ARM9Read32(historyCountAddr);
        const u32 historyEndFrame = nds->ARM9Read32(historyStartFrameAddr) + historyCount;
        const bool historyAdvanced = historyIndex > state.FrameHistoryIndex;
        const bool historyCompleted = historyIndex >= historyCount &&
            nds->ARM9Read32(0x0208B668) >= historyEndFrame;
        if (historyAdvanced || state.HistoryTransactionInFlight)
        {
            state.LastHistoryRunWallUs = static_cast<unsigned long long>(elapsed.count());
            state.HistoryRunWallUs += state.LastHistoryRunWallUs;
            state.HistoryRunMaxUs = std::max(state.HistoryRunMaxUs, state.LastHistoryRunWallUs);
            state.HistoryRunFrames++;
        }
        state.HistoryTransactionInFlight =
            (historyAdvanced || state.HistoryTransactionInFlight) && !historyCompleted;
        state.FrameTimingActive = false;
    }
    else if (beforeFrame)
    {
        state.FrameTimingActive = true;
        state.FrameHistoryIndex = nds->ARM9Read32(historyIndexAddr);
        state.FrameStartedAt = std::chrono::steady_clock::now();
    }

    if (!beforeFrame && state.NormalControlPending && !state.AfterDumped)
    {
        dump("after-normal-control-tick");
        state.NormalControlPending = false;
        state.AfterDumped = true;
        state.AfterDisplayFrame = nds->NumFrames;
        state.AfterGameFrame = nds->ARM9Read32(0x0208B668);
        return;
    }
    if (!beforeFrame && !state.AfterDumped &&
        nds->ARM9Read32(historyIndexAddr) >= state.TransactionTicks &&
        nds->ARM9Read32(0x0208B668) >=
            nds->ARM9Read32(historyStartFrameAddr) + state.TransactionTicks)
    {
        dump(isTarget ? "after-renderless-tick" : "after-normal-control-tick");
        state.AfterDumped = true;
        state.AfterDisplayFrame = nds->NumFrames;
        state.AfterGameFrame = nds->ARM9Read32(0x0208B668);
        return;
    }
    if (beforeFrame && state.AfterDumped && !state.BeforeRecoveryDumped &&
        nds->NumFrames == state.AfterDisplayFrame)
    {
        dump("before-recovery-normal-tick");
        // Run exactly one deterministic recovery tick on both peers.  Letting the
        // target leave the gate entirely can execute two game loops inside one DS
        // display frame, which makes a frame-boundary comparison off by one tick.
        const u16 recoveryTick = static_cast<u16>(nds->ARM9Read16(historyAddr) + cfg.ExtraTicks);
        nds->ARM9Write16(historyAddr, recoveryTick);
        nds->ARM9Write16(historyAddr + 2, 0);
        nds->ARM9Write16(historyAddr + 4, 0);
        nds->ARM9Write16(historyAddr + 6, 0);
        nds->ARM9Write32(historyAddr + 8, 0);
        nds->ARM9Write32(historyAddr + 12, 0);
        nds->ARM9Write32(historyIndexAddr, 0);
        nds->ARM9Write32(historyCountAddr, 1);
        nds->ARM9Write32(historyTargetAddr, isTarget ? 1 : 0);
        nds->ARM9Write32(historyStartFrameAddr, state.AfterGameFrame);
        nds->ARM9Write32(historyEnabledAddr, 1);
        nds->ARM9Write32(activeAddr, 0);
        nds->ARM9Write32(requestAddr, 0);
        state.FrameHistoryIndex = 0;
        state.BeforeRecoveryDumped = true;
        return;
    }
    if (!beforeFrame && state.BeforeRecoveryDumped &&
        nds->ARM9Read32(0x0208B668) >= state.AfterGameFrame + 1)
    {
        dump(isTarget ? "after-recovery-normal-tick" : "after-second-normal-control-tick");
        nds->ARM9Write32(historyEnabledAddr, 0);
        nds->ARM9Write32(historyTargetAddr, 0);
        state.Done = true;
    }
}

template <CPUExecuteMode cpuMode>
u32 NDS::RunFrame()
{
    HandleNSMLRomGameTickProbeFrameBoundary(this, true);
    RecordNSMLRomGameTickProbeStage(this, 0x100);
    Current = this;

    FrameStartTimestamp = SysTimestamp;

    GPU.TotalScanlines = 0;

    LagFrameFlag = true;
    bool runFrame = Running && !(CPUStop & CPUStop_Sleep);
    while (Running)
    {
        u64 frametarget = SysTimestamp + 560190;

        if (CPUStop & CPUStop_Sleep)
        {
            // we are running in sleep mode
            // we still need to run the RTC during this mode
            // we also keep outputting audio, so that frontends using audio sync don't skyrocket to 1000+FPS

            while (Running && (SysTimestamp < frametarget))
            {
                u64 target = NextTargetSleep();
                if (target > frametarget)
                    target = frametarget;

                ARM9Timestamp = target << ARM9ClockShift;
                ARM7Timestamp = target;
                TimerTimestamp[0] = target;
                TimerTimestamp[1] = target;
                GPU.GPU3D.Timestamp = target;
                RunSystemSleep(target);

                if (!(CPUStop & CPUStop_Sleep))
                    break;
            }

            if (SysTimestamp >= frametarget)
                GPU.BlankFrame();
        }
        else
        {
            if (cpuMode == CPUExecuteMode::InterpreterGDB)
            {
                ARM9.CheckGdbIncoming();
                ARM7.CheckGdbIncoming();
            }

            if (!(CPUStop & CPUStop_Wakeup))
            {
                GPU.StartFrame();
            }
            CPUStop &= ~CPUStop_Wakeup;

            while (Running && GPU.TotalScanlines==0)
            {
                u64 target = NextTarget();
                ARM9Target = target << ARM9ClockShift;
                CurCPU = 0;

                if (CPUStop & CPUStop_GXStall)
                {
                    // GXFIFO stall
                    s32 cycles = GPU.GPU3D.CyclesToRunFor();

                    ARM9Timestamp = std::min(ARM9Target, ARM9Timestamp+(cycles<<ARM9ClockShift));
                }
                else if (CPUStop & CPUStop_DMA9)
                {
                    DMAs[0].Run();
                    if (!(CPUStop & CPUStop_GXStall)) DMAs[1].Run();
                    if (!(CPUStop & CPUStop_GXStall)) DMAs[2].Run();
                    if (!(CPUStop & CPUStop_GXStall)) DMAs[3].Run();
                    if (ConsoleType == 1)
                    {
                        auto& dsi = dynamic_cast<melonDS::DSi&>(*this);
                        dsi.RunNDMAs(0);
                    }
                }
                else
                {
                    ARM9.Execute<cpuMode>();
                }

                RunTimers(0);
                GPU.GPU3D.Run();

                target = ARM9Timestamp >> ARM9ClockShift;
                CurCPU = 1;

                while (ARM7Timestamp < target)
                {
                    ARM7Target = target; // might be changed by a reschedule

                    if (CPUStop & CPUStop_DMA7)
                    {
                        DMAs[4].Run();
                        DMAs[5].Run();
                        DMAs[6].Run();
                        DMAs[7].Run();
                        if (ConsoleType == 1)
                        {
                            auto& dsi = dynamic_cast<melonDS::DSi&>(*this);
                            dsi.RunNDMAs(1);
                        }
                    }
                    else
                    {
                        ARM7.Execute<cpuMode>();
                    }

                    RunTimers(1);
                }

                RunSystem(target);

                if (CPUStop & CPUStop_Sleep)
                {
                    break;
                }
            }
        }

        if (GPU.TotalScanlines == 0)
            continue;

#ifdef DEBUG_CHECK_DESYNC
        Log(LogLevel::Debug, "[%08X%08X] ARM9=%ld, ARM7=%ld, GPU=%ld\n",
            (u32)(SysTimestamp>>32), (u32)SysTimestamp,
            (ARM9Timestamp>>1)-SysTimestamp,
            ARM7Timestamp-SysTimestamp,
            GPU.GPU3D.Timestamp-SysTimestamp);
#endif
        break;
    }

    // Ensure the last audio samples produced for this frame are available to the frontend immediately
    SPU.BufferAudio();
    RecordNSMLRomGameTickProbeStage(this, 0x101);

    // In the context of TASes, frame count is traditionally the primary measure of emulated time,
    // so it needs to be tracked even if NDS is powered off.
    NumFrames++;
    HandleNSMLRomGameTickProbeFrameBoundary(this, false);
    if (LagFrameFlag)
        NumLagFrames++;

    if (Running)
        return GPU.TotalScanlines;
    else
        return 263;
}

u32 NDS::RunFrame()
{
#ifdef JIT_ENABLED
    if (EnableJIT)
        return RunFrame<CPUExecuteMode::JIT>();
    else
#endif
#ifdef GDBSTUB_ENABLED
    if (EnableGDBStub)
    {
        return RunFrame<CPUExecuteMode::InterpreterGDB>();
    } else
#endif
    {
        return RunFrame<CPUExecuteMode::Interpreter>();
    }
}

void NDS::Reschedule(u64 target)
{
    if (CurCPU == 0)
    {
        if (NSMLGameTickProbeFreezeScheduler)
            return;
        if (target < (ARM9Target >> ARM9ClockShift))
            ARM9Target = (target << ARM9ClockShift);
    }
    else
    {
        if (target < ARM7Target)
            ARM7Target = target;
    }
}

void NDS::CaptureNSMLGameTickProbeSchedulerState(NSMLGameTickProbeSchedulerState& state) const
{
    std::copy(std::begin(SchedList), std::end(SchedList), state.Events.begin());
    state.Mask = SchedListMask;
}

void NDS::ApplyNSMLPendingGameRAMRestore()
{
    if (!NSMLGameRAMRestorePending)
    {
        constexpr u32 historyEnabledAddr = 0x02001ACC;
        constexpr u32 historyIndexAddr = 0x02001AD0;
        constexpr u32 historyCountAddr = 0x02001AD4;
        if (ARM9Read32(historyEnabledAddr) != 0)
        {
            const u32 historyIndex = ARM9Read32(historyIndexAddr);
            if (historyIndex >= ARM9Read32(historyCountAddr))
            {
                NSMLGameRAMHistoryReachedExitGate = true;
                ARM9Write32(historyEnabledAddr, 0);
                return;
            }
            // Rebuild checkpoints invalidated by the correction. A later
            // mismatch in the same input burst must start from corrected RAM,
            // not from a snapshot that still contains the earlier prediction.
            if (historyIndex != 0 && NSMLGameRAMReplayDisplayStartFrame != 0xFFFFFFFF)
            {
                CaptureNSMLGameRAMCheckpointAtGate(
                    NSMLGameRAMReplayDisplayStartFrame + historyIndex);
            }
            return;
        }
        if (!NSMLGameRAMHistoryReachedExitGate)
            CaptureNSMLGameRAMCheckpointAtGate(
                NSMLGameRAMCheckpointTimeline.CaptureFrame(NumFrames));
        return;
    }
    if (!MainRAM)
        return;

    const u32 length = std::min(MainRAMMask + 1, 0x400000u);
    if (!NSMLGameRAMRestoreData || NSMLGameRAMRestoreLength != length)
    {
        NSMLGameRAMRestorePending = false;
        NSMLGameRAMRestoreData = nullptr;
        NSMLGameRAMRestoreLength = 0;
        NSMLGameRAMRestoreOwnedBuffer.clear();
        return;
    }

    constexpr u32 controlOffset = 0x1AC0;
    // The control header remains in the ROM's low scratch area. Replay history
    // lives in a dedicated high scratch interval and is preserved separately.
    constexpr u32 controlLength = 0x20;
    constexpr u32 historyOffset = 0x3C1300;
    constexpr u32 historyLength = 12 * 16;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyStartFrameAddr = 0x02001ADC;
    constexpr u32 gameFrameAddr = 0x0208B668;
    // NitroSDK owns this MainRAM interval. In particular it contains
    // OSi_CurrentThreadPtr/OSi_ThreadInfo (0x020942AC) and the live tick/alarm
    // queue (0x020945A4/0x020945B8), followed by filesystem and WM state. These
    // structures belong to the current outer emulator frame, not the rewound
    // game tick. Restoring them makes the final replay tick return through a
    // stale OS scheduler/alarm state and leaves the game loop stopped.
    constexpr u32 sdkRuntimeOffset = 0x942A0;
    constexpr u32 sdkRuntimeEnd = 0x98000;

    const u32 configuredHistoryCount = ARM9Read32(historyCountAddr);
    const u32 historyStartFrame = ARM9Read32(historyStartFrameAddr);
    const u32 preRestoreGameFrame = ARM9Read32(gameFrameAddr);
    // History count is expressed in the generation-local display timeline and
    // deliberately includes the input gate after the current logical frame.
    // Clamping it to the pre-restore game frame drops that final tick. A first
    // correction can appear to recover, but its rebuilt checkpoint ring then
    // starts the next correction one game tick behind.

    std::array<u8, controlLength> control {};
    memcpy(control.data(), MainRAM + controlOffset, control.size());
    std::array<u8, historyLength> history {};
    static_assert(historyOffset + historyLength <= 0x400000);
    memcpy(history.data(), MainRAM + historyOffset, history.size());

    if (getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_STAGE_TRACE"))
    {
        printf("NSMB ROM-loop gate state: frame=%u gameFrame=%u start=%u "
               "configuredTicks=%u gateTicks=%u currentSP=%08X "
               "currentLR=%08X currentCPSR=%08X\n",
               NumFrames, preRestoreGameFrame, historyStartFrame,
               configuredHistoryCount, ARM9Read32(historyCountAddr),
               ARM9.R[13], ARM9.R[14], ARM9.CPSR);
        fflush(stdout);
    }

    const auto restoreStart = std::chrono::steady_clock::now();
    static_assert(sdkRuntimeOffset < sdkRuntimeEnd);
    static_assert(sdkRuntimeEnd <= 0x400000);
    const u32 beforeSDK = std::min(length, sdkRuntimeOffset);
    memcpy(MainRAM, NSMLGameRAMRestoreData, beforeSDK);
    if (length > sdkRuntimeEnd)
    {
        memcpy(MainRAM + sdkRuntimeEnd, NSMLGameRAMRestoreData + sdkRuntimeEnd,
               length - sdkRuntimeEnd);
    }
    memcpy(MainRAM + controlOffset, control.data(), control.size());
    memcpy(MainRAM + historyOffset, history.data(), history.size());
    const auto restoreElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - restoreStart);
    NSMLGameRAMRestoreUs = static_cast<unsigned long long>(restoreElapsed.count());
    NSMLGameRAMRestoreBytes = length;
    NSMLGameRAMRestorePending = false;
    NSMLGameRAMRestoreData = nullptr;
    NSMLGameRAMRestoreLength = 0;
    NSMLGameRAMRestoreOwnedBuffer.clear();
}

void NDS::SetNSMLGameRAMCheckpointFrame(u32 logicalFrame)
{
    if (!NSMLGameRAMCheckpointTimeline.SetLogicalFrame(logicalFrame))
        return;

    // Checkpoints captured while the start-ready handshake was still using
    // raw emulator frames do not belong to the generation-local logical
    // timeline. A backwards logical frame likewise marks a new generation.
    for (auto& checkpoint : NSMLGameRAMCheckpoints)
        checkpoint.Valid = false;
    NSMLNextGameRAMCheckpoint = 0;
}

void NDS::CaptureNSMLGameRAMCheckpointAtGate(u32 displayFrame)
{
    static const bool enabled = []
    {
        const char* backend = getenv("MELONDS_NSML_ROLLBACK_BACKEND");
        return backend &&
            (!strcmp(backend, "romloop") || !strcmp(backend, "rom-loop") ||
             !strcmp(backend, "slippi"));
    }();
    if (!enabled || !MainRAM)
        return;

    constexpr u32 capacity = 16;
    const u32 length = std::min(MainRAMMask + 1, 0x400000u);
    if (length != 0x400000u)
        return;
    if (NSMLGameRAMCheckpoints.size() != capacity)
    {
        NSMLGameRAMCheckpoints.resize(capacity);
        for (auto& checkpoint : NSMLGameRAMCheckpoints)
        {
            checkpoint.Valid = false;
            checkpoint.MainRAM.resize(length);
        }
        NSMLNextGameRAMCheckpoint = 0;
    }

    NSMLGameRAMCheckpoint& checkpoint =
        NSMLGameRAMCheckpoints[NSMLNextGameRAMCheckpoint];
    checkpoint.Valid = true;
    checkpoint.DisplayFrame = displayFrame;
    checkpoint.GameFrame = ARM9Read32(0x0208B668);
    memcpy(checkpoint.MainRAM.data(), MainRAM, length);
    NSMLNextGameRAMCheckpoint =
        (NSMLNextGameRAMCheckpoint + 1) % capacity;
}

bool NDS::CopyNSMLGameRAMCheckpointAtOrBefore(
    u32 frame, u32& checkpointFrame, u32& gameFrame, std::vector<u8>& image) const
{
    const NSMLGameRAMCheckpoint* best = nullptr;
    for (const auto& checkpoint : NSMLGameRAMCheckpoints)
    {
        if (!checkpoint.Valid || checkpoint.MainRAM.empty() || checkpoint.DisplayFrame > frame)
            continue;
        if (!best || checkpoint.DisplayFrame > best->DisplayFrame)
            best = &checkpoint;
    }
    if (!best)
        return false;
    checkpointFrame = best->DisplayFrame;
    gameFrame = best->GameFrame;
    image = best->MainRAM;
    return true;
}

u32 NDS::DiscardNSMLGameRAMCheckpointsAfter(u32 frame)
{
    u32 discarded = 0;
    u32 firstDiscarded = NSMLNextGameRAMCheckpoint;
    u32 firstDiscardedFrame = 0xFFFFFFFF;
    for (u32 index = 0; index < NSMLGameRAMCheckpoints.size(); index++)
    {
        auto& checkpoint = NSMLGameRAMCheckpoints[index];
        if (!checkpoint.Valid || checkpoint.DisplayFrame <= frame)
            continue;

        if (checkpoint.DisplayFrame < firstDiscardedFrame)
        {
            firstDiscarded = index;
            firstDiscardedFrame = checkpoint.DisplayFrame;
        }
        checkpoint.Valid = false;
        discarded++;
    }
    if (discarded != 0)
        NSMLNextGameRAMCheckpoint = firstDiscarded;
    return discarded;
}

bool NDS::IsNSMLGameRAMRollbackTransactionInFlight()
{
    constexpr u32 historyEnabledAddr = 0x02001ACC;
    return NSMLGameRAMRestorePending || NSMLGameRAMHistoryReachedExitGate ||
        ARM9Read32(historyEnabledAddr) != 0;
}

bool NDS::FinalizeNSMLGameRAMRollbackTransaction()
{
    constexpr u32 historyEnabledAddr = 0x02001ACC;
    constexpr u32 historyIndexAddr = 0x02001AD0;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyTargetAddr = 0x02001AD8;
    constexpr u32 historyStartFrameAddr = 0x02001ADC;
    constexpr u32 gameFrameAddr = 0x0208B668;
    const bool historyEnabled = ARM9Read32(historyEnabledAddr) != 0;
    const u32 historyCount = ARM9Read32(historyCountAddr);
    if (!NSMLGameRAMRollback::CanFinalizeTransaction(
            NSMLGameRAMRestorePending,
            NSMLGameRAMHistoryReachedExitGate,
            historyEnabled,
            ARM9Read32(historyIndexAddr),
            historyCount,
            ARM9Read32(gameFrameAddr),
            ARM9Read32(historyStartFrameAddr)))
        return false;

    // A ROM-loop replay can finish all requested game ticks before the guest
    // reaches the next input gate.  Finalize at this outer-frame boundary so a
    // newly arrived mismatch can arm a consecutive correction on the next
    // frame instead of aging past the configured prediction horizon.
    ARM9Write32(historyEnabledAddr, 0);
    ARM9Write32(historyIndexAddr, 0);
    ARM9Write32(historyCountAddr, 0);
    ARM9Write32(historyTargetAddr, 0);
    ARM9Write32(historyStartFrameAddr, 0);
    NSMLGameRAMHistoryReachedExitGate = false;
    NSMLGameRAMReplayDisplayStartFrame = 0xFFFFFFFF;
    return true;
}

bool NDS::ScheduleNSMLGameRAMRestore(std::vector<u8>&& image, u32 displayStartFrame)
{
    const u32 length = std::min(MainRAMMask + 1, 0x400000u);
    if (!MainRAM || image.size() != length || IsNSMLGameRAMRollbackTransactionInFlight())
        return false;

    NSMLGameRAMRestoreOwnedBuffer = std::move(image);
    NSMLGameRAMRestoreData = NSMLGameRAMRestoreOwnedBuffer.data();
    NSMLGameRAMRestoreLength = static_cast<u32>(NSMLGameRAMRestoreOwnedBuffer.size());
    NSMLGameRAMHistoryReachedExitGate = false;
    NSMLGameRAMReplayDisplayStartFrame = displayStartFrame;
    NSMLGameRAMRestorePending = true;
    return true;
}

void NDS::RestoreNSMLGameTickProbeSchedulerState(const NSMLGameTickProbeSchedulerState& state)
{
    std::copy(state.Events.begin(), state.Events.end(), std::begin(SchedList));
    SchedListMask = state.Mask;
}

void NDS::RegisterEventFuncs(u32 id, void* that, const std::initializer_list<EventFunc>& funcs)
{
    SchedEvent& evt = SchedList[id];

    evt.That = that;
    assert(funcs.size() <= MaxEventFunctions);
    int i = 0;
    for (EventFunc func : funcs)
    {
        evt.Funcs[i++] = func;        
    }
}

void NDS::UnregisterEventFuncs(u32 id)
{
    SchedEvent& evt = SchedList[id];

    evt.That = nullptr;
    for (int i = 0; i < MaxEventFunctions; i++)
        evt.Funcs[i] = nullptr;
}

void NDS::ScheduleEvent(u32 id, bool periodic, s32 delay, u32 funcid, u32 param)
{
    if (SchedListMask & (1<<id))
    {
        Log(LogLevel::Debug, "!! EVENT %d ALREADY SCHEDULED\n", id);
        return; 
    }

    SchedEvent& evt = SchedList[id];

    if (periodic)
        evt.Timestamp += delay;
    else
    {
        if (CurCPU == 0)
            evt.Timestamp = (ARM9Timestamp >> ARM9ClockShift) + delay;
        else
            evt.Timestamp = ARM7Timestamp + delay;
    }

    evt.FuncID = funcid;
    evt.Param = param;

    SchedListMask |= (1<<id);

    Reschedule(evt.Timestamp);
}

void NDS::CancelEvent(u32 id)
{
    SchedListMask &= ~(1<<id);
}


void NDS::TouchScreen(u16 x, u16 y)
{
    SPI.GetTSC()->SetTouchCoords(x, y);
}

void NDS::ReleaseScreen()
{
    SPI.GetTSC()->SetTouchCoords(0x000, 0xFFF);
}


void NDS::CheckKeyIRQ(u32 cpu, u32 oldkey, u32 newkey)
{
    u16 cnt = KeyCnt[cpu];
    if (!(cnt & (1<<14))) // IRQ disabled
        return;

    u32 mask = (cnt & 0x03FF);
    oldkey &= mask;
    newkey &= mask;

    bool oldmatch, newmatch;
    if (cnt & (1<<15))
    {
        // logical AND

        oldmatch = (oldkey == 0);
        newmatch = (newkey == 0);
    }
    else
    {
        // logical OR

        oldmatch = (oldkey != mask);
        newmatch = (newkey != mask);
    }

    if ((!oldmatch) && newmatch)
        SetIRQ(cpu, IRQ_Keypad);
}

void NDS::SetKeyMask(u32 mask)
{
    u32 key_lo = mask & 0x3FF;
    u32 key_hi = (mask >> 10) & 0x3;

    u32 oldkey = KeyInput;
    KeyInput &= 0xFFFCFC00;
    KeyInput |= key_lo | (key_hi << 16);

    CheckKeyIRQ(0, oldkey, KeyInput);
    CheckKeyIRQ(1, oldkey, KeyInput);
}

bool NDS::IsLidClosed() const
{
    if (KeyInput & (1<<23)) return true;
    return false;
}

void NDS::SetLidClosed(bool closed)
{
    if (closed)
    {
        KeyInput |= (1<<23);
    }
    else
    {
        KeyInput &= ~(1<<23);
        SetIRQ(1, IRQ_LidOpen);
    }
}

/*int ImportSRAM(u8* data, u32 length)
{
    return NDSCart::ImportSRAM(data, length);
}*/


void NDS::Halt()
{
    Log(LogLevel::Info, "Halt()\n");
    Running = false;
}


void NDS::SetExMemCnt(u32 cpu, u16 val, u16 mask)
{
    val &= mask;

    if (cpu == 0)
    {
        u16 oldval = ExMemCnt[0];

        // DSi has one extra bit (access rights for second cart slot)
        u16 rwmask = (ConsoleType == 1) ? 0x8CFF : 0x88FF;

        // bit13/14 are read-only
        ExMemCnt[0] = (ExMemCnt[0] & (~mask | 0x6000)) | (val & rwmask);
        ExMemCnt[1] = (ExMemCnt[0] & 0xFF80) | (ExMemCnt[1] & 0x007F);
        u16 diff = oldval ^ ExMemCnt[0];

        if (diff & 0xFF)
            SetGBASlotTimings();

        if (diff & (1<<11))
            NDSCartSlots[0]->SetCPUSelect((ExMemCnt[0] >> 11) & 0x1);

        if (ConsoleType == 1)
        {
            if (diff & (1<<10))
                NDSCartSlots[1]->SetCPUSelect((ExMemCnt[0] >> 10) & 0x1);
        }
    }
    else
    {
        if (!(mask & 0xFF))
            return;

        u16 oldval = ExMemCnt[1];
        ExMemCnt[1] = (ExMemCnt[1] & 0xFF80) | (val & 0x007F);
        u16 diff = oldval ^ ExMemCnt[1];

        if (diff & 0xFF)
            SetGBASlotTimings();
    }
}


void NDS::MapSharedWRAM(u8 val)
{
    if (val == WRAMCnt)
        return;

    JIT.Memory.RemapSWRAM();

    WRAMCnt = val;

    switch (WRAMCnt & 0x3)
    {
    case 0:
        SWRAM_ARM9.Mem = &SharedWRAM[0];
        SWRAM_ARM9.Mask = 0x7FFF;
        SWRAM_ARM7.Mem = NULL;
        SWRAM_ARM7.Mask = 0;
        break;

    case 1:
        SWRAM_ARM9.Mem = &SharedWRAM[0x4000];
        SWRAM_ARM9.Mask = 0x3FFF;
        SWRAM_ARM7.Mem = &SharedWRAM[0];
        SWRAM_ARM7.Mask = 0x3FFF;
        break;

    case 2:
        SWRAM_ARM9.Mem = &SharedWRAM[0];
        SWRAM_ARM9.Mask = 0x3FFF;
        SWRAM_ARM7.Mem = &SharedWRAM[0x4000];
        SWRAM_ARM7.Mask = 0x3FFF;
        break;

    case 3:
        SWRAM_ARM9.Mem = NULL;
        SWRAM_ARM9.Mask = 0;
        SWRAM_ARM7.Mem = &SharedWRAM[0];
        SWRAM_ARM7.Mask = 0x7FFF;
        break;
    }
}


void NDS::UpdateWifiTimings()
{
    if (PowerControl7 & 0x0002)
    {
        const int ntimings[4] = {10, 8, 6, 18};
        u16 val = WifiWaitCnt;

        SetARM7RegionTimings(0x04800, 0x04808, Mem7_Wifi0, 16, ntimings[val & 0x3], (val & 0x4) ? 4 : 6);
        SetARM7RegionTimings(0x04808, 0x04810, Mem7_Wifi1, 16, ntimings[(val>>3) & 0x3], (val & 0x20) ? 4 : 10);
    }
    else
    {
        SetARM7RegionTimings(0x04800, 0x04808, Mem7_Wifi0, 32, 1, 1);
        SetARM7RegionTimings(0x04808, 0x04810, Mem7_Wifi1, 32, 1, 1);
    }
}

void NDS::SetWifiWaitCnt(u16 val)
{
    if (WifiWaitCnt == val) return;

    WifiWaitCnt = val;
    UpdateWifiTimings();
}

void NDS::SetGBASlotTimings()
{
    const int ntimings[4] = {10, 8, 6, 18};
    const u16 openbus[4] = {0xFE08, 0x0000, 0x0000, 0xFFFF};

    u16 curcpu = (ExMemCnt[0] >> 7) & 0x1;
    u16 curcnt = ExMemCnt[curcpu];
    int ramN = ntimings[curcnt & 0x3];
    int romN = ntimings[(curcnt>>2) & 0x3];
    int romS = (curcnt & 0x10) ? 4 : 6;

    // GBA slot timings only apply on the selected side

    if (curcpu == 0)
    {
        SetARM9RegionTimings(0x08000, 0x0A000, Mem9_GBAROM, 16, romN, romS);
        SetARM9RegionTimings(0x0A000, 0x0B000, Mem9_GBARAM, 8, ramN, ramN);

        SetARM7RegionTimings(0x08000, 0x0A000, 0, 32, 1, 1);
        SetARM7RegionTimings(0x0A000, 0x0B000, 0, 32, 1, 1);
    }
    else
    {
        SetARM9RegionTimings(0x08000, 0x0A000, 0, 32, 1, 1);
        SetARM9RegionTimings(0x0A000, 0x0B000, 0, 32, 1, 1);

        SetARM7RegionTimings(0x08000, 0x0A000, Mem7_GBAROM, 16, romN, romS);
        SetARM7RegionTimings(0x0A000, 0x0B000, Mem7_GBARAM, 8, ramN, ramN);
    }

    // this open-bus implementation is a rough way of simulating the way values
    // lingering on the bus decay after a while, which is visible at higher waitstates
    // for example, the Cartridge Construction Kit relies on this to determine that
    // the GBA slot is empty

    GBACartSlot.SetOpenBusDecay(openbus[(curcnt>>2) & 0x3]);
}


void NDS::UpdateIRQ(u32 cpu)
{
    ARM& arm = cpu ? (ARM&)ARM7 : (ARM&)ARM9;

    if (IME[cpu] & 0x1)
    {
        arm.IRQ = !!(IE[cpu] & IF[cpu]);
        if ((ConsoleType == 1) && cpu)
            arm.IRQ |= !!(IE2 & IF2);
    }
    else
    {
        arm.IRQ = 0;
    }
}

void NDS::SetIRQ(u32 cpu, u32 irq)
{
    IF[cpu] |= (1 << irq);
    UpdateIRQ(cpu);

    if ((cpu == 1) && (CPUStop & CPUStop_Sleep))
    {
        if (IE[1] & (1 << irq))
        {
            CPUStop &= ~CPUStop_Sleep;
            CPUStop |= CPUStop_Wakeup;
            GPU.Restart3DFrame();
        }
    }
}

void NDS::ClearIRQ(u32 cpu, u32 irq)
{
    IF[cpu] &= ~(1 << irq);
    UpdateIRQ(cpu);
}

void NDS::SetIRQ2(u32 irq)
{
    IF2 |= (1 << irq);
    UpdateIRQ(1);
}

void NDS::ClearIRQ2(u32 irq)
{
    IF2 &= ~(1 << irq);
    UpdateIRQ(1);
}

bool NDS::HaltInterrupted(u32 cpu) const
{
    if (cpu == 0)
    {
        if (!(IME[0] & 0x1))
            return false;
    }

    if (IF[cpu] & IE[cpu])
        return true;

    if ((ConsoleType == 1) && cpu && (IF2 & IE2))
        return true;

    return false;
}

void NDS::StopCPU(u32 cpu, u32 mask)
{
    if (cpu)
    {
        CPUStop |= (mask << 16);
        ARM7.Halt(2);
    }
    else
    {
        CPUStop |= mask;
        ARM9.Halt(2);
    }
}

void NDS::ResumeCPU(u32 cpu, u32 mask)
{
    if (cpu) mask <<= 16;
    CPUStop &= ~mask;
}

void NDS::GXFIFOStall()
{
    if (CPUStop & CPUStop_GXStall) return;

    CPUStop |= CPUStop_GXStall;

    if (CurCPU == 1) ARM9.Halt(2);
    else
    {
        DMAs[0].StallIfRunning();
        DMAs[1].StallIfRunning();
        DMAs[2].StallIfRunning();
        DMAs[3].StallIfRunning();
        if (ConsoleType == 1)
        {
            auto& dsi = dynamic_cast<melonDS::DSi&>(*this);
            dsi.StallNDMAs();
        }
    }
}

void NDS::GXFIFOUnstall()
{
    CPUStop &= ~CPUStop_GXStall;
}

void NDS::EnterSleepMode()
{
    if (CPUStop & CPUStop_Sleep) return;

    CPUStop |= CPUStop_Sleep;
    ARM7.Halt(2);
}

u32 NDS::GetPC(u32 cpu) const
{
    return cpu ? ARM7.R[15] : ARM9.R[15];
}

u64 NDS::GetSysClockCycles(int num)
{
    u64 ret;

    if (num == 0 || num == 2)
    {
        if (CurCPU == 0)
            ret = ARM9Timestamp >> ARM9ClockShift;
        else
            ret = ARM7Timestamp;

        if (num == 2) ret -= FrameStartTimestamp;
    }
    else if (num == 1)
    {
        ret = LastSysClockCycles;
        LastSysClockCycles = 0;

        if (CurCPU == 0)
            LastSysClockCycles = ARM9Timestamp >> ARM9ClockShift;
        else
            LastSysClockCycles = ARM7Timestamp;
    }

    return ret;
}

void NDS::NocashPrint(u32 ncpu, u32 addr, bool appendNewline)
{
    // addr: debug string

    ARM* cpu = ncpu ? (ARM*)&ARM7 : (ARM*)&ARM9;
    u8 (NDS::*readfn)(u32) = ncpu ? &NDS::ARM7Read8 : &NDS::ARM9Read8;

    char output[1024];
    int ptr = 0;

    for (int i = 0; i < 120 && ptr < 1023; )
    {
        char ch = (this->*readfn)(addr++);
        i++;

        if (ch == '%')
        {
            char cmd[16]; int j;
            for (j = 0; j < 15; )
            {
                char ch2 = (this->*readfn)(addr++);
                i++;
                if (i >= 120) break;
                if (ch2 == '%') break;
                cmd[j++] = ch2;
            }
            cmd[j] = '\0';

            char subs[64];

            if (cmd[0] == 'r')
            {
                if      (!strcmp(cmd, "r0")) snprintf(subs, sizeof(subs), "%08X", cpu->R[0]);
                else if (!strcmp(cmd, "r1")) snprintf(subs, sizeof(subs), "%08X", cpu->R[1]);
                else if (!strcmp(cmd, "r2")) snprintf(subs, sizeof(subs), "%08X", cpu->R[2]);
                else if (!strcmp(cmd, "r3")) snprintf(subs, sizeof(subs), "%08X", cpu->R[3]);
                else if (!strcmp(cmd, "r4")) snprintf(subs, sizeof(subs), "%08X", cpu->R[4]);
                else if (!strcmp(cmd, "r5")) snprintf(subs, sizeof(subs), "%08X", cpu->R[5]);
                else if (!strcmp(cmd, "r6")) snprintf(subs, sizeof(subs), "%08X", cpu->R[6]);
                else if (!strcmp(cmd, "r7")) snprintf(subs, sizeof(subs), "%08X", cpu->R[7]);
                else if (!strcmp(cmd, "r8")) snprintf(subs, sizeof(subs), "%08X", cpu->R[8]);
                else if (!strcmp(cmd, "r9")) snprintf(subs, sizeof(subs), "%08X", cpu->R[9]);
                else if (!strcmp(cmd, "r10")) snprintf(subs, sizeof(subs), "%08X", cpu->R[10]);
                else if (!strcmp(cmd, "r11")) snprintf(subs, sizeof(subs), "%08X", cpu->R[11]);
                else if (!strcmp(cmd, "r12")) snprintf(subs, sizeof(subs), "%08X", cpu->R[12]);
                else if (!strcmp(cmd, "r13")) snprintf(subs, sizeof(subs), "%08X", cpu->R[13]);
                else if (!strcmp(cmd, "r14")) snprintf(subs, sizeof(subs), "%08X", cpu->R[14]);
                else if (!strcmp(cmd, "r15")) snprintf(subs, sizeof(subs), "%08X", cpu->R[15]);
            }
            else
            {
                if      (!strcmp(cmd, "sp")) snprintf(subs, sizeof(subs), "%08X", cpu->R[13]);
                else if (!strcmp(cmd, "lr")) snprintf(subs, sizeof(subs), "%08X", cpu->R[14]);
                else if (!strcmp(cmd, "pc")) snprintf(subs, sizeof(subs), "%08X", cpu->R[15]);
                else if (!strcmp(cmd, "frame")) snprintf(subs, sizeof(subs), "%u", NumFrames);
                else if (!strcmp(cmd, "scanline")) snprintf(subs, sizeof(subs), "%u", GPU.VCount);
                else if (!strcmp(cmd, "totalclks")) snprintf(subs, sizeof(subs), "%" PRIu64, GetSysClockCycles(0));
                else if (!strcmp(cmd, "lastclks")) snprintf(subs, sizeof(subs), "%" PRIu64, GetSysClockCycles(1));
                else if (!strcmp(cmd, "zeroclks"))
                {
                    snprintf(subs, sizeof(subs), "%s", "");
                    GetSysClockCycles(1);
                }
            }

            int slen = strnlen(subs, sizeof(subs));
            if ((ptr+slen) > 1023) slen = 1023-ptr;
            strncpy(&output[ptr], subs, slen);
            ptr += slen;
        }
        else
        {
            output[ptr++] = ch;
            if (ch == '\0') break;
        }
    }

    output[ptr] = '\0';
    Log(LogLevel::Debug, appendNewline ? "%s\n" : "%s", output);
}

void NDS::MonitorARM9Jump(u32 addr)
{
    // checkme: can the entrypoint addr be THUMB?
    // also TODO: make it work in DSi mode

    if ((!RunningGame) && NDSCartSlot.GetCart())
    {
        const NDSHeader& header = NDSCartSlot.GetCart()->GetHeader();
        if (addr == header.ARM9EntryAddress)
        {
            Log(LogLevel::Info, "Game is now booting\n");
            RunningGame = true;
        }
    }
}



void NDS::HandleTimerOverflow(u32 tid)
{
    Timer* timer = &Timers[tid];

    timer->Counter += (timer->Reload << 10);
    if (timer->Cnt & (1<<6))
        SetIRQ(tid >> 2, IRQ_Timer0 + (tid & 0x3));

    if ((tid & 0x3) == 3)
        return;

    for (;;)
    {
        tid++;

        timer = &Timers[tid];

        if ((timer->Cnt & 0x84) != 0x84)
            break;

        timer->Counter += (1 << 10);
        if (!(timer->Counter >> 26))
            break;

        timer->Counter = timer->Reload << 10;
        if (timer->Cnt & (1<<6))
            SetIRQ(tid >> 2, IRQ_Timer0 + (tid & 0x3));

        if ((tid & 0x3) == 3)
            break;
    }
}

void NDS::RunTimer(u32 tid, s32 cycles)
{
    Timer* timer = &Timers[tid];

    timer->Counter += (cycles << timer->CycleShift);
    while (timer->Counter >> 26)
    {
        timer->Counter -= (1 << 26);
        HandleTimerOverflow(tid);
    }
}

void NDS::RunTimers(u32 cpu)
{
    u32 timermask = TimerCheckMask[cpu];
    s32 cycles;

    if (cpu == 0)
        cycles = (ARM9Timestamp >> ARM9ClockShift) - TimerTimestamp[0];
    else
        cycles = ARM7Timestamp - TimerTimestamp[1];

    if (timermask & 0x1) RunTimer((cpu<<2)+0, cycles);
    if (timermask & 0x2) RunTimer((cpu<<2)+1, cycles);
    if (timermask & 0x4) RunTimer((cpu<<2)+2, cycles);
    if (timermask & 0x8) RunTimer((cpu<<2)+3, cycles);

    TimerTimestamp[cpu] += cycles;
}

const s32 TimerPrescaler[4] = {0, 6, 8, 10};

u16 NDS::TimerGetCounter(u32 timer)
{
    RunTimers(timer>>2);
    u32 ret = Timers[timer].Counter;

    return ret >> 10;
}

void NDS::TimerStart(u32 id, u16 cnt)
{
    Timer* timer = &Timers[id];
    u16 curstart = timer->Cnt & (1<<7);
    u16 newstart = cnt & (1<<7);

    RunTimers(id>>2);

    timer->Cnt = cnt;
    timer->CycleShift = 10 - TimerPrescaler[cnt & 0x03];

    if ((!curstart) && newstart)
    {
        timer->Counter = timer->Reload << 10;
    }

    if ((cnt & 0x84) == 0x80)
        TimerCheckMask[id>>2] |= 0x01 << (id&0x3);
    else
        TimerCheckMask[id>>2] &= ~(0x01 << (id&0x3));
}



bool NDS::DMAsInMode(u32 cpu, u32 mode) const
{
    cpu <<= 2;
    if (DMAs[cpu+0].IsInMode(mode)) return true;
    if (DMAs[cpu+1].IsInMode(mode)) return true;
    if (DMAs[cpu+2].IsInMode(mode)) return true;
    if (DMAs[cpu+3].IsInMode(mode)) return true;

    return false;
}

bool NDS::DMAsRunning(u32 cpu) const
{
    cpu <<= 2;
    if (DMAs[cpu+0].IsRunning()) return true;
    if (DMAs[cpu+1].IsRunning()) return true;
    if (DMAs[cpu+2].IsRunning()) return true;
    if (DMAs[cpu+3].IsRunning()) return true;

    return false;
}

void NDS::CheckDMAs(u32 cpu, u32 mode)
{
    cpu <<= 2;
    DMAs[cpu+0].StartIfNeeded(mode);
    DMAs[cpu+1].StartIfNeeded(mode);
    DMAs[cpu+2].StartIfNeeded(mode);
    DMAs[cpu+3].StartIfNeeded(mode);
}

void NDS::StopDMAs(u32 cpu, u32 mode)
{
    cpu <<= 2;
    DMAs[cpu+0].StopIfNeeded(mode);
    DMAs[cpu+1].StopIfNeeded(mode);
    DMAs[cpu+2].StopIfNeeded(mode);
    DMAs[cpu+3].StopIfNeeded(mode);
}



void NDS::DivDone(u32 param)
{
    DivCnt &= ~0xC000;

    switch (DivCnt & 0x0003)
    {
    case 0x0000:
        {
            s32 num = (s32)DivNumerator[0];
            s32 den = (s32)DivDenominator[0];
            if (den == 0)
            {
                DivQuotient[0] = (num<0) ? 1:-1;
                DivQuotient[1] = (num<0) ? -1:0;
                *(s64*)&DivRemainder[0] = num;
            }
            else if (num == -0x80000000 && den == -1)
            {
                *(s64*)&DivQuotient[0] = 0x80000000;
            }
            else
            {
                *(s64*)&DivQuotient[0] = (s64)(num / den);
                *(s64*)&DivRemainder[0] = (s64)(num % den);
            }
        }
        break;

    case 0x0001:
    case 0x0003:
        {
            s64 num = *(s64*)&DivNumerator[0];
            s32 den = (s32)DivDenominator[0];
            if (den == 0)
            {
                *(s64*)&DivQuotient[0] = (num<0) ? 1:-1;
                *(s64*)&DivRemainder[0] = num;
            }
            else if (num == -0x8000000000000000 && den == -1)
            {
                *(s64*)&DivQuotient[0] = 0x8000000000000000;
                *(s64*)&DivRemainder[0] = 0;
            }
            else
            {
                *(s64*)&DivQuotient[0] = (s64)(num / den);
                *(s64*)&DivRemainder[0] = (s64)(num % den);
            }
        }
        break;

    case 0x0002:
        {
            s64 num = *(s64*)&DivNumerator[0];
            s64 den = *(s64*)&DivDenominator[0];
            if (den == 0)
            {
                *(s64*)&DivQuotient[0] = (num<0) ? 1:-1;
                *(s64*)&DivRemainder[0] = num;
            }
            else if (num == -0x8000000000000000 && den == -1)
            {
                *(s64*)&DivQuotient[0] = 0x8000000000000000;
                *(s64*)&DivRemainder[0] = 0;
            }
            else
            {
                *(s64*)&DivQuotient[0] = (s64)(num / den);
                *(s64*)&DivRemainder[0] = (s64)(num % den);
            }
        }
        break;
    }

    if ((DivDenominator[0] | DivDenominator[1]) == 0)
        DivCnt |= 0x4000;
}

void NDS::StartDiv()
{
    CancelEvent(Event_Div);
    DivCnt |= 0x8000;
    ScheduleEvent(Event_Div, false, ((DivCnt&0x3)==0) ? 18:34, 0, 0);
}

// http://stackoverflow.com/questions/1100090/looking-for-an-efficient-integer-square-root-algorithm-for-arm-thumb2
void NDS::SqrtDone(u32 param)
{
    u64 val;
    u32 res = 0;
    u64 rem = 0;
    u32 prod = 0;
    u32 nbits, topshift;

    SqrtCnt &= ~0x8000;

    if (SqrtCnt & 0x0001)
    {
        val = *(u64*)&SqrtVal[0];
        nbits = 32;
        topshift = 62;
    }
    else
    {
        val = (u64)SqrtVal[0]; // 32bit
        nbits = 16;
        topshift = 30;
    }

    for (u32 i = 0; i < nbits; i++)
    {
        rem = (rem << 2) + ((val >> topshift) & 0x3);
        val <<= 2;
        res <<= 1;

        prod = (res << 1) + 1;
        if (rem >= prod)
        {
            rem -= prod;
            res++;
        }
    }

    SqrtRes = res;
}

void NDS::StartSqrt()
{
    CancelEvent(Event_Sqrt);
    SqrtCnt |= 0x8000;
    ScheduleEvent(Event_Sqrt, false, 13, 0, 0);
}



void NDS::debug(u32 param)
{
    Log(LogLevel::Debug, "ARM9 PC=%08X LR=%08X %08X\n", ARM9.R[15], ARM9.R[14], ARM9.R_IRQ[1]);
    Log(LogLevel::Debug, "ARM7 PC=%08X LR=%08X %08X\n", ARM7.R[15], ARM7.R[14], ARM7.R_IRQ[1]);

    Log(LogLevel::Debug, "ARM9 IME=%08X IE=%08X IF=%08X\n", IME[0], IE[0], IF[0]);
    Log(LogLevel::Debug, "ARM7 IME=%08X IE=%08X IF=%08X IE2=%04X IF2=%04X\n", IME[1], IE[1], IF[1], IE2, IF2);

    //for (int i = 0; i < 9; i++)
    //    printf("VRAM %c: %02X\n", 'A'+i, GPU->VRAMCNT[i]);
return;
    Platform::FileHandle* shit = Platform::OpenFile("debug/dragonball.bin", FileMode::Write);
    Platform::FileWrite(ARM9.ITCM, 0x8000, 1, shit);
    for (u32 i = 0x02000000; i < 0x02400000; i+=4)
    {
        u32 val = NDS::ARM7Read32(i);
        Platform::FileWrite(&val, 4, 1, shit);
    }
    for (u32 i = 0x037F0000; i < 0x03810000; i+=4)
    {
        u32 val = NDS::ARM7Read32(i);
        Platform::FileWrite(&val, 4, 1, shit);
    }
    for (u32 i = 0x06000000; i < 0x06040000; i+=4)
    {
        u32 val = NDS::ARM7Read32(i);
        Platform::FileWrite(&val, 4, 1, shit);
    }
    Platform::CloseFile(shit);

    /*FILE*
    shit = fopen("debug/bowser9.bin", "wb");
    fwrite(ARM9.ITCM, 0x8000, 1, shit);
    for (u32 i = 0x02000000; i < 0x04000000; i+=4)
    {
        u32 val = ARM9Read32(i);
        fwrite(&val, 4, 1, shit);
    }
    fclose(shit);
    shit = fopen("debug/bowser7.bin", "wb");
    for (u32 i = 0x02000000; i < 0x04000000; i+=4)
    {
        u32 val = ARM7Read32(i);
        fwrite(&val, 4, 1, shit);
    }
    fclose(shit);*/
}



u8 NDS::ARM9Read8(u32 addr)
{
    if ((addr & 0xFFFFF000) == 0xFFFF0000)
    {
        return *(u8*)&ARM9BIOS[addr & 0xFFF];
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        return *(u8*)&MainRAM[addr & MainRAMMask];

    case 0x03000000:
        if (SWRAM_ARM9.Mem)
        {
            return *(u8*)&SWRAM_ARM9.Mem[addr & SWRAM_ARM9.Mask];
        }
        else
        {
            return 0;
        }

    case 0x04000000:
        // Specifically want to call the NDS version, not a subclass
        return NDS::ARM9IORead8(addr);

    case 0x05000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return 0;
        return GPU.ReadPalette<u8>(addr);

    case 0x06000000:
        switch (addr & 0x00E00000)
        {
        case 0x00000000: GPU.SyncVRAM_ABG(addr, false); return GPU.ReadVRAM_ABG<u8>(addr);
        case 0x00200000: GPU.SyncVRAM_BBG(addr, false); return GPU.ReadVRAM_BBG<u8>(addr);
        case 0x00400000: GPU.SyncVRAM_AOBJ(addr, false); return GPU.ReadVRAM_AOBJ<u8>(addr);
        case 0x00600000: GPU.SyncVRAM_BOBJ(addr, false); return GPU.ReadVRAM_BOBJ<u8>(addr);
        default:         GPU.SyncVRAM_LCDC(addr, false); return GPU.ReadVRAM_LCDC<u8>(addr);
        }

    case 0x07000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return 0;
        return GPU.ReadOAM<u8>(addr);

    case 0x08000000:
    case 0x09000000:
        if (ExMemCnt[0] & (1<<7)) return 0x00; // deselected CPU is 00h-filled
        if (addr & 0x1) return GBACartSlot.ROMRead(addr-1) >> 8;
        return GBACartSlot.ROMRead(addr) & 0xFF;

    case 0x0A000000:
        if (ExMemCnt[0] & (1<<7)) return 0x00; // deselected CPU is 00h-filled
        return GBACartSlot.SRAMRead(addr);
    }

    Log(LogLevel::Debug, "unknown arm9 read8 %08X\n", addr);
    return 0;
}

u16 NDS::ARM9Read16(u32 addr)
{
    addr &= ~0x1;

    if ((addr & 0xFFFFF000) == 0xFFFF0000)
    {
        return *(u16*)&ARM9BIOS[addr & 0xFFF];
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        return *(u16*)&MainRAM[addr & MainRAMMask];

    case 0x03000000:
        if (SWRAM_ARM9.Mem)
        {
            return *(u16*)&SWRAM_ARM9.Mem[addr & SWRAM_ARM9.Mask];
        }
        else
        {
            return 0;
        }

    case 0x04000000:
        return NDS::ARM9IORead16(addr);

    case 0x05000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return 0;
        return GPU.ReadPalette<u16>(addr);

    case 0x06000000:
        switch (addr & 0x00E00000)
        {
        case 0x00000000: GPU.SyncVRAM_ABG(addr, false); return GPU.ReadVRAM_ABG<u16>(addr);
        case 0x00200000: GPU.SyncVRAM_BBG(addr, false); return GPU.ReadVRAM_BBG<u16>(addr);
        case 0x00400000: GPU.SyncVRAM_AOBJ(addr, false); return GPU.ReadVRAM_AOBJ<u16>(addr);
        case 0x00600000: GPU.SyncVRAM_BOBJ(addr, false); return GPU.ReadVRAM_BOBJ<u16>(addr);
        default:         GPU.SyncVRAM_LCDC(addr, false); return GPU.ReadVRAM_LCDC<u16>(addr);
        }

    case 0x07000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return 0;
        return GPU.ReadOAM<u16>(addr);

    case 0x08000000:
    case 0x09000000:
        if (ExMemCnt[0] & (1<<7)) return 0x0000; // deselected CPU is 00h-filled
        return GBACartSlot.ROMRead(addr);

    case 0x0A000000:
        if (ExMemCnt[0] & (1<<7)) return 0x0000; // deselected CPU is 00h-filled
        return GBACartSlot.SRAMRead(addr) |
              (GBACartSlot.SRAMRead(addr+1) << 8);
    }

    //if (addr) Log(LogLevel::Warn, "unknown arm9 read16 %08X %08X\n", addr, ARM9.R[15]);
    return 0;
}

u32 NDS::ARM9Read32(u32 addr)
{
    addr &= ~0x3;

    if ((addr & 0xFFFFF000) == 0xFFFF0000)
    {
        return *(u32*)&ARM9BIOS[addr & 0xFFF];
    }

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        return *(u32*)&MainRAM[addr & MainRAMMask];

    case 0x03000000:
        if (SWRAM_ARM9.Mem)
        {
            return *(u32*)&SWRAM_ARM9.Mem[addr & SWRAM_ARM9.Mask];
        }
        else
        {
            return 0;
        }

    case 0x04000000:
        return NDS::ARM9IORead32(addr);

    case 0x05000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return 0;
        return GPU.ReadPalette<u32>(addr);

    case 0x06000000:
        switch (addr & 0x00E00000)
        {
        case 0x00000000: GPU.SyncVRAM_ABG(addr, false); return GPU.ReadVRAM_ABG<u32>(addr);
        case 0x00200000: GPU.SyncVRAM_BBG(addr, false); return GPU.ReadVRAM_BBG<u32>(addr);
        case 0x00400000: GPU.SyncVRAM_AOBJ(addr, false); return GPU.ReadVRAM_AOBJ<u32>(addr);
        case 0x00600000: GPU.SyncVRAM_BOBJ(addr, false); return GPU.ReadVRAM_BOBJ<u32>(addr);
        default:         GPU.SyncVRAM_LCDC(addr, false); return GPU.ReadVRAM_LCDC<u32>(addr);
        }

    case 0x07000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return 0;
        return GPU.ReadOAM<u32>(addr & 0x7FF);

    case 0x08000000:
    case 0x09000000:
        if (ExMemCnt[0] & (1<<7)) return 0x00000000; // deselected CPU is 00h-filled
        return GBACartSlot.ROMRead(addr) |
              (GBACartSlot.ROMRead(addr+2) << 16);

    case 0x0A000000:
        if (ExMemCnt[0] & (1<<7)) return 0x00000000; // deselected CPU is 00h-filled
        return GBACartSlot.SRAMRead(addr) |
              (GBACartSlot.SRAMRead(addr+1) << 8) |
              (GBACartSlot.SRAMRead(addr+2) << 16) |
              (GBACartSlot.SRAMRead(addr+3) << 24);
    }

    //Log(LogLevel::Warn, "unknown arm9 read32 %08X | %08X %08X\n", addr, ARM9.R[15], ARM9.R[12]);
    return 0;
}

void NDS::ARM9Write8(u32 addr, u8 val)
{
    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_MainRAM>(addr);
        if (NSMLWatchWriteMaybeEnabled())
            TraceNSMLWatchWrite(this, "ARM9", ARM9.R[15], addr, 1, val);
        *(u8*)&MainRAM[addr & MainRAMMask] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM9.Mem)
        {
            JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_SharedWRAM>(addr);
            *(u8*)&SWRAM_ARM9.Mem[addr & SWRAM_ARM9.Mask] = val;
        }
        return;

    case 0x04000000:
        NDS::ARM9IOWrite8(addr, val);
        return;

    case 0x05000000:
    case 0x06000000:
    case 0x07000000:
        return;

    case 0x08000000:
    case 0x09000000:
        return;

    case 0x0A000000:
        if (ExMemCnt[0] & (1<<7)) return; // deselected CPU, skip the write
        GBACartSlot.SRAMWrite(addr, val);
        return;
    }

    Log(LogLevel::Debug, "unknown arm9 write8 %08X %02X\n", addr, val);
}

void NDS::ARM9Write16(u32 addr, u16 val)
{
    addr &= ~0x1;

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_MainRAM>(addr);
        if (NSMLWatchWriteMaybeEnabled())
            TraceNSMLWatchWrite(this, "ARM9", ARM9.R[15], addr, 2, val);
        *(u16*)&MainRAM[addr & MainRAMMask] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM9.Mem)
        {
            JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_SharedWRAM>(addr);
            *(u16*)&SWRAM_ARM9.Mem[addr & SWRAM_ARM9.Mask] = val;
        }
        return;

    case 0x04000000:
        NDS::ARM9IOWrite16(addr, val);
        return;

    case 0x05000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return;
        GPU.WritePalette<u16>(addr, val);
        return;

    case 0x06000000:
        JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_VRAM>(addr);
        switch (addr & 0x00E00000)
        {
        case 0x00000000: GPU.SyncVRAM_ABG(addr, true); GPU.WriteVRAM_ABG<u16>(addr, val); return;
        case 0x00200000: GPU.SyncVRAM_BBG(addr, true); GPU.WriteVRAM_BBG<u16>(addr, val); return;
        case 0x00400000: GPU.SyncVRAM_AOBJ(addr, true); GPU.WriteVRAM_AOBJ<u16>(addr, val); return;
        case 0x00600000: GPU.SyncVRAM_BOBJ(addr, true); GPU.WriteVRAM_BOBJ<u16>(addr, val); return;
        default: GPU.SyncVRAM_LCDC(addr, true); GPU.WriteVRAM_LCDC<u16>(addr, val); return;
        }

    case 0x07000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return;
        GPU.WriteOAM<u16>(addr, val);
        return;

    case 0x08000000:
    case 0x09000000:
        if (ExMemCnt[0] & (1<<7)) return; // deselected CPU, skip the write
        GBACartSlot.ROMWrite(addr, val);
        return;

    case 0x0A000000:
        if (ExMemCnt[0] & (1<<7)) return; // deselected CPU, skip the write
        GBACartSlot.SRAMWrite(addr, val & 0xFF);
        GBACartSlot.SRAMWrite(addr+1, val >> 8);
        return;
    }

    //if (addr) Log(LogLevel::Warn, "unknown arm9 write16 %08X %04X\n", addr, val);
}

void NDS::ARM9Write32(u32 addr, u32 val)
{
    addr &= ~0x3;

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_MainRAM>(addr);
        if (NSMLWatchWriteMaybeEnabled())
            TraceNSMLWatchWrite(this, "ARM9", ARM9.R[15], addr, 4, val);
        *(u32*)&MainRAM[addr & MainRAMMask] = val;
        return ;

    case 0x03000000:
        if (SWRAM_ARM9.Mem)
        {
            JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_SharedWRAM>(addr);
            *(u32*)&SWRAM_ARM9.Mem[addr & SWRAM_ARM9.Mask] = val;
        }
        return;

    case 0x04000000:
        NDS::ARM9IOWrite32(addr, val);
        return;

    case 0x05000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return;
        GPU.WritePalette(addr, val);
        return;

    case 0x06000000:
        JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_VRAM>(addr);
        switch (addr & 0x00E00000)
        {
        case 0x00000000: GPU.SyncVRAM_ABG(addr, true); GPU.WriteVRAM_ABG<u32>(addr, val); return;
        case 0x00200000: GPU.SyncVRAM_BBG(addr, true); GPU.WriteVRAM_BBG<u32>(addr, val); return;
        case 0x00400000: GPU.SyncVRAM_AOBJ(addr, true); GPU.WriteVRAM_AOBJ<u32>(addr, val); return;
        case 0x00600000: GPU.SyncVRAM_BOBJ(addr, true); GPU.WriteVRAM_BOBJ<u32>(addr, val); return;
        default: GPU.SyncVRAM_LCDC(addr, true); GPU.WriteVRAM_LCDC<u32>(addr, val); return;
        }

    case 0x07000000:
        if (!(PowerControl9 & ((addr & 0x400) ? (1<<9) : (1<<1)))) return;
        GPU.WriteOAM<u32>(addr, val);
        return;

    case 0x08000000:
    case 0x09000000:
        if (ExMemCnt[0] & (1<<7)) return; // deselected CPU, skip the write
        GBACartSlot.ROMWrite(addr, val & 0xFFFF);
        GBACartSlot.ROMWrite(addr+2, val >> 16);
        return;

    case 0x0A000000:
        if (ExMemCnt[0] & (1<<7)) return; // deselected CPU, skip the write
        GBACartSlot.SRAMWrite(addr, val & 0xFF);
        GBACartSlot.SRAMWrite(addr+1, (val >> 8) & 0xFF);
        GBACartSlot.SRAMWrite(addr+2, (val >> 16) & 0xFF);
        GBACartSlot.SRAMWrite(addr+3, val >> 24);
        return;
    }

    //Log(LogLevel::Warn, "unknown arm9 write32 %08X %08X | %08X\n", addr, val, ARM9.R[15]);
}

bool NDS::ARM9GetMemRegion(u32 addr, bool write, MemRegion* region)
{
    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        region->Mem = MainRAM;
        region->Mask = MainRAMMask;
        return true;

    case 0x03000000:
        if (SWRAM_ARM9.Mem)
        {
            region->Mem = SWRAM_ARM9.Mem;
            region->Mask = SWRAM_ARM9.Mask;
            return true;
        }
        break;
    }

    if ((addr & 0xFFFFF000) == 0xFFFF0000 && !write)
    {
        region->Mem = &ARM9BIOS[0];
        region->Mask = 0xFFF;
        return true;
    }

    region->Mem = NULL;
    return false;
}



u8 NDS::ARM7Read8(u32 addr)
{
    if (addr < 0x00004000)
    {
        // TODO: check the boundary? is it 4000 or higher on regular DS?
        if (ARM7.R[15] >= 0x00004000)
            return 0xFF;
        if (addr < ARM7BIOSProt && ARM7.R[15] >= ARM7BIOSProt)
            return 0xFF;

        return *(u8*)&ARM7BIOS[addr];
    }

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        return *(u8*)&MainRAM[addr & MainRAMMask];

    case 0x03000000:
        if (SWRAM_ARM7.Mem)
        {
            return *(u8*)&SWRAM_ARM7.Mem[addr & SWRAM_ARM7.Mask];
        }
        else
        {
            return *(u8*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];
        }

    case 0x03800000:
        return *(u8*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];

    case 0x04000000:
        return NDS::ARM7IORead8(addr);

    case 0x04800000:
        if (addr < 0x04810000)
        {
            if (!(PowerControl7 & (1<<1))) return 0;
            if (addr & 0x1) return Wifi.Read(addr-1) >> 8;
            return Wifi.Read(addr) & 0xFF;
        }
        break;

    case 0x06000000:
    case 0x06800000:
        return GPU.ReadVRAM_ARM7<u8>(addr);

    case 0x08000000:
    case 0x08800000:
    case 0x09000000:
    case 0x09800000:
        if (!(ExMemCnt[0] & (1<<7))) return 0x00; // deselected CPU is 00h-filled
        if (addr & 0x1) return GBACartSlot.ROMRead(addr-1) >> 8;
        return GBACartSlot.ROMRead(addr) & 0xFF;

    case 0x0A000000:
    case 0x0A800000:
        if (!(ExMemCnt[0] & (1<<7))) return 0x00; // deselected CPU is 00h-filled
        return GBACartSlot.SRAMRead(addr);
    }

    Log(LogLevel::Debug, "unknown arm7 read8 %08X %08X %08X/%08X\n", addr, ARM7.R[15], ARM7.R[0], ARM7.R[1]);
    return 0;
}

u16 NDS::ARM7Read16(u32 addr)
{
    addr &= ~0x1;

    if (addr < 0x00004000)
    {
        if (ARM7.R[15] >= 0x00004000)
            return 0xFFFF;
        if (addr < ARM7BIOSProt && ARM7.R[15] >= ARM7BIOSProt)
            return 0xFFFF;

        return *(u16*)&ARM7BIOS[addr];
    }

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        return *(u16*)&MainRAM[addr & MainRAMMask];

    case 0x03000000:
        if (SWRAM_ARM7.Mem)
        {
            return *(u16*)&SWRAM_ARM7.Mem[addr & SWRAM_ARM7.Mask];
        }
        else
        {
            return *(u16*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];
        }

    case 0x03800000:
        return *(u16*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];

    case 0x04000000:
        return NDS::ARM7IORead16(addr);

    case 0x04800000:
        if (addr < 0x04810000)
        {
            if (!(PowerControl7 & (1<<1))) return 0;
            return Wifi.Read(addr);
        }
        break;

    case 0x06000000:
    case 0x06800000:
        return GPU.ReadVRAM_ARM7<u16>(addr);

    case 0x08000000:
    case 0x08800000:
    case 0x09000000:
    case 0x09800000:
        if (!(ExMemCnt[0] & (1<<7))) return 0x0000; // deselected CPU is 00h-filled
        return GBACartSlot.ROMRead(addr);

    case 0x0A000000:
    case 0x0A800000:
        if (!(ExMemCnt[0] & (1<<7))) return 0x0000; // deselected CPU is 00h-filled
        return GBACartSlot.SRAMRead(addr) |
              (GBACartSlot.SRAMRead(addr+1) << 8);
    }

    Log(LogLevel::Debug, "unknown arm7 read16 %08X %08X\n", addr, ARM7.R[15]);
    return 0;
}

u32 NDS::ARM7Read32(u32 addr)
{
    addr &= ~0x3;

    if (addr < 0x00004000)
    {
        if (ARM7.R[15] >= 0x00004000)
            return 0xFFFFFFFF;
        if (addr < ARM7BIOSProt && ARM7.R[15] >= ARM7BIOSProt)
            return 0xFFFFFFFF;

        return *(u32*)&ARM7BIOS[addr];
    }

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        return *(u32*)&MainRAM[addr & MainRAMMask];

    case 0x03000000:
        if (SWRAM_ARM7.Mem)
        {
            return *(u32*)&SWRAM_ARM7.Mem[addr & SWRAM_ARM7.Mask];
        }
        else
        {
            return *(u32*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];
        }

    case 0x03800000:
        return *(u32*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];

    case 0x04000000:
        return NDS::ARM7IORead32(addr);

    case 0x04800000:
        if (addr < 0x04810000)
        {
            if (!(PowerControl7 & (1<<1))) return 0;
            return Wifi.Read(addr) | (Wifi.Read(addr+2) << 16);
        }
        break;

    case 0x06000000:
    case 0x06800000:
        return GPU.ReadVRAM_ARM7<u32>(addr);

    case 0x08000000:
    case 0x08800000:
    case 0x09000000:
    case 0x09800000:
        if (!(ExMemCnt[0] & (1<<7))) return 0x00000000; // deselected CPU is 00h-filled
        return GBACartSlot.ROMRead(addr) |
              (GBACartSlot.ROMRead(addr+2) << 16);

    case 0x0A000000:
    case 0x0A800000:
        if (!(ExMemCnt[0] & (1<<7))) return 0x00000000; // deselected CPU is 00h-filled
        return GBACartSlot.SRAMRead(addr) |
              (GBACartSlot.SRAMRead(addr+1) << 8) |
              (GBACartSlot.SRAMRead(addr+2) << 16) |
              (GBACartSlot.SRAMRead(addr+3) << 24);
    }

    //Log(LogLevel::Warn, "unknown arm7 read32 %08X | %08X\n", addr, ARM7.R[15]);
    return 0;
}

void NDS::ARM7Write8(u32 addr, u8 val)
{
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_MainRAM>(addr);
        if (NSMLWatchWriteMaybeEnabled())
            TraceNSMLWatchWrite(this, "ARM7", ARM7.R[15], addr, 1, val);
        *(u8*)&MainRAM[addr & MainRAMMask] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM7.Mem)
        {
            JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_SharedWRAM>(addr);
            *(u8*)&SWRAM_ARM7.Mem[addr & SWRAM_ARM7.Mask] = val;
            return;
        }
        else
        {
            JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_WRAM7>(addr);
            *(u8*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)] = val;
            return;
        }

    case 0x03800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_WRAM7>(addr);
        *(u8*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)] = val;
        return;

    case 0x04000000:
        NDS::ARM7IOWrite8(addr, val);
        return;

    case 0x06000000:
    case 0x06800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_VWRAM>(addr);
        GPU.WriteVRAM_ARM7<u8>(addr, val);
        return;

    case 0x08000000:
    case 0x08800000:
    case 0x09000000:
    case 0x09800000:
        return;

    case 0x0A000000:
    case 0x0A800000:
        if (!(ExMemCnt[0] & (1<<7))) return; // deselected CPU, skip the write
        GBACartSlot.SRAMWrite(addr, val);
        return;
    }

    //if (ARM7.R[15] > 0x00002F30) // ARM7 BIOS bug
    if (addr >= 0x01000000)
        Log(LogLevel::Debug, "unknown arm7 write8 %08X %02X @ %08X\n", addr, val, ARM7.R[15]);
}

void NDS::ARM7Write16(u32 addr, u16 val)
{
    addr &= ~0x1;

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_MainRAM>(addr);
        if (NSMLWatchWriteMaybeEnabled())
            TraceNSMLWatchWrite(this, "ARM7", ARM7.R[15], addr, 2, val);
        *(u16*)&MainRAM[addr & MainRAMMask] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM7.Mem)
        {
            JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_SharedWRAM>(addr);
            *(u16*)&SWRAM_ARM7.Mem[addr & SWRAM_ARM7.Mask] = val;
            return;
        }
        else
        {
            JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_WRAM7>(addr);
            *(u16*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)] = val;
            return;
        }

    case 0x03800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_WRAM7>(addr);
        *(u16*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)] = val;
        return;

    case 0x04000000:
        NDS::ARM7IOWrite16(addr, val);
        return;

    case 0x04800000:
        if (addr < 0x04810000)
        {
            if (!(PowerControl7 & (1<<1))) return;
            Wifi.Write(addr, val);
            return;
        }
        break;

    case 0x06000000:
    case 0x06800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_VWRAM>(addr);
        GPU.WriteVRAM_ARM7<u16>(addr, val);
        return;

    case 0x08000000:
    case 0x08800000:
    case 0x09000000:
    case 0x09800000:
        if (!(ExMemCnt[0] & (1<<7))) return; // deselected CPU, skip the write
        GBACartSlot.ROMWrite(addr, val);
        return;

    case 0x0A000000:
    case 0x0A800000:
        if (!(ExMemCnt[0] & (1<<7))) return; // deselected CPU, skip the write
        GBACartSlot.SRAMWrite(addr, val & 0xFF);
        GBACartSlot.SRAMWrite(addr+1, val >> 8);
        return;
    }

    if (addr >= 0x01000000)
        Log(LogLevel::Debug, "unknown arm7 write16 %08X %04X @ %08X\n", addr, val, ARM7.R[15]);
}

void NDS::ARM7Write32(u32 addr, u32 val)
{
    addr &= ~0x3;

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_MainRAM>(addr);
        if (NSMLWatchWriteMaybeEnabled())
            TraceNSMLWatchWrite(this, "ARM7", ARM7.R[15], addr, 4, val);
        *(u32*)&MainRAM[addr & MainRAMMask] = val;
        return;

    case 0x03000000:
        if (SWRAM_ARM7.Mem)
        {
            JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_SharedWRAM>(addr);
            *(u32*)&SWRAM_ARM7.Mem[addr & SWRAM_ARM7.Mask] = val;
            return;
        }
        else
        {
            JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_WRAM7>(addr);
            *(u32*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)] = val;
            return;
        }

    case 0x03800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_WRAM7>(addr);
        *(u32*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)] = val;
        return;

    case 0x04000000:
        NDS::ARM7IOWrite32(addr, val);
        return;

    case 0x04800000:
        if (addr < 0x04810000)
        {
            if (!(PowerControl7 & (1<<1))) return;
            Wifi.Write(addr, val & 0xFFFF);
            Wifi.Write(addr+2, val >> 16);
            return;
        }
        break;

    case 0x06000000:
    case 0x06800000:
        JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_VWRAM>(addr);
        GPU.WriteVRAM_ARM7<u32>(addr, val);
        return;

    case 0x08000000:
    case 0x08800000:
    case 0x09000000:
    case 0x09800000:
        if (!(ExMemCnt[0] & (1<<7))) return; // deselected CPU, skip the write
        GBACartSlot.ROMWrite(addr, val & 0xFFFF);
        GBACartSlot.ROMWrite(addr+2, val >> 16);
        return;

    case 0x0A000000:
    case 0x0A800000:
        if (!(ExMemCnt[0] & (1<<7))) return; // deselected CPU, skip the write
        GBACartSlot.SRAMWrite(addr, val & 0xFF);
        GBACartSlot.SRAMWrite(addr+1, (val >> 8) & 0xFF);
        GBACartSlot.SRAMWrite(addr+2, (val >> 16) & 0xFF);
        GBACartSlot.SRAMWrite(addr+3, val >> 24);
        return;
    }

    if (addr >= 0x01000000)
        Log(LogLevel::Debug, "unknown arm7 write32 %08X %08X @ %08X\n", addr, val, ARM7.R[15]);
}

bool NDS::ARM7GetMemRegion(u32 addr, bool write, MemRegion* region)
{
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        region->Mem = MainRAM;
        region->Mask = MainRAMMask;
        return true;

    case 0x03000000:
        // note on this, and why we can only cover it in one particular case:
        // it is typical for games to map all shared WRAM to the ARM7
        // then access all the WRAM as one contiguous block starting at 0x037F8000
        // this case needs a bit of a hack to cover
        // it's not really worth bothering anyway
        if (!SWRAM_ARM7.Mem)
        {
            region->Mem = ARM7WRAM;
            region->Mask = ARM7WRAMSize-1;
            return true;
        }
        break;

    case 0x03800000:
        region->Mem = ARM7WRAM;
        region->Mask = ARM7WRAMSize-1;
        return true;
    }

    // BIOS. ARM7 PC has to be within range.
    if (addr < 0x00004000 && !write)
    {
        if (ARM7.R[15] < 0x4000 && (addr >= ARM7BIOSProt || ARM7.R[15] < ARM7BIOSProt))
        {
            region->Mem = &ARM7BIOS[0];
            region->Mask = 0x3FFF;
            return true;
        }
    }

    region->Mem = NULL;
    return false;
}




#define CASE_READ8_16BIT(addr, val) \
    case (addr): return (val) & 0xFF; \
    case (addr+1): return (val) >> 8;

#define CASE_READ8_32BIT(addr, val) \
    case (addr): return (val) & 0xFF; \
    case (addr+1): return ((val) >> 8) & 0xFF; \
    case (addr+2): return ((val) >> 16) & 0xFF; \
    case (addr+3): return (val) >> 24;

u8 NDS::ARM9IORead8(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU.DispStat[0] & 0xFF;
    case 0x04000005: return GPU.DispStat[0] >> 8;
    case 0x04000006: return GPU.VCount & 0xFF;
    case 0x04000007: return GPU.VCount >> 8;

    case 0x04000064:
    case 0x04000065:
    case 0x04000066:
    case 0x04000067:
    case 0x0400006C:
    case 0x0400006D:
    case 0x0400106C:
    case 0x0400106D: return GPU.Read8(addr);

    case 0x04000130: LagFrameFlag = false; return KeyInput & 0xFF;
    case 0x04000131: LagFrameFlag = false; return (KeyInput >> 8) & 0xFF;
    case 0x04000132: return KeyCnt[0] & 0xFF;
    case 0x04000133: return KeyCnt[0] >> 8;

    case 0x04000180: return IPCSync9 & 0xFF;
    case 0x04000181: return IPCSync9 >> 8;

    case 0x040001A0: return NDSCartSlots[0]->ReadSPICnt(0) & 0xFF;
    case 0x040001A1: return NDSCartSlots[0]->ReadSPICnt(0) >> 8;
    case 0x040001A2: return NDSCartSlots[0]->ReadSPIData(0);
    case 0x040001A4: return NDSCartSlots[0]->ReadROMCnt(0) & 0xFF;
    case 0x040001A5: return (NDSCartSlots[0]->ReadROMCnt(0) >> 8) & 0xFF;
    case 0x040001A6: return (NDSCartSlots[0]->ReadROMCnt(0) >> 16) & 0xFF;
    case 0x040001A7: return NDSCartSlots[0]->ReadROMCnt(0) >> 24;

    case 0x04000208: return IME[0];

    case 0x04000240: return GPU.VRAMCNT[0];
    case 0x04000241: return GPU.VRAMCNT[1];
    case 0x04000242: return GPU.VRAMCNT[2];
    case 0x04000243: return GPU.VRAMCNT[3];
    case 0x04000244: return GPU.VRAMCNT[4];
    case 0x04000245: return GPU.VRAMCNT[5];
    case 0x04000246: return GPU.VRAMCNT[6];
    case 0x04000247: return WRAMCnt;
    case 0x04000248: return GPU.VRAMCNT[7];
    case 0x04000249: return GPU.VRAMCNT[8];

    CASE_READ8_16BIT(0x04000280, DivCnt)
    CASE_READ8_32BIT(0x04000290, DivNumerator[0])
    CASE_READ8_32BIT(0x04000294, DivNumerator[1])
    CASE_READ8_32BIT(0x04000298, DivDenominator[0])
    CASE_READ8_32BIT(0x0400029C, DivDenominator[1])
    CASE_READ8_32BIT(0x040002A0, DivQuotient[0])
    CASE_READ8_32BIT(0x040002A4, DivQuotient[1])
    CASE_READ8_32BIT(0x040002A8, DivRemainder[0])
    CASE_READ8_32BIT(0x040002AC, DivRemainder[1])

    CASE_READ8_16BIT(0x040002B0, SqrtCnt)
    CASE_READ8_32BIT(0x040002B4, SqrtRes)
    CASE_READ8_32BIT(0x040002B8, SqrtVal[0])
    CASE_READ8_32BIT(0x040002BC, SqrtVal[1])

    case 0x04000300: return PostFlag9;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        return GPU.GPU2D_A.Read8(addr);
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        return GPU.GPU2D_B.Read8(addr);
    }
    if (addr >= 0x04000320 && addr < 0x040006A4)
    {
        return GPU.GPU3D.Read8(addr);
    }
    // NO$GBA debug register "Emulation ID"
    if(addr >= 0x04FFFA00 && addr < 0x04FFFA10)
    {
        // FIX: GBATek says this should be padded with spaces
        static char const emuID[16] = "melonDS " MELONDS_VERSION_BASE;
        auto idx = addr - 0x04FFFA00;
        return (u8)(emuID[idx]);
    }

    if ((addr & 0xFFFFF000) != 0x04004000)
        Log(LogLevel::Debug, "unknown ARM9 IO read8 %08X %08X\n", addr, ARM9.R[15]);
    return 0;
}

u16 NDS::ARM9IORead16(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU.DispStat[0];
    case 0x04000006: return GPU.VCount;

    case 0x04000060: return GPU.GPU3D.Read16(addr);
    case 0x04000064:
    case 0x04000066:
    case 0x0400006C:
    case 0x0400106C: return GPU.Read16(addr);

    case 0x040000B8: return DMAs[0].Cnt & 0xFFFF;
    case 0x040000BA: return DMAs[0].Cnt >> 16;
    case 0x040000C4: return DMAs[1].Cnt & 0xFFFF;
    case 0x040000C6: return DMAs[1].Cnt >> 16;
    case 0x040000D0: return DMAs[2].Cnt & 0xFFFF;
    case 0x040000D2: return DMAs[2].Cnt >> 16;
    case 0x040000DC: return DMAs[3].Cnt & 0xFFFF;
    case 0x040000DE: return DMAs[3].Cnt >> 16;

    case 0x040000E0: return ((u16*)DMA9Fill)[0];
    case 0x040000E2: return ((u16*)DMA9Fill)[1];
    case 0x040000E4: return ((u16*)DMA9Fill)[2];
    case 0x040000E6: return ((u16*)DMA9Fill)[3];
    case 0x040000E8: return ((u16*)DMA9Fill)[4];
    case 0x040000EA: return ((u16*)DMA9Fill)[5];
    case 0x040000EC: return ((u16*)DMA9Fill)[6];
    case 0x040000EE: return ((u16*)DMA9Fill)[7];

    case 0x04000100: return TimerGetCounter(0);
    case 0x04000102: return Timers[0].Cnt;
    case 0x04000104: return TimerGetCounter(1);
    case 0x04000106: return Timers[1].Cnt;
    case 0x04000108: return TimerGetCounter(2);
    case 0x0400010A: return Timers[2].Cnt;
    case 0x0400010C: return TimerGetCounter(3);
    case 0x0400010E: return Timers[3].Cnt;

    case 0x04000130: LagFrameFlag = false; return KeyInput & 0xFFFF;
    case 0x04000132: return KeyCnt[0];

    case 0x04000180: return IPCSync9;
    case 0x04000184:
        {
            u16 val = IPCFIFOCnt9;
            if (IPCFIFO9.IsEmpty())     val |= 0x0001;
            else if (IPCFIFO9.IsFull()) val |= 0x0002;
            if (IPCFIFO7.IsEmpty())     val |= 0x0100;
            else if (IPCFIFO7.IsFull()) val |= 0x0200;
            return val;
        }

    case 0x040001A0: return NDSCartSlots[0]->ReadSPICnt(0);
    case 0x040001A2: return NDSCartSlots[0]->ReadSPIData(0);
    case 0x040001A4: return NDSCartSlots[0]->ReadROMCnt(0) & 0xFFFF;
    case 0x040001A6: return NDSCartSlots[0]->ReadROMCnt(0) >> 16;

    case 0x04000204: return ExMemCnt[0];
    case 0x04000208: return IME[0];
    case 0x04000210: return IE[0] & 0xFFFF;
    case 0x04000212: return IE[0] >> 16;
    case 0x04000214: return IF[0] & 0xFFFF;
    case 0x04000216: return IF[0] >> 16;

    case 0x04000240: return GPU.VRAMCNT[0] | (GPU.VRAMCNT[1] << 8);
    case 0x04000242: return GPU.VRAMCNT[2] | (GPU.VRAMCNT[3] << 8);
    case 0x04000244: return GPU.VRAMCNT[4] | (GPU.VRAMCNT[5] << 8);
    case 0x04000246: return GPU.VRAMCNT[6] | (WRAMCnt << 8);
    case 0x04000248: return GPU.VRAMCNT[7] | (GPU.VRAMCNT[8] << 8);

    case 0x04000280: return DivCnt;
    case 0x04000290: return DivNumerator[0] & 0xFFFF;
    case 0x04000292: return DivNumerator[0] >> 16;
    case 0x04000294: return DivNumerator[1] & 0xFFFF;
    case 0x04000296: return DivNumerator[1] >> 16;
    case 0x04000298: return DivDenominator[0] & 0xFFFF;
    case 0x0400029A: return DivDenominator[0] >> 16;
    case 0x0400029C: return DivDenominator[1] & 0xFFFF;
    case 0x0400029E: return DivDenominator[1] >> 16;
    case 0x040002A0: return DivQuotient[0] & 0xFFFF;
    case 0x040002A2: return DivQuotient[0] >> 16;
    case 0x040002A4: return DivQuotient[1] & 0xFFFF;
    case 0x040002A6: return DivQuotient[1] >> 16;
    case 0x040002A8: return DivRemainder[0] & 0xFFFF;
    case 0x040002AA: return DivRemainder[0] >> 16;
    case 0x040002AC: return DivRemainder[1] & 0xFFFF;
    case 0x040002AE: return DivRemainder[1] >> 16;

    case 0x040002B0: return SqrtCnt;
    case 0x040002B4: return SqrtRes & 0xFFFF;
    case 0x040002B6: return SqrtRes >> 16;
    case 0x040002B8: return SqrtVal[0] & 0xFFFF;
    case 0x040002BA: return SqrtVal[0] >> 16;
    case 0x040002BC: return SqrtVal[1] & 0xFFFF;
    case 0x040002BE: return SqrtVal[1] >> 16;

    case 0x04000300: return PostFlag9;
    case 0x04000304: return PowerControl9;

    case 0x04004000:
    case 0x04004004:
    case 0x04004010:
        // shut up logging for DSi registers
        return 0;
    }

    if ((addr >= 0x04000000 && addr < 0x04000060) || (addr == 0x0400006C))
    {
        return GPU.GPU2D_A.Read16(addr);
    }
    if ((addr >= 0x04001000 && addr < 0x04001060) || (addr == 0x0400106C))
    {
        return GPU.GPU2D_B.Read16(addr);
    }
    if (addr >= 0x04000320 && addr < 0x040006A4)
    {
        return GPU.GPU3D.Read16(addr);
    }

    if ((addr & 0xFFFFF000) != 0x04004000)
        Log(LogLevel::Debug, "unknown ARM9 IO read16 %08X %08X\n", addr, ARM9.R[15]);
    return 0;
}

u32 NDS::ARM9IORead32(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU.DispStat[0] | (GPU.VCount << 16);

    case 0x04000060: return GPU.GPU3D.Read32(addr);
    case 0x04000064:
    case 0x0400006C:
    case 0x0400106C: return GPU.Read32(addr);

    case 0x040000B0: return DMAs[0].SrcAddr;
    case 0x040000B4: return DMAs[0].DstAddr;
    case 0x040000B8: return DMAs[0].Cnt;
    case 0x040000BC: return DMAs[1].SrcAddr;
    case 0x040000C0: return DMAs[1].DstAddr;
    case 0x040000C4: return DMAs[1].Cnt;
    case 0x040000C8: return DMAs[2].SrcAddr;
    case 0x040000CC: return DMAs[2].DstAddr;
    case 0x040000D0: return DMAs[2].Cnt;
    case 0x040000D4: return DMAs[3].SrcAddr;
    case 0x040000D8: return DMAs[3].DstAddr;
    case 0x040000DC: return DMAs[3].Cnt;

    case 0x040000E0: return DMA9Fill[0];
    case 0x040000E4: return DMA9Fill[1];
    case 0x040000E8: return DMA9Fill[2];
    case 0x040000EC: return DMA9Fill[3];

    case 0x040000F4: return 0; // ???? Golden Sun Dark Dawn keeps reading this

    case 0x04000100: return TimerGetCounter(0) | (Timers[0].Cnt << 16);
    case 0x04000104: return TimerGetCounter(1) | (Timers[1].Cnt << 16);
    case 0x04000108: return TimerGetCounter(2) | (Timers[2].Cnt << 16);
    case 0x0400010C: return TimerGetCounter(3) | (Timers[3].Cnt << 16);

    case 0x04000130: LagFrameFlag = false; return (KeyInput & 0xFFFF) | (KeyCnt[0] << 16);

    case 0x04000180: return IPCSync9;
    case 0x04000184: return NDS::ARM9IORead16(addr);

    case 0x040001A0: return NDSCartSlots[0]->ReadSPICnt(0) | (NDSCartSlots[0]->ReadSPIData(0) << 16);
    case 0x040001A4: return NDSCartSlots[0]->ReadROMCnt(0);

    case 0x04000208: return IME[0];
    case 0x04000210: return IE[0];
    case 0x04000214: return IF[0];

    case 0x04000240: return GPU.VRAMCNT[0] | (GPU.VRAMCNT[1] << 8) | (GPU.VRAMCNT[2] << 16) | (GPU.VRAMCNT[3] << 24);
    case 0x04000244: return GPU.VRAMCNT[4] | (GPU.VRAMCNT[5] << 8) | (GPU.VRAMCNT[6] << 16) | (WRAMCnt << 24);
    case 0x04000248: return GPU.VRAMCNT[7] | (GPU.VRAMCNT[8] << 8);

    case 0x04000280: return DivCnt;
    case 0x04000290: return DivNumerator[0];
    case 0x04000294: return DivNumerator[1];
    case 0x04000298: return DivDenominator[0];
    case 0x0400029C: return DivDenominator[1];
    case 0x040002A0: return DivQuotient[0];
    case 0x040002A4: return DivQuotient[1];
    case 0x040002A8: return DivRemainder[0];
    case 0x040002AC: return DivRemainder[1];

    case 0x040002B0: return SqrtCnt;
    case 0x040002B4: return SqrtRes;
    case 0x040002B8: return SqrtVal[0];
    case 0x040002BC: return SqrtVal[1];

    case 0x04000300: return PostFlag9;
    case 0x04000304: return PowerControl9;

    case 0x04100000:
        if (IPCFIFOCnt9 & 0x8000)
        {
            u32 ret;
            if (IPCFIFO7.IsEmpty())
            {
                IPCFIFOCnt9 |= 0x4000;
                ret = IPCFIFO7.Peek();
            }
            else
            {
                ret = IPCFIFO7.Read();

                if (IPCFIFO7.IsEmpty() && (IPCFIFOCnt7 & 0x0004))
                    SetIRQ(1, IRQ_IPCSendDone);
            }
            return ret;
        }
        else
            return IPCFIFO7.Peek();

    case 0x04100010:
        return NDSCartSlots[0]->ReadROMData(0);

    case 0x04004000:
    case 0x04004004:
    case 0x04004010:
        // shut up logging for DSi registers
        return 0;

    // NO$GBA debug register "Clock Cycles"
    // Since it's a 64 bit reg. the CPU will access it in two parts:
    case 0x04FFFA20: return (u32)(GetSysClockCycles(0) & 0xFFFFFFFF);
    case 0x04FFFA24: return (u32)(GetSysClockCycles(0) >> 32);
    case 0x04FFFA2C:
        {
            static const bool replayRender =
                getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_REPLAY_RENDER") != nullptr;
            return replayRender ? 1 : 0;
        }
    }

    if ((addr >= 0x04000000 && addr < 0x04000060) || (addr == 0x0400006C))
    {
        return GPU.GPU2D_A.Read32(addr);
    }
    if ((addr >= 0x04001000 && addr < 0x04001060) || (addr == 0x0400106C))
    {
        return GPU.GPU2D_B.Read32(addr);
    }
    if (addr >= 0x04000320 && addr < 0x040006A4)
    {
        return GPU.GPU3D.Read32(addr);
    }

    if ((addr & 0xFFFFF000) != 0x04004000)
        Log(LogLevel::Debug, "unknown ARM9 IO read32 %08X %08X\n", addr, ARM9.R[15]);
    return 0;
}

void NDS::ARM9IOWrite8(u32 addr, u8 val)
{
    switch (addr)
    {
    case 0x04000004: GPU.SetDispStat(0, val, 0x00FF); return;
    case 0x04000005: GPU.SetDispStat(0, val << 8, 0xFF00); return;
    case 0x04000006: GPU.SetVCount(val, 0x00FF); return;
    case 0x04000007: GPU.SetVCount(val << 8, 0xFF00); return;

    case 0x04000060:
    case 0x04000061: GPU.GPU3D.Write8(addr, val); return;
    case 0x04000064:
    case 0x04000065:
    case 0x04000066:
    case 0x04000067:
    case 0x04000068:
    case 0x04000069:
    case 0x0400006A:
    case 0x0400006B:
    case 0x0400006C:
    case 0x0400006D:
    case 0x0400106C:
    case 0x0400106D: GPU.Write8(addr, val); return;

    case 0x04000132:
        KeyCnt[0] = (KeyCnt[0] & 0xFF00) | val;
        return;
    case 0x04000133:
        KeyCnt[0] = (KeyCnt[0] & 0x00FF) | (val << 8);
        return;

    case 0x04000181:
        IPCSync7 &= 0xFFF0;
        IPCSync7 |= (val & 0x0F);
        IPCSync9 &= 0xB0FF;
        IPCSync9 |= ((val & 0x4F) << 8);
        if ((val & 0x20) && (IPCSync7 & 0x4000))
        {
            SetIRQ(1, IRQ_IPCSync);
        }
        return;

    case 0x04000188:
        NDS::ARM9IOWrite32(addr, val | (val << 8) | (val << 16) | (val << 24));
        return;

    case 0x040001A0:
        NDSCartSlots[0]->WriteSPICnt(0, val, 0x00FF);
        return;
    case 0x040001A1:
        NDSCartSlots[0]->WriteSPICnt(0, val << 8, 0xFF00);
        return;
    case 0x040001A2:
        NDSCartSlots[0]->WriteSPIData(0, val);
        return;

    case 0x040001A4:
        NDSCartSlots[0]->WriteROMCnt(0, val, 0x000000FF);
        return;
    case 0x040001A5:
        NDSCartSlots[0]->WriteROMCnt(0, val << 8, 0x0000FF00);
        return;
    case 0x040001A6:
        NDSCartSlots[0]->WriteROMCnt(0, val << 16, 0x00FF0000);
        return;
    case 0x040001A7:
        NDSCartSlots[0]->WriteROMCnt(0, val << 24, 0xFF000000);
        return;

    case 0x040001A8: NDSCartSlots[0]->WriteROMCommand(0, 0, val); return;
    case 0x040001A9: NDSCartSlots[0]->WriteROMCommand(0, 1, val); return;
    case 0x040001AA: NDSCartSlots[0]->WriteROMCommand(0, 2, val); return;
    case 0x040001AB: NDSCartSlots[0]->WriteROMCommand(0, 3, val); return;
    case 0x040001AC: NDSCartSlots[0]->WriteROMCommand(0, 4, val); return;
    case 0x040001AD: NDSCartSlots[0]->WriteROMCommand(0, 5, val); return;
    case 0x040001AE: NDSCartSlots[0]->WriteROMCommand(0, 6, val); return;
    case 0x040001AF: NDSCartSlots[0]->WriteROMCommand(0, 7, val); return;

    case 0x04000208: IME[0] = val & 0x1; UpdateIRQ(0); return;

    case 0x04000240: GPU.MapVRAM_AB(0, val); return;
    case 0x04000241: GPU.MapVRAM_AB(1, val); return;
    case 0x04000242: GPU.MapVRAM_CD(2, val); return;
    case 0x04000243: GPU.MapVRAM_CD(3, val); return;
    case 0x04000244: GPU.MapVRAM_E(4, val); return;
    case 0x04000245: GPU.MapVRAM_FG(5, val); return;
    case 0x04000246: GPU.MapVRAM_FG(6, val); return;
    case 0x04000247: MapSharedWRAM(val); return;
    case 0x04000248: GPU.MapVRAM_H(7, val); return;
    case 0x04000249: GPU.MapVRAM_I(8, val); return;

    case 0x04000300:
        if (PostFlag9 & 0x01) val |= 0x01;
        PostFlag9 = val & 0x03;
        return;

    // NO$GBA debug register "Char Out"
        case 0x04FFFA1C: Log(LogLevel::Debug, "%c", char(val)); return;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        GPU.GPU2D_A.Write8(addr, val);
        return;
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        GPU.GPU2D_B.Write8(addr, val);
        return;
    }
    if (addr >= 0x04000320 && addr < 0x040006A4)
    {
        GPU.GPU3D.Write8(addr, val);
        return;
    }

    Log(LogLevel::Debug, "unknown ARM9 IO write8 %08X %02X %08X\n", addr, val, ARM9.R[15]);
}

void NDS::ARM9IOWrite16(u32 addr, u16 val)
{
    switch (addr)
    {
    case 0x04000004: GPU.SetDispStat(0, val, 0xFFFF); return;
    case 0x04000006: GPU.SetVCount(val, 0xFFFF); return;

    case 0x04000060: GPU.GPU3D.Write16(addr, val); return;
    case 0x04000064:
    case 0x04000066:
    case 0x04000068:
    case 0x0400006A:
    case 0x0400006C:
    case 0x0400106C: GPU.Write16(addr, val); return;

    case 0x040000B8: DMAs[0].WriteCnt((DMAs[0].Cnt & 0xFFFF0000) | val); return;
    case 0x040000BA: DMAs[0].WriteCnt((DMAs[0].Cnt & 0x0000FFFF) | (val << 16)); return;
    case 0x040000C4: DMAs[1].WriteCnt((DMAs[1].Cnt & 0xFFFF0000) | val); return;
    case 0x040000C6: DMAs[1].WriteCnt((DMAs[1].Cnt & 0x0000FFFF) | (val << 16)); return;
    case 0x040000D0: DMAs[2].WriteCnt((DMAs[2].Cnt & 0xFFFF0000) | val); return;
    case 0x040000D2: DMAs[2].WriteCnt((DMAs[2].Cnt & 0x0000FFFF) | (val << 16)); return;
    case 0x040000DC: DMAs[3].WriteCnt((DMAs[3].Cnt & 0xFFFF0000) | val); return;
    case 0x040000DE: DMAs[3].WriteCnt((DMAs[3].Cnt & 0x0000FFFF) | (val << 16)); return;

    case 0x040000E0: DMA9Fill[0] = (DMA9Fill[0] & 0xFFFF0000) | val; return;
    case 0x040000E2: DMA9Fill[0] = (DMA9Fill[0] & 0x0000FFFF) | (val << 16); return;
    case 0x040000E4: DMA9Fill[1] = (DMA9Fill[1] & 0xFFFF0000) | val; return;
    case 0x040000E6: DMA9Fill[1] = (DMA9Fill[1] & 0x0000FFFF) | (val << 16); return;
    case 0x040000E8: DMA9Fill[2] = (DMA9Fill[2] & 0xFFFF0000) | val; return;
    case 0x040000EA: DMA9Fill[2] = (DMA9Fill[2] & 0x0000FFFF) | (val << 16); return;
    case 0x040000EC: DMA9Fill[3] = (DMA9Fill[3] & 0xFFFF0000) | val; return;
    case 0x040000EE: DMA9Fill[3] = (DMA9Fill[3] & 0x0000FFFF) | (val << 16); return;

    case 0x04000100: Timers[0].Reload = val; return;
    case 0x04000102: TimerStart(0, val); return;
    case 0x04000104: Timers[1].Reload = val; return;
    case 0x04000106: TimerStart(1, val); return;
    case 0x04000108: Timers[2].Reload = val; return;
    case 0x0400010A: TimerStart(2, val); return;
    case 0x0400010C: Timers[3].Reload = val; return;
    case 0x0400010E: TimerStart(3, val); return;

    case 0x04000132:
        KeyCnt[0] = val;
        return;

    case 0x04000180:
        IPCSync7 &= 0xFFF0;
        IPCSync7 |= ((val & 0x0F00) >> 8);
        IPCSync9 &= 0xB0FF;
        IPCSync9 |= (val & 0x4F00);
        if ((val & 0x2000) && (IPCSync7 & 0x4000))
        {
            SetIRQ(1, IRQ_IPCSync);
        }
        return;

    case 0x04000184:
        if (val & 0x0008)
            IPCFIFO9.Clear();
        if ((val & 0x0004) && (!(IPCFIFOCnt9 & 0x0004)) && IPCFIFO9.IsEmpty())
            SetIRQ(0, IRQ_IPCSendDone);
        if ((val & 0x0400) && (!(IPCFIFOCnt9 & 0x0400)) && (!IPCFIFO7.IsEmpty()))
            SetIRQ(0, IRQ_IPCRecv);
        if (val & 0x4000)
            IPCFIFOCnt9 &= ~0x4000;
        IPCFIFOCnt9 = (val & 0x8404) | (IPCFIFOCnt9 & 0x4000);
        return;

    case 0x04000188:
        NDS::ARM9IOWrite32(addr, val | (val << 16));
        return;

    case 0x040001A0:
        NDSCartSlots[0]->WriteSPICnt(0, val, 0xFFFF);
        return;
    case 0x040001A2:
        NDSCartSlots[0]->WriteSPIData(0, val & 0xFF);
        return;

    case 0x040001A4:
        NDSCartSlots[0]->WriteROMCnt(0, val, 0x0000FFFF);
        return;
    case 0x040001A6:
        NDSCartSlots[0]->WriteROMCnt(0, val << 16, 0xFFFF0000);
        return;

    case 0x040001A8:
        NDSCartSlots[0]->WriteROMCommand(0, 0, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 1, val >> 8);
        return;
    case 0x040001AA:
        NDSCartSlots[0]->WriteROMCommand(0, 2, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 3, val >> 8);
        return;
    case 0x040001AC:
        NDSCartSlots[0]->WriteROMCommand(0, 4, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 5, val >> 8);
        return;
    case 0x040001AE:
        NDSCartSlots[0]->WriteROMCommand(0, 6, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 7, val >> 8);
        return;

    case 0x040001B8:
        NDSCartSlots[0]->WriteKey2Seed0(0, (u64)val << 32, 0x7F00000000ULL);
        return;
    case 0x040001BA:
        NDSCartSlots[0]->WriteKey2Seed1(0, (u64)val << 32, 0x7F00000000ULL);
        return;

    case 0x04000204:
        SetExMemCnt(0, val, 0xFFFF);
        return;

    case 0x04000208: IME[0] = val & 0x1; UpdateIRQ(0); return;
    case 0x04000210: IE[0] = (IE[0] & 0xFFFF0000) | val; UpdateIRQ(0); return;
    case 0x04000212: IE[0] = (IE[0] & 0x0000FFFF) | (val << 16); UpdateIRQ(0); return;
    // TODO: what happens when writing to IF this way??
    case 0x04000214: IF[0] &= ~val; GPU.GPU3D.CheckFIFOIRQ(); UpdateIRQ(0); return;
    case 0x04000216: IF[0] &= ~(val<<16); GPU.GPU3D.CheckFIFOIRQ(); UpdateIRQ(0); return;

    case 0x04000240:
        GPU.MapVRAM_AB(0, val & 0xFF);
        GPU.MapVRAM_AB(1, val >> 8);
        return;
    case 0x04000242:
        GPU.MapVRAM_CD(2, val & 0xFF);
        GPU.MapVRAM_CD(3, val >> 8);
        return;
    case 0x04000244:
        GPU.MapVRAM_E(4, val & 0xFF);
        GPU.MapVRAM_FG(5, val >> 8);
        return;
    case 0x04000246:
        GPU.MapVRAM_FG(6, val & 0xFF);
        MapSharedWRAM(val >> 8);
        return;
    case 0x04000248:
        GPU.MapVRAM_H(7, val & 0xFF);
        GPU.MapVRAM_I(8, val >> 8);
        return;

    case 0x04000280: DivCnt = val; StartDiv(); return;

    case 0x040002B0: SqrtCnt = val; StartSqrt(); return;

    case 0x04000300:
        if (PostFlag9 & 0x01) val |= 0x01;
        PostFlag9 = val & 0x03;
        return;

    case 0x04000304:
        PowerControl9 = val & 0x820F;
        GPU.SetPowerCnt(PowerControl9);
        return;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        GPU.GPU2D_A.Write16(addr, val);
        return;
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        GPU.GPU2D_B.Write16(addr, val);
        return;
    }
    if (addr >= 0x04000320 && addr < 0x040006A4)
    {
        GPU.GPU3D.Write16(addr, val);
        return;
    }

    Log(LogLevel::Debug, "unknown ARM9 IO write16 %08X %04X %08X\n", addr, val, ARM9.R[15]);
}

void NDS::ARM9IOWrite32(u32 addr, u32 val)
{
    switch (addr)
    {
    case 0x04000004:
        GPU.SetDispStat(0, val & 0xFFFF, 0xFFFF);
        GPU.SetVCount(val >> 16, 0xFFFF);
        return;

    case 0x04000060: GPU.GPU3D.Write32(addr, val); return;
    case 0x04000064:
    case 0x04000068:
    case 0x0400006C:
    case 0x0400106C: GPU.Write32(addr, val); return;

    case 0x040000B0: DMAs[0].SrcAddr = val; return;
    case 0x040000B4: DMAs[0].DstAddr = val; return;
    case 0x040000B8: DMAs[0].WriteCnt(val); return;
    case 0x040000BC: DMAs[1].SrcAddr = val; return;
    case 0x040000C0: DMAs[1].DstAddr = val; return;
    case 0x040000C4: DMAs[1].WriteCnt(val); return;
    case 0x040000C8: DMAs[2].SrcAddr = val; return;
    case 0x040000CC: DMAs[2].DstAddr = val; return;
    case 0x040000D0: DMAs[2].WriteCnt(val); return;
    case 0x040000D4: DMAs[3].SrcAddr = val; return;
    case 0x040000D8: DMAs[3].DstAddr = val; return;
    case 0x040000DC: DMAs[3].WriteCnt(val); return;

    case 0x040000E0: DMA9Fill[0] = val; return;
    case 0x040000E4: DMA9Fill[1] = val; return;
    case 0x040000E8: DMA9Fill[2] = val; return;
    case 0x040000EC: DMA9Fill[3] = val; return;

    case 0x04000100:
        Timers[0].Reload = val & 0xFFFF;
        TimerStart(0, val>>16);
        return;
    case 0x04000104:
        Timers[1].Reload = val & 0xFFFF;
        TimerStart(1, val>>16);
        return;
    case 0x04000108:
        Timers[2].Reload = val & 0xFFFF;
        TimerStart(2, val>>16);
        return;
    case 0x0400010C:
        Timers[3].Reload = val & 0xFFFF;
        TimerStart(3, val>>16);
        return;

    case 0x04000130:
        KeyCnt[0] = val >> 16;
        return;

    case 0x04000180:
    case 0x04000184:
        NDS::ARM9IOWrite16(addr, val);
        return;
    case 0x04000188:
        if (IPCFIFOCnt9 & 0x8000)
        {
            if (IPCFIFO9.IsFull())
                IPCFIFOCnt9 |= 0x4000;
            else
            {
                TraceNSMLSoundCommandList(*this, val);
                TraceNSMLIPC9Send(*this, val);
                bool wasempty = IPCFIFO9.IsEmpty();
                IPCFIFO9.Write(val);
                if ((IPCFIFOCnt7 & 0x0400) && wasempty)
                    SetIRQ(1, IRQ_IPCRecv);
            }
        }
        return;

    case 0x040001A0:
        NDSCartSlots[0]->WriteSPICnt(0, val & 0xFFFF, 0xFFFF);
        NDSCartSlots[0]->WriteSPIData(0, (val >> 16) & 0xFF);
        return;
    case 0x040001A4:
        NDSCartSlots[0]->WriteROMCnt(0, val, 0xFFFFFFFF);
        return;

    case 0x040001A8:
        NDSCartSlots[0]->WriteROMCommand(0, 0, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 1, (val >> 8) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 2, (val >> 16) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 3, val >> 24);
        return;
    case 0x040001AC:
        NDSCartSlots[0]->WriteROMCommand(0, 4, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 5, (val >> 8) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 6, (val >> 16) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(0, 7, val >> 24);
        return;

    case 0x040001B0:
        NDSCartSlots[0]->WriteKey2Seed0(0, (u64)val, 0x00FFFFFFFFULL);
        return;
    case 0x040001B4:
        NDSCartSlots[0]->WriteKey2Seed1(0, (u64)val, 0x00FFFFFFFFULL);
        return;

    case 0x04000208: IME[0] = val & 0x1; UpdateIRQ(0); return;
    case 0x04000210: IE[0] = val; UpdateIRQ(0); return;
    case 0x04000214: IF[0] &= ~val; GPU.GPU3D.CheckFIFOIRQ(); UpdateIRQ(0); return;

    case 0x04000240:
        GPU.MapVRAM_AB(0, val & 0xFF);
        GPU.MapVRAM_AB(1, (val >> 8) & 0xFF);
        GPU.MapVRAM_CD(2, (val >> 16) & 0xFF);
        GPU.MapVRAM_CD(3, val >> 24);
        return;
    case 0x04000244:
        GPU.MapVRAM_E(4, val & 0xFF);
        GPU.MapVRAM_FG(5, (val >> 8) & 0xFF);
        GPU.MapVRAM_FG(6, (val >> 16) & 0xFF);
        MapSharedWRAM(val >> 24);
        return;
    case 0x04000248:
        GPU.MapVRAM_H(7, val & 0xFF);
        GPU.MapVRAM_I(8, (val >> 8) & 0xFF);
        return;

    case 0x04000280: DivCnt = val; StartDiv(); return;

    case 0x040002B0: SqrtCnt = val; StartSqrt(); return;

    case 0x04000290: DivNumerator[0] = val; StartDiv(); return;
    case 0x04000294: DivNumerator[1] = val; StartDiv(); return;
    case 0x04000298: DivDenominator[0] = val; StartDiv(); return;
    case 0x0400029C: DivDenominator[1] = val; StartDiv(); return;

    case 0x040002B8: SqrtVal[0] = val; StartSqrt(); return;
    case 0x040002BC: SqrtVal[1] = val; StartSqrt(); return;

    case 0x04000304:
        PowerControl9 = val & 0x820F;
        GPU.SetPowerCnt(PowerControl9);
        return;

    case 0x04100010:
        NDSCartSlots[0]->WriteROMData(0, val, 0xFFFFFFFF);
        return;

    // NO$GBA debug register "String Out (raw)"
    case 0x04FFFA10:
        {
            char output[1024] = { 0 };
            char ch = '.';
            for (size_t i = 0; i < 1023 && ch != '\0'; i++)
            {
                ch = NDS::ARM9Read8(val + i);
                output[i] = ch;
            }
            Log(LogLevel::Debug, "%s", output);
            return;
        }

    // NO$GBA debug registers "String Out (with parameters)" and "String Out (with parameters, plus linefeed)"
    case 0x04FFFA14:
    case 0x04FFFA18:
        {
            NocashPrint(0, val, 0x04FFFA18 == addr);

            return;
        }

    // NO$GBA debug register "Char Out"
        case 0x04FFFA1C: Log(LogLevel::Debug, "%c", val & 0xFF); return;

    // Diagnostic-only NSMB rollback stage marker.  Stable ROMs never write this
    // otherwise-unused NO$GBA debug-register slot.
    case 0x04FFFA28: RecordNSMLRomGameTickProbeStage(this, val); return;
    }

    if (addr >= 0x04000000 && addr < 0x04000060)
    {
        GPU.GPU2D_A.Write32(addr, val);
        return;
    }
    if (addr >= 0x04001000 && addr < 0x04001060)
    {
        GPU.GPU2D_B.Write32(addr, val);
        return;
    }
    if (addr >= 0x04000320 && addr < 0x040006A4)
    {
        GPU.GPU3D.Write32(addr, val);
        return;
    }

    Log(LogLevel::Debug, "unknown ARM9 IO write32 %08X %08X %08X\n", addr, val, ARM9.R[15]);
}


u8 NDS::ARM7IORead8(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU.DispStat[1] & 0xFF;
    case 0x04000005: return GPU.DispStat[1] >> 8;
    case 0x04000006: return GPU.VCount & 0xFF;
    case 0x04000007: return GPU.VCount >> 8;

    case 0x04000130: return KeyInput & 0xFF;
    case 0x04000131: return (KeyInput >> 8) & 0xFF;
    case 0x04000132: return KeyCnt[1] & 0xFF;
    case 0x04000133: return KeyCnt[1] >> 8;
    case 0x04000134: return RCnt & 0xFF;
    case 0x04000135: return RCnt >> 8;
    case 0x04000136: return (KeyInput >> 16) & 0xFF;
    case 0x04000137: return KeyInput >> 24;

    case 0x04000138: return RTC.Read() & 0xFF;

    case 0x04000180: return IPCSync7 & 0xFF;
    case 0x04000181: return IPCSync7 >> 8;

    case 0x040001A0: return NDSCartSlots[0]->ReadSPICnt(1) & 0xFF;
    case 0x040001A1: return NDSCartSlots[0]->ReadSPICnt(1) >> 8;
    case 0x040001A2: return NDSCartSlots[0]->ReadSPIData(1);
    case 0x040001A4: return NDSCartSlots[0]->ReadROMCnt(1) & 0xFF;
    case 0x040001A5: return (NDSCartSlots[0]->ReadROMCnt(1) >> 8) & 0xFF;
    case 0x040001A6: return (NDSCartSlots[0]->ReadROMCnt(1) >> 16) & 0xFF;
    case 0x040001A7: return NDSCartSlots[0]->ReadROMCnt(1) >> 24;

    case 0x040001C2: return SPI.ReadData();

    case 0x04000208: return IME[1];

    case 0x04000240: return GPU.VRAMSTAT;
    case 0x04000241: return WRAMCnt;

    case 0x04000300: return PostFlag7;
    case 0x04000304: return PowerControl7;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        return SPU.Read8(addr);
    }

    if ((addr & 0xFFFFF000) != 0x04004000)
        Log(LogLevel::Debug, "unknown ARM7 IO read8 %08X %08X\n", addr, ARM7.R[15]);
    return 0;
}

u16 NDS::ARM7IORead16(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU.DispStat[1];
    case 0x04000006: return GPU.VCount;

    case 0x040000B8: return DMAs[4].Cnt & 0xFFFF;
    case 0x040000BA: return DMAs[4].Cnt >> 16;
    case 0x040000C4: return DMAs[5].Cnt & 0xFFFF;
    case 0x040000C6: return DMAs[5].Cnt >> 16;
    case 0x040000D0: return DMAs[6].Cnt & 0xFFFF;
    case 0x040000D2: return DMAs[6].Cnt >> 16;
    case 0x040000DC: return DMAs[7].Cnt & 0xFFFF;
    case 0x040000DE: return DMAs[7].Cnt >> 16;

    case 0x04000100: return TimerGetCounter(4);
    case 0x04000102: return Timers[4].Cnt;
    case 0x04000104: return TimerGetCounter(5);
    case 0x04000106: return Timers[5].Cnt;
    case 0x04000108: return TimerGetCounter(6);
    case 0x0400010A: return Timers[6].Cnt;
    case 0x0400010C: return TimerGetCounter(7);
    case 0x0400010E: return Timers[7].Cnt;

    case 0x04000130: return KeyInput & 0xFFFF;
    case 0x04000132: return KeyCnt[1];
    case 0x04000134: return RCnt;
    case 0x04000136: return KeyInput >> 16;

    case 0x04000138: return RTC.Read();

    case 0x04000180: return IPCSync7;
    case 0x04000184:
        {
            u16 val = IPCFIFOCnt7;
            if (IPCFIFO7.IsEmpty())     val |= 0x0001;
            else if (IPCFIFO7.IsFull()) val |= 0x0002;
            if (IPCFIFO9.IsEmpty())     val |= 0x0100;
            else if (IPCFIFO9.IsFull()) val |= 0x0200;
            return val;
        }

    case 0x040001A0: return NDSCartSlots[0]->ReadSPICnt(1);
    case 0x040001A2: return NDSCartSlots[0]->ReadSPIData(1);
    case 0x040001A4: return NDSCartSlots[0]->ReadROMCnt(1) & 0xFFFF;
    case 0x040001A6: return NDSCartSlots[0]->ReadROMCnt(1) >> 16;

    case 0x040001C0: return SPI.ReadCnt();
    case 0x040001C2: return SPI.ReadData();

    case 0x04000204: return ExMemCnt[1];
    case 0x04000206:
        if (!(PowerControl7 & (1<<1))) return 0;
        return WifiWaitCnt;

    case 0x04000208: return IME[1];
    case 0x04000210: return IE[1] & 0xFFFF;
    case 0x04000212: return IE[1] >> 16;

    case 0x04000300: return PostFlag7;
    case 0x04000304: return PowerControl7;
    case 0x04000308: return ARM7BIOSProt;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        return SPU.Read16(addr);
    }

    if ((addr & 0xFFFFF000) != 0x04004000)
        Log(LogLevel::Debug, "unknown ARM7 IO read16 %08X %08X\n", addr, ARM7.R[15]);
    return 0;
}

u32 NDS::ARM7IORead32(u32 addr)
{
    switch (addr)
    {
    case 0x04000004: return GPU.DispStat[1] | (GPU.VCount << 16);

    case 0x040000B0: return DMAs[4].SrcAddr;
    case 0x040000B4: return DMAs[4].DstAddr;
    case 0x040000B8: return DMAs[4].Cnt;
    case 0x040000BC: return DMAs[5].SrcAddr;
    case 0x040000C0: return DMAs[5].DstAddr;
    case 0x040000C4: return DMAs[5].Cnt;
    case 0x040000C8: return DMAs[6].SrcAddr;
    case 0x040000CC: return DMAs[6].DstAddr;
    case 0x040000D0: return DMAs[6].Cnt;
    case 0x040000D4: return DMAs[7].SrcAddr;
    case 0x040000D8: return DMAs[7].DstAddr;
    case 0x040000DC: return DMAs[7].Cnt;

    case 0x04000100: return TimerGetCounter(4) | (Timers[4].Cnt << 16);
    case 0x04000104: return TimerGetCounter(5) | (Timers[5].Cnt << 16);
    case 0x04000108: return TimerGetCounter(6) | (Timers[6].Cnt << 16);
    case 0x0400010C: return TimerGetCounter(7) | (Timers[7].Cnt << 16);

    case 0x04000130: return (KeyInput & 0xFFFF) | (KeyCnt[1] << 16);
    case 0x04000134: return RCnt | (KeyInput & 0xFFFF0000);
    case 0x04000138: return RTC.Read();

    case 0x04000180: return IPCSync7;
    case 0x04000184: return NDS::ARM7IORead16(addr);

    case 0x040001A0: return NDSCartSlots[0]->ReadSPICnt(1) | (NDSCartSlots[0]->ReadSPIData(1) << 16);
    case 0x040001A4: return NDSCartSlots[0]->ReadROMCnt(1);

    case 0x040001C0:
        return SPI.ReadCnt() | (SPI.ReadData() << 16);

    case 0x04000208: return IME[1];
    case 0x04000210: return IE[1];
    case 0x04000214: return IF[1];

    case 0x04000304: return PowerControl7;
    case 0x04000308: return ARM7BIOSProt;

    case 0x04100000:
        if (IPCFIFOCnt7 & 0x8000)
        {
            u32 ret;
            if (IPCFIFO9.IsEmpty())
            {
                IPCFIFOCnt7 |= 0x4000;
                ret = IPCFIFO9.Peek();
            }
            else
            {
                ret = IPCFIFO9.Read();

                if (IPCFIFO9.IsEmpty() && (IPCFIFOCnt9 & 0x0004))
                    SetIRQ(0, IRQ_IPCSendDone);
            }
            return ret;
        }
        else
            return IPCFIFO9.Peek();

    case 0x04100010:
        return NDSCartSlots[0]->ReadROMData(1);
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        return SPU.Read32(addr);
    }

    if ((addr & 0xFFFFF000) != 0x04004000)
        Log(LogLevel::Debug, "unknown ARM7 IO read32 %08X %08X\n", addr, ARM7.R[15]);
    return 0;
}

void NDS::ARM7IOWrite8(u32 addr, u8 val)
{
    switch (addr)
    {
    case 0x04000004: GPU.SetDispStat(1, val, 0x00FF); return;
    case 0x04000005: GPU.SetDispStat(1, val << 8, 0xFF00); return;
    case 0x04000006: GPU.SetVCount(val, 0x00FF); return;
    case 0x04000007: GPU.SetVCount(val << 8, 0xFF00); return;

    case 0x04000132:
        KeyCnt[1] = (KeyCnt[1] & 0xFF00) | val;
        return;
    case 0x04000133:
        KeyCnt[1] = (KeyCnt[1] & 0x00FF) | (val << 8);
        return;
    case 0x04000134:
        RCnt = (RCnt & 0xFF00) | val;
        return;
    case 0x04000135:
        RCnt = (RCnt & 0x00FF) | (val << 8);
        return;

    case 0x04000138: RTC.Write(val, true); return;

    case 0x04000181:
        IPCSync9 &= 0xFFF0;
        IPCSync9 |= (val & 0x0F);
        IPCSync7 &= 0xB0FF;
        IPCSync7 |= ((val & 0x4F) << 8);
        if ((val & 0x20) && (IPCSync9 & 0x4000))
        {
            SetIRQ(0, IRQ_IPCSync);
        }
        return;

    case 0x04000188:
        NDS::ARM7IOWrite32(addr, val | (val << 8) | (val << 16) | (val << 24));
        return;

    case 0x040001A0:
        NDSCartSlots[0]->WriteSPICnt(1, val, 0x00FF);
        return;
    case 0x040001A1:
        NDSCartSlots[0]->WriteSPICnt(1, val << 8, 0xFF00);
        return;
    case 0x040001A2:
        NDSCartSlots[0]->WriteSPIData(1, val);
        return;

    case 0x040001A4:
        NDSCartSlots[0]->WriteROMCnt(1, val, 0x000000FF);
        return;
    case 0x040001A5:
        NDSCartSlots[0]->WriteROMCnt(1, val << 8, 0x0000FF00);
        return;
    case 0x040001A6:
        NDSCartSlots[0]->WriteROMCnt(1, val << 16, 0x00FF0000);
        return;
    case 0x040001A7:
        NDSCartSlots[0]->WriteROMCnt(1, val << 24, 0xFF000000);
        return;

    case 0x040001A8: NDSCartSlots[0]->WriteROMCommand(1, 0, val); return;
    case 0x040001A9: NDSCartSlots[0]->WriteROMCommand(1, 1, val); return;
    case 0x040001AA: NDSCartSlots[0]->WriteROMCommand(1, 2, val); return;
    case 0x040001AB: NDSCartSlots[0]->WriteROMCommand(1, 3, val); return;
    case 0x040001AC: NDSCartSlots[0]->WriteROMCommand(1, 4, val); return;
    case 0x040001AD: NDSCartSlots[0]->WriteROMCommand(1, 5, val); return;
    case 0x040001AE: NDSCartSlots[0]->WriteROMCommand(1, 6, val); return;
    case 0x040001AF: NDSCartSlots[0]->WriteROMCommand(1, 7, val); return;

    case 0x040001C2:
        SPI.WriteData(val);
        return;

    case 0x04000208: IME[1] = val & 0x1; UpdateIRQ(1); return;

    case 0x04000300:
        if (ARM7.R[15] >= 0x4000)
            return;
        if (!(PostFlag7 & 0x01))
            PostFlag7 = val & 0x01;
        return;

    case 0x04000301:
        val &= 0xC0;
        if      (val == 0x40) Stop(StopReason::GBAModeNotSupported);
        else if (val == 0x80) ARM7.Halt(1);
        else if (val == 0xC0) EnterSleepMode();
        return;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        SPU.Write8(addr, val);
        return;
    }

    Log(LogLevel::Debug, "unknown ARM7 IO write8 %08X %02X %08X\n", addr, val, ARM7.R[15]);
}

void NDS::ARM7IOWrite16(u32 addr, u16 val)
{
    switch (addr)
    {
    case 0x04000004: GPU.SetDispStat(1, val, 0xFFFF); return;
    case 0x04000006: GPU.SetVCount(val, 0xFFFF); return;

    case 0x040000B8: DMAs[4].WriteCnt((DMAs[4].Cnt & 0xFFFF0000) | val); return;
    case 0x040000BA: DMAs[4].WriteCnt((DMAs[4].Cnt & 0x0000FFFF) | (val << 16)); return;
    case 0x040000C4: DMAs[5].WriteCnt((DMAs[5].Cnt & 0xFFFF0000) | val); return;
    case 0x040000C6: DMAs[5].WriteCnt((DMAs[5].Cnt & 0x0000FFFF) | (val << 16)); return;
    case 0x040000D0: DMAs[6].WriteCnt((DMAs[6].Cnt & 0xFFFF0000) | val); return;
    case 0x040000D2: DMAs[6].WriteCnt((DMAs[6].Cnt & 0x0000FFFF) | (val << 16)); return;
    case 0x040000DC: DMAs[7].WriteCnt((DMAs[7].Cnt & 0xFFFF0000) | val); return;
    case 0x040000DE: DMAs[7].WriteCnt((DMAs[7].Cnt & 0x0000FFFF) | (val << 16)); return;

    case 0x04000100: Timers[4].Reload = val; return;
    case 0x04000102: TimerStart(4, val); return;
    case 0x04000104: Timers[5].Reload = val; return;
    case 0x04000106: TimerStart(5, val); return;
    case 0x04000108: Timers[6].Reload = val; return;
    case 0x0400010A: TimerStart(6, val); return;
    case 0x0400010C: Timers[7].Reload = val; return;
    case 0x0400010E: TimerStart(7, val); return;

    case 0x04000132: KeyCnt[1] = val; return;
    case 0x04000134: RCnt = val; return;

    case 0x04000138: RTC.Write(val, false); return;

    case 0x04000180:
        IPCSync9 &= 0xFFF0;
        IPCSync9 |= ((val & 0x0F00) >> 8);
        IPCSync7 &= 0xB0FF;
        IPCSync7 |= (val & 0x4F00);
        if ((val & 0x2000) && (IPCSync9 & 0x4000))
        {
            SetIRQ(0, IRQ_IPCSync);
        }
        return;

    case 0x04000184:
        if (val & 0x0008)
            IPCFIFO7.Clear();
        if ((val & 0x0004) && (!(IPCFIFOCnt7 & 0x0004)) && IPCFIFO7.IsEmpty())
            SetIRQ(1, IRQ_IPCSendDone);
        if ((val & 0x0400) && (!(IPCFIFOCnt7 & 0x0400)) && (!IPCFIFO9.IsEmpty()))
            SetIRQ(1, IRQ_IPCRecv);
        if (val & 0x4000)
            IPCFIFOCnt7 &= ~0x4000;
        IPCFIFOCnt7 = (val & 0x8404) | (IPCFIFOCnt7 & 0x4000);
        return;

    case 0x04000188:
        NDS::ARM7IOWrite32(addr, val | (val << 16));
        return;

    case 0x040001A0:
        NDSCartSlots[0]->WriteSPICnt(1, val, 0xFFFF);
        return;
    case 0x040001A2:
        NDSCartSlots[0]->WriteSPIData(1, val & 0xFF);
        return;

    case 0x040001A4:
        NDSCartSlots[0]->WriteROMCnt(1, val, 0x0000FFFF);
        return;
    case 0x040001A6:
        NDSCartSlots[0]->WriteROMCnt(1, val << 16, 0xFFFF0000);
        return;

    case 0x040001A8:
        NDSCartSlots[0]->WriteROMCommand(1, 0, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 1, val >> 8);
        return;
    case 0x040001AA:
        NDSCartSlots[0]->WriteROMCommand(1, 2, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 3, val >> 8);
        return;
    case 0x040001AC:
        NDSCartSlots[0]->WriteROMCommand(1, 4, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 5, val >> 8);
        return;
    case 0x040001AE:
        NDSCartSlots[0]->WriteROMCommand(1, 6, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 7, val >> 8);
        return;

    case 0x040001B8:
        NDSCartSlots[0]->WriteKey2Seed0(1, (u64)val << 32, 0x7F00000000ULL);
        return;
    case 0x040001BA:
        NDSCartSlots[0]->WriteKey2Seed1(1, (u64)val << 32, 0x7F00000000ULL);
        return;

    case 0x040001C0:
        SPI.WriteCnt(val);
        return;
    case 0x040001C2:
        SPI.WriteData(val & 0xFF);
        return;

    case 0x04000204:
        SetExMemCnt(1, val, 0xFFFF);
        return;

    case 0x04000206:
        if (!(PowerControl7 & (1<<1))) return;
        SetWifiWaitCnt(val);
        return;

    case 0x04000208: IME[1] = val & 0x1; UpdateIRQ(1); return;
    case 0x04000210: IE[1] = (IE[1] & 0xFFFF0000) | val; UpdateIRQ(1); return;
    case 0x04000212: IE[1] = (IE[1] & 0x0000FFFF) | (val << 16); UpdateIRQ(1); return;
    // TODO: what happens when writing to IF this way??

    case 0x04000300:
        if (ARM7.R[15] >= 0x4000)
            return;
        if (!(PostFlag7 & 0x01))
            PostFlag7 = val & 0x01;
        return;

    case 0x04000304:
        {
            u16 change = PowerControl7 ^ val;
            PowerControl7 = val & 0x0003;
            SPU.SetPowerCnt(val & 0x0001);
            Wifi.SetPowerCnt(val & 0x0002);
            if (change & 0x0002) UpdateWifiTimings();
        }
        return;

    case 0x04000308:
        if (ARM7BIOSProt == 0)
            ARM7BIOSProt = val & 0xFFFE;
        return;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        SPU.Write16(addr, val);
        return;
    }

    Log(LogLevel::Debug, "unknown ARM7 IO write16 %08X %04X %08X\n", addr, val, ARM7.R[15]);
}

void NDS::ARM7IOWrite32(u32 addr, u32 val)
{
    switch (addr)
    {
    case 0x04000004:
        GPU.SetDispStat(1, val & 0xFFFF, 0xFFFF);
        GPU.SetVCount(val >> 16, 0xFFFF);
        return;

    case 0x040000B0: DMAs[4].SrcAddr = val; return;
    case 0x040000B4: DMAs[4].DstAddr = val; return;
    case 0x040000B8: DMAs[4].WriteCnt(val); return;
    case 0x040000BC: DMAs[5].SrcAddr = val; return;
    case 0x040000C0: DMAs[5].DstAddr = val; return;
    case 0x040000C4: DMAs[5].WriteCnt(val); return;
    case 0x040000C8: DMAs[6].SrcAddr = val; return;
    case 0x040000CC: DMAs[6].DstAddr = val; return;
    case 0x040000D0: DMAs[6].WriteCnt(val); return;
    case 0x040000D4: DMAs[7].SrcAddr = val; return;
    case 0x040000D8: DMAs[7].DstAddr = val; return;
    case 0x040000DC: DMAs[7].WriteCnt(val); return;

    case 0x04000100:
        Timers[4].Reload = val & 0xFFFF;
        TimerStart(4, val>>16);
        return;
    case 0x04000104:
        Timers[5].Reload = val & 0xFFFF;
        TimerStart(5, val>>16);
        return;
    case 0x04000108:
        Timers[6].Reload = val & 0xFFFF;
        TimerStart(6, val>>16);
        return;
    case 0x0400010C:
        Timers[7].Reload = val & 0xFFFF;
        TimerStart(7, val>>16);
        return;

    case 0x04000130: KeyCnt[1] = val >> 16; return;
    case 0x04000134: RCnt = val & 0xFFFF; return;
    case 0x04000138: RTC.Write(val & 0xFFFF, false); return;

    case 0x04000180:
    case 0x04000184:
        NDS::ARM7IOWrite16(addr, val);
        return;
    case 0x04000188:
        if (IPCFIFOCnt7 & 0x8000)
        {
            if (IPCFIFO7.IsFull())
                IPCFIFOCnt7 |= 0x4000;
            else
            {
                TraceNSMLIPC7Send(*this, val);
                bool wasempty = IPCFIFO7.IsEmpty();
                IPCFIFO7.Write(val);
                if ((IPCFIFOCnt9 & 0x0400) && wasempty)
                    SetIRQ(0, IRQ_IPCRecv);
            }
        }
        return;

    case 0x040001A0:
        NDSCartSlots[0]->WriteSPICnt(1, val & 0xFFFF, 0xFFFF);
        NDSCartSlots[0]->WriteSPIData(1, (val >> 16) & 0xFF);
        return;
    case 0x040001A4:
        NDSCartSlots[0]->WriteROMCnt(1, val, 0xFFFFFFFF);
        return;

    case 0x040001A8:
        NDSCartSlots[0]->WriteROMCommand(1, 0, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 1, (val >> 8) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 2, (val >> 16) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 3, val >> 24);
        return;
    case 0x040001AC:
        NDSCartSlots[0]->WriteROMCommand(1, 4, val & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 5, (val >> 8) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 6, (val >> 16) & 0xFF);
        NDSCartSlots[0]->WriteROMCommand(1, 7, val >> 24);
        return;

    case 0x040001B0:
        NDSCartSlots[0]->WriteKey2Seed0(1, (u64)val, 0x00FFFFFFFFULL);
        return;
    case 0x040001B4:
        NDSCartSlots[0]->WriteKey2Seed1(1, (u64)val, 0x00FFFFFFFFULL);
        return;

    case 0x040001C0:
        SPI.WriteCnt(val & 0xFFFF);
        SPI.WriteData((val >> 16) & 0xFF);
        return;

    case 0x04000208: IME[1] = val & 0x1; UpdateIRQ(1); return;
    case 0x04000210: IE[1] = val; UpdateIRQ(1); return;
    case 0x04000214: IF[1] &= ~val; UpdateIRQ(1); return;

    case 0x04000304:
        {
            u16 change = PowerControl7 ^ val;
            PowerControl7 = val & 0x0003;
            SPU.SetPowerCnt(val & 0x0001);
            Wifi.SetPowerCnt(val & 0x0002);
            if (change & 0x0002) UpdateWifiTimings();
        }
        return;

    case 0x04000308:
        if (ARM7BIOSProt == 0)
            ARM7BIOSProt = val & 0xFFFE;
        return;

    case 0x04100010:
        NDSCartSlots[0]->WriteROMData(1, val, 0xFFFFFFFF);
        return;
    }

    if (addr >= 0x04000400 && addr < 0x04000520)
    {
        SPU.Write32(addr, val);
        return;
    }

    Log(LogLevel::Debug, "unknown ARM7 IO write32 %08X %08X %08X\n", addr, val, ARM7.R[15]);
}

}
