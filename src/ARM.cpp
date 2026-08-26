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

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "NDS.h"
#include "DSi.h"
#include "ARM.h"
#include "ARMInterpreter.h"
#include "AREngine.h"
#include "ARMJIT.h"
#include "Platform.h"
#include "GPU.h"
#include "ARMJIT_Memory.h"
#if defined(JIT_ENABLED) && defined(__x86_64__)
#include "ARMJIT_x64/ARMJIT_Offsets.h"
#endif

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

static std::mutex NSMLTraceConfigMutex;
static std::mutex NSMLTraceOutputMutex;
static std::mutex NSMLPacketBridgeMutex;

struct NSMLPacketReplayEntry
{
    bool Valid[2] {};
    std::array<u8, 52> Packet[2] {};
};

struct NSMLLocalPacketCapture
{
    bool Available = false;
    u32 Tick = 0;
    u32 Keys = 0;
    u32 Frame = 0;
    std::array<u8, 52> Packet {};
};

static std::map<NDS*, NSMLLocalPacketCapture> NSMLLocalPackets;
static std::map<NDS*, std::map<u32, NSMLPacketReplayEntry>> NSMLLiveReplayPackets;

static bool NSMLEnvFlag(const char* name)
{
    const char* value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static u32 NSMLEnvU32(const char* name, u32 fallback)
{
    const char* value = getenv(name);
    if (!value || !value[0])
        return fallback;
    return static_cast<u32>(strtoul(value, nullptr, 0));
}

static bool NSMLBadJumpTraceEnabled()
{
#ifdef NSML_CORE_RUNTIME_HOOKS_DISABLED
    return false;
#else
    // JumpTo() is an ARM9 hot path. The smoke runners set this launch-time flag
    // before creating the process, so cache it instead of calling getenv per jump.
    static const bool enabled = NSMLEnvFlag("MELONDS_NSML_BAD_JUMP_TRACE");
    return enabled;
#endif
}

static u32 NSMLMvlStage()
{
    u32 stage = NSMLEnvU32("MELONDS_NSML_MVL_STAGE", 0);
    return std::min(stage, 4u);
}

static bool NSMLRomLoopRollbackConfigured()
{
    const char* backend = getenv("MELONDS_NSML_ROLLBACK_BACKEND");
    return backend &&
        (!strcmp(backend, "romloop") || !strcmp(backend, "rom-loop") ||
         !strcmp(backend, "slippi"));
}

static bool NSMLRuntimeHooksMaybeEnabled()
{
#ifdef NSML_CORE_RUNTIME_HOOKS_DISABLED
    return false;
#else
    static const bool enabled =
        NSMLRomLoopRollbackConfigured() ||
        NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE") ||
        NSMLEnvFlag("MELONDS_NSML_PACKET_CAPTURE_LOG") ||
        NSMLEnvFlag("MELONDS_NSML_PACKET_REPLAY_FILE") ||
        NSMLEnvFlag("MELONDS_NSML_RANDOM_TRACE") ||
        NSMLEnvFlag("MELONDS_NSML_CALL_TRACE") ||
        NSMLEnvFlag("MELONDS_NSML_TRACE_STAGE_CAMERA") ||
        NSMLEnvFlag("MELONDS_NSML_TRACE_PLAYER_RENDER") ||
        NSMLEnvFlag("MELONDS_NSML_TRACE_PLAYER_DEFEATED") ||
        NSMLEnvFlag("MELONDS_NSML_TRACE_PLAYER_LIFE_CALLS") ||
        NSMLEnvFlag("MELONDS_NSML_GAME_TICK_PROBE") ||
        NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE") ||
        NSMLEnvFlag("MELONDS_NSML_GUARD_PLAYER_MODEL_RENDER_PTRS") ||
        NSMLEnvFlag("MELONDS_NSML_DYNAMIC_CAMERA_LEAD") ||
        NSMLEnvFlag("MELONDS_NSML_RENDER_CAMERA_ALIAS") ||
        NSMLEnvFlag("MELONDS_NSML_FORCE_CAMERA_FOCUS_LOOP_COUNT") ||
        NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_BYPASS_NET_DISCONNECT") ||
        NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_FORCE_TRANSFER_RESULT");
    return enabled;
#endif
}

static u32 NSMLPacketBridgeEnvFrame(const char* name, u32 fallback);

static bool NSMLPacketBridgeEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE") ? 1 : 0;
    return enabled != 0;
}

static u32 NSMLPacketBridgeLocalPlayer()
{
    if (const char* value = getenv("MELONDS_NSML_PACKET_BRIDGE_LOCAL_PLAYER"))
        return static_cast<u32>(strtoul(value, nullptr, 0)) & 1u;
    if (const char* role = getenv("MELONDS_NSML_ROLE"))
    {
        if (!strcmp(role, "client"))
            return 1;
    }
    if (const char* role = getenv("MELONDS_NSML_LAN_ROLE"))
    {
        if (!strcmp(role, "client"))
            return 1;
    }
    return 0;
}


static bool IsNSMLMarioVsLuigiPacketContext(NDS& nds);

static bool IsNSMLMarioVsLuigiGGID(u32 value)
{
    // A2DJ traces used the compact 0x42 GGID. US A2DE keeps the MvL group
    // identifier as 0x00400150 in the same runtime slot.
    return value == 0x42 || value == 0x00400150;
}

static bool IsNSMLMarioVsLuigiGameplay(NDS& nds)
{
    // Direct-MvL ROM patches can enter the VS stage without preserving the
    // normal Net GGID slot. In gameplay, stageGroup=9 and vsMode=1 are the
    // stronger signal for this PoC path; requiring GGID here disables the WAN
    // packet adapter exactly when the stage has started.
    return nds.ARM9Read32(0x02085A18) == 9
        && nds.ARM9Read32(0x02085A84) == 1;
}

static bool NSMLPacketBridgeAllowPreGame()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_ALLOW_PRE_GAME") ? 1 : 0;
    return enabled != 0;
}

static bool NSMLPacketBridgeActiveForFrame(NDS& nds)
{
    if (!NSMLPacketBridgeEnabled())
        return false;

    static int deferUntilStart = -1;
    static u32 startFrame = 0xFFFFFFFF;
    if (deferUntilStart < 0)
        deferUntilStart = NSMLEnvFlag("MELONDS_NSML_DEFER_NETWORK_UNTIL_START") ? 1 : 0;
    if (startFrame == 0xFFFFFFFF)
        startFrame = NSMLPacketBridgeEnvFrame("MELONDS_NSML_NETPLAY_START_FRAME", 0);

    return !deferUntilStart || nds.NumFrames >= startFrame;
}

static bool IsNSMLMarioVsLuigiPacketContext(NDS& nds)
{
    if (NSMLPacketBridgeEnabled() && !NSMLPacketBridgeActiveForFrame(nds))
        return false;

    if (IsNSMLMarioVsLuigiGameplay(nds))
        return true;

    return NSMLPacketBridgeAllowPreGame()
        && IsNSMLMarioVsLuigiGGID(nds.ARM9Read32(0x02088858));
}

static u64 HashNSMLGameTickProbeMainRAM(NDS& nds)
{
    constexpr u64 fnvOffset = 1469598103934665603ull;
    constexpr u64 fnvPrime = 1099511628211ull;
    u64 hash = fnvOffset;
    const u32 length = std::min(nds.MainRAMMask + 1, 0x400000u);
    for (u32 offset = 0; offset < length; offset++)
    {
        hash ^= nds.MainRAM[offset];
        hash *= fnvPrime;
    }
    return hash;
}

static bool HandleNSMLRomGameTickProbe(ARM* cpu, u32 instrAddr)
{
    constexpr u32 loopStart = 0x02004EC8;
    constexpr u32 loopGateAfterCounter = 0x020019C0;
    constexpr u32 requestAddr = 0x02001AC0;
    constexpr u32 activeAddr = 0x02001AC4;
    constexpr u32 magicAddr = 0x02001AC8;
    constexpr u32 magic = 0x32505447;
    constexpr u32 netPacketTickAddr = 0x020888E0;
    constexpr u32 scratchTickAddr = 0x023C1200;
    constexpr u32 scratchKeysAddr = 0x023C1208;
    constexpr u32 scratchPacketsAddr = 0x023C1240;
    constexpr u32 historyEnabledAddr = 0x02001ACC;
    constexpr u32 historyIndexAddr = 0x02001AD0;
    constexpr u32 historyCountAddr = 0x02001AD4;
    constexpr u32 historyTargetAddr = 0x02001AD8;
    constexpr u32 historyStartFrameAddr = 0x02001ADC;
    constexpr u32 historyAddr = 0x023C1300;
    constexpr u32 historyEntrySize = 16;

    if (instrAddr == loopStart)
        cpu->NDS.ApplyNSMLPendingGameRAMRestore();
    if (instrAddr != loopStart && instrAddr != loopGateAfterCounter)
        return false;

    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        bool RestoreAfterExtra = false;
        bool RenderlessAB = false;
        bool HistoricalInputAB = false;
        bool GuestOwnedHistoryAB = false;
        u32 StartFrame = 900;
        u32 ExtraTicks = 1;
        std::string Role;
        std::string TargetRole = "host";
        std::string OutputDir;
        FILE* LogFile = nullptr;
    };
    struct State
    {
        bool TargetSeen = false;
        bool InExtraTicks = false;
        bool NeedNextNormalTick = false;
        bool NextNormalTickEntered = false;
        bool Done = false;
        bool RenderlessABTickPending = false;
        u32 ActualTargetFrame = 0;
        u32 ExtraTicksSeen = 0;
        u32 HistoricalBaseTick = 0;
        u32 InputSequenceHash = 2166136261u;
        std::vector<u8> SavedMainRAM;
    };

    static Config cfg;
    static std::map<NDS*, State> states;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    if (!cfg.Checked)
    {
        cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE") &&
            !NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE_FRAME_BOUNDARY");
        cfg.RestoreAfterExtra = NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE_RESTORE_AFTER_EXTRA");
        cfg.RenderlessAB = NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE_RENDERLESS_AB");
        cfg.HistoricalInputAB = NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE_HISTORICAL_INPUT_AB");
        cfg.GuestOwnedHistoryAB = NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE_GUEST_HISTORY_AB");
        cfg.StartFrame = NSMLEnvU32("MELONDS_NSML_ROM_GAME_TICK_PROBE_START_FRAME", 900);
        cfg.ExtraTicks = std::clamp(
            NSMLEnvU32("MELONDS_NSML_ROM_GAME_TICK_PROBE_EXTRA_TICKS", 1), 1u, 7u);
        if (const char* role = getenv("MELONDS_NSML_ROLE"))
            cfg.Role = role;
        if (cfg.Role.empty())
            cfg.Role = "local";
        if (const char* targetRole = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_TARGET_ROLE"))
            cfg.TargetRole = targetRole;
        if (const char* outputDir = getenv("MELONDS_NSML_ROM_GAME_TICK_PROBE_DIR"))
            cfg.OutputDir = outputDir;
        if (cfg.Enabled && !cfg.OutputDir.empty())
        {
            const std::string logPath = cfg.OutputDir + "/rom-game-tick-probe-" + cfg.Role + ".csv";
            cfg.LogFile = fopen(logPath.c_str(), "w");
            if (cfg.LogFile)
            {
                fprintf(cfg.LogFile,
                    "role,frame,phase,main_ram_hash,game_frame_counter,arm9_timestamp,arm7_timestamp,request,active,extra_ticks_seen,input_sequence_hash,scratch_tick,scratch_keys0,scratch_keys1,history_enabled,history_index,history_count\n");
                fflush(cfg.LogFile);
            }
        }
        cfg.Checked = true;
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0 || !cpu->NDS.MainRAM || cfg.OutputDir.empty() ||
        cpu->NDS.ARM9Read32(magicAddr) != magic)
        return false;

    State& state = states[&cpu->NDS];
    const bool isTarget = cfg.Role == cfg.TargetRole;

    auto dump = [&](const char* phase)
    {
        const u32 frame = cpu->NDS.NumFrames;
        const u64 mainRAMHash = HashNSMLGameTickProbeMainRAM(cpu->NDS);
        if (cfg.LogFile)
        {
            fprintf(cfg.LogFile, "%s,%u,%s,%016llX,%u,%llu,%llu,%u,%u,%u,%08X,%u,%u,%u,%u,%u,%u\n",
                cfg.Role.c_str(),
                frame,
                phase,
                static_cast<unsigned long long>(mainRAMHash),
                cpu->NDS.ARM9Read32(0x0208B668),
                static_cast<unsigned long long>(cpu->NDS.ARM9Timestamp),
                static_cast<unsigned long long>(cpu->NDS.ARM7Timestamp),
                cpu->NDS.ARM9Read32(requestAddr),
                cpu->NDS.ARM9Read32(activeAddr),
                state.ExtraTicksSeen,
                state.InputSequenceHash,
                cpu->NDS.ARM9Read16(scratchTickAddr),
                cpu->NDS.ARM9Read16(scratchKeysAddr),
                cpu->NDS.ARM9Read16(scratchKeysAddr + 2),
                cpu->NDS.ARM9Read32(historyEnabledAddr),
                cpu->NDS.ARM9Read32(historyIndexAddr),
                cpu->NDS.ARM9Read32(historyCountAddr));
            fflush(cfg.LogFile);
        }

        char filename[1024];
        snprintf(filename, sizeof(filename), "%s/rom-game-tick-probe-%s-frame%u-%s.bin",
            cfg.OutputDir.c_str(), cfg.Role.c_str(), frame, phase);
        if (FILE* file = fopen(filename, "wb"))
        {
            const u32 length = std::min(cpu->NDS.MainRAMMask + 1, 0x400000u);
            fwrite(cpu->NDS.MainRAM, 1, length, file);
            fclose(file);
        }
    };

    if (cfg.RenderlessAB)
    {
        constexpr std::array<u16, 7> player0Keys = {0x010, 0x011, 0x000, 0x020, 0x001, 0x810, 0x000};
        constexpr std::array<u16, 7> player1Keys = {0x020, 0x022, 0x000, 0x010, 0x002, 0x820, 0x000};
        auto hashHistoricalInput = [&](u16 tick, u16 keys0, u16 keys1)
        {
            for (const u16 value : {tick, keys0, keys1})
            {
                state.InputSequenceHash ^= value & 0xFF;
                state.InputSequenceHash *= 16777619u;
                state.InputSequenceHash ^= value >> 8;
                state.InputSequenceHash *= 16777619u;
            }
        };
        auto applyHistoricalInput = [&]()
        {
            // Deterministic diagnostic history. The JIT helper patch already
            // routes NSMB's per-player key reads through these scratch words.
            const u32 index = std::min(state.ExtraTicksSeen, cfg.ExtraTicks - 1);
            const u16 tick = static_cast<u16>((state.HistoricalBaseTick + index) & 0xFFFF);
            const u16 keys0 = player0Keys[index];
            const u16 keys1 = player1Keys[index];
            cpu->NDS.ARM9Write16(netPacketTickAddr, tick);
            cpu->NDS.ARM9Write16(scratchTickAddr, tick);
            cpu->NDS.ARM9Write16(scratchKeysAddr, keys0);
            cpu->NDS.ARM9Write16(scratchKeysAddr + 2, keys1);
            for (u32 player = 0; player < 2; player++)
            {
                const u32 packetAddr = scratchPacketsAddr + player * 0x40;
                const u16 keys = player == 0 ? keys0 : keys1;
                cpu->NDS.ARM9Write16(packetAddr, tick);
                cpu->NDS.ARM9Write16(packetAddr + 2, keys);
                cpu->NDS.ARM9Write32(packetAddr + 4, 0);
            }
            hashHistoricalInput(tick, keys0, keys1);
        };
        auto prepareGuestHistory = [&]()
        {
            cpu->NDS.ARM9Write32(historyIndexAddr, 0);
            cpu->NDS.ARM9Write32(historyCountAddr, cfg.ExtraTicks);
            cpu->NDS.ARM9Write32(historyTargetAddr, 0);
            cpu->NDS.ARM9Write32(historyStartFrameAddr, cpu->NDS.ARM9Read32(0x0208B668));
            for (u32 index = 0; index < cfg.ExtraTicks; index++)
            {
                const u16 tick = static_cast<u16>((state.HistoricalBaseTick + index) & 0xFFFF);
                const u32 entryAddr = historyAddr + index * historyEntrySize;
                cpu->NDS.ARM9Write16(entryAddr, tick);
                cpu->NDS.ARM9Write16(entryAddr + 2, player0Keys[index]);
                cpu->NDS.ARM9Write16(entryAddr + 4, player1Keys[index]);
                cpu->NDS.ARM9Write16(entryAddr + 6, 0);
                cpu->NDS.ARM9Write32(entryAddr + 8, 0);
                cpu->NDS.ARM9Write32(entryAddr + 12, 0);
                hashHistoricalInput(tick, player0Keys[index], player1Keys[index]);
            }
            cpu->NDS.ARM9Write32(historyEnabledAddr, 1);
        };

        if (instrAddr == loopStart && !state.TargetSeen && !state.Done &&
            cpu->NDS.NumFrames >= cfg.StartFrame && IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        {
            state.TargetSeen = true;
            state.ActualTargetFrame = cpu->NDS.NumFrames;
            state.RenderlessABTickPending = true;
            dump("before-renderless-ab-tick");
            state.HistoricalBaseTick = cpu->NDS.ARM9Read16(scratchTickAddr);
            if (cfg.HistoricalInputAB)
            {
                if (cfg.GuestOwnedHistoryAB)
                    prepareGuestHistory();
                else
                    applyHistoricalInput();
            }
            if (isTarget)
            {
                cpu->NDS.ARM9Write32(activeAddr, 1);
                cpu->NDS.ARM9Write32(requestAddr, cfg.ExtraTicks - 1);
            }
            return false;
        }
        if (instrAddr == loopStart && state.RenderlessABTickPending && cfg.HistoricalInputAB &&
            !cfg.GuestOwnedHistoryAB)
        {
            applyHistoricalInput();
            return false;
        }
        if (instrAddr == loopGateAfterCounter && state.RenderlessABTickPending)
        {
            state.ExtraTicksSeen++;
            if (state.ExtraTicksSeen < cfg.ExtraTicks)
                return false;

            dump(isTarget ? "after-renderless-tick" : "after-normal-control-tick");
            if (cfg.GuestOwnedHistoryAB)
                cpu->NDS.ARM9Write32(historyEnabledAddr, 0);
            cpu->NDS.ARM9Write32(requestAddr, 0);
            cpu->NDS.ARM9Write32(activeAddr, 0);
            state.RenderlessABTickPending = false;
            state.NeedNextNormalTick = true;
            return false;
        }
        if (instrAddr == loopStart && state.NeedNextNormalTick && !state.NextNormalTickEntered)
        {
            state.NextNormalTickEntered = true;
            dump("before-recovery-normal-tick");
            return false;
        }
        if (instrAddr == loopGateAfterCounter && state.NeedNextNormalTick && state.NextNormalTickEntered)
        {
            dump(isTarget ? "after-recovery-normal-tick" : "after-second-normal-control-tick");
            state.NeedNextNormalTick = false;
            state.Done = true;
        }
        return false;
    }

    if (instrAddr == loopStart)
    {
        if (state.NeedNextNormalTick && !state.NextNormalTickEntered)
        {
            state.NextNormalTickEntered = true;
            dump("next-before-normal-tick");
        }
        return false;
    }

    const u32 request = cpu->NDS.ARM9Read32(requestAddr);
    const bool active = cpu->NDS.ARM9Read32(activeAddr) != 0;
    if (isTarget && state.InExtraTicks && active)
    {
        state.ExtraTicksSeen++;
        if (request != 0)
            return false;

        dump("after-extra-tick");
        if (cfg.RestoreAfterExtra && !state.SavedMainRAM.empty())
        {
            const u32 length = std::min(cpu->NDS.MainRAMMask + 1, 0x400000u);
            if (state.SavedMainRAM.size() == length)
                memcpy(cpu->NDS.MainRAM, state.SavedMainRAM.data(), length);
        }
        cpu->NDS.ARM9Write32(requestAddr, 0);
        cpu->NDS.ARM9Write32(activeAddr, 0);
        cpu->NDS.ARM9Write32(magicAddr, magic);
        state.InExtraTicks = false;
        state.NeedNextNormalTick = true;
        return false;
    }

    if (state.NeedNextNormalTick && state.NextNormalTickEntered)
    {
        dump("next-after-normal-tick");
        state.Done = true;
        state.NeedNextNormalTick = false;
        return false;
    }

    if (state.TargetSeen || state.Done || cpu->NDS.NumFrames < cfg.StartFrame ||
        !IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        return false;

    state.TargetSeen = true;
    state.ActualTargetFrame = cpu->NDS.NumFrames;
    dump("target-after-normal-tick");
    if (!isTarget)
    {
        state.NeedNextNormalTick = true;
        return false;
    }

    if (cfg.RestoreAfterExtra)
    {
        const u32 length = std::min(cpu->NDS.MainRAMMask + 1, 0x400000u);
        state.SavedMainRAM.assign(cpu->NDS.MainRAM, cpu->NDS.MainRAM + length);
    }
    cpu->NDS.ARM9Write32(requestAddr, cfg.ExtraTicks);
    state.InExtraTicks = true;
    return false;
}

static bool HandleNSMLGameTickProbe(ARM* cpu, u32 instrAddr)
{
    constexpr u32 processListExecute = 0x0204D46C;
    constexpr u32 mainLoopAfterProcessLists = 0x02004EEC;
    constexpr u32 updateProcessList = 0x0208FB18;
    constexpr u32 renderProcessList = 0x0208FB38;
    constexpr u32 currentExecutingProcessList = 0x020852A8;
    constexpr u32 currentProcessNode = 0x0208FB08;
    constexpr u32 gameFrameCounter = 0x0208B668;

    if (instrAddr != processListExecute && instrAddr != mainLoopAfterProcessLists)
        return false;

    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        u32 StartFrame = 900;
        u32 GameFrameCounterStep = 2;
        bool RestoreAfterExtra = false;
        bool FreezeHardwareDuringExtra = false;
        u32 ExtraArm9CycleBudget = 1000000;
        std::string Role;
        std::string TargetRole = "host";
        std::string OutputDir;
        FILE* LogFile = nullptr;
    };
    struct State
    {
        bool TargetSeen = false;
        bool InExtraUpdate = false;
        bool NeedNextUpdate = false;
        bool NextUpdateEntered = false;
        bool NextRenderEntered = false;
        bool Done = false;
        u32 ActualTargetFrame = 0;
        u32 SavedRegs[5] {};
        u32 SavedLR = 0;
        u32 SavedCurrentExecutingProcessList = 0;
        u32 SavedCurrentProcessNode = 0;
        u32 ExtraGameFrameCounter = 0;
        std::vector<u8> SavedMainRAM;
        u64 SavedARM9Timestamp = 0;
        u64 SavedARM9Target = 0;
        u64 SavedARM7Timestamp = 0;
        u64 SavedARM7Target = 0;
        NDS::NSMLGameTickProbeSchedulerState SavedSchedulerState;
    };

    static Config cfg;
    static std::map<NDS*, State> states;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    if (!cfg.Checked)
    {
        cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_GAME_TICK_PROBE");
        cfg.StartFrame = NSMLEnvU32("MELONDS_NSML_GAME_TICK_PROBE_START_FRAME", 900);
        cfg.GameFrameCounterStep = NSMLEnvU32("MELONDS_NSML_GAME_TICK_PROBE_GAME_FRAME_STEP", 2);
        cfg.RestoreAfterExtra = NSMLEnvFlag("MELONDS_NSML_GAME_TICK_PROBE_RESTORE_AFTER_EXTRA");
        cfg.FreezeHardwareDuringExtra = NSMLEnvFlag("MELONDS_NSML_GAME_TICK_PROBE_FREEZE_HARDWARE");
        cfg.ExtraArm9CycleBudget = NSMLEnvU32("MELONDS_NSML_GAME_TICK_PROBE_ARM9_CYCLE_BUDGET", 1000000);
        if (const char* role = getenv("MELONDS_NSML_ROLE"))
            cfg.Role = role;
        if (cfg.Role.empty())
            cfg.Role = "local";
        if (const char* targetRole = getenv("MELONDS_NSML_GAME_TICK_PROBE_TARGET_ROLE"))
            cfg.TargetRole = targetRole;
        if (const char* outputDir = getenv("MELONDS_NSML_GAME_TICK_PROBE_DIR"))
            cfg.OutputDir = outputDir;
        if (cfg.Enabled && !cfg.OutputDir.empty())
        {
            const std::string logPath = cfg.OutputDir + "/game-tick-probe-" + cfg.Role + ".csv";
            cfg.LogFile = fopen(logPath.c_str(), "w");
            if (cfg.LogFile)
            {
                fprintf(cfg.LogFile,
                    "role,frame,phase,main_ram_hash,game_frame_counter,arm9_timestamp,arm7_timestamp,current_process_list,current_process_node\n");
                fflush(cfg.LogFile);
            }
        }
        cfg.Checked = true;
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0 || !cpu->NDS.MainRAM || cfg.OutputDir.empty())
        return false;

    State& state = states[&cpu->NDS];
    const bool isTarget = cfg.Role == cfg.TargetRole;

    auto dump = [&](const char* phase)
    {
        const u32 frame = cpu->NDS.NumFrames;
        const u64 mainRAMHash = HashNSMLGameTickProbeMainRAM(cpu->NDS);
        if (cfg.LogFile)
        {
            fprintf(cfg.LogFile, "%s,%u,%s,%016llX,%u,%llu,%llu,%08X,%08X\n",
                cfg.Role.c_str(),
                frame,
                phase,
                static_cast<unsigned long long>(mainRAMHash),
                cpu->NDS.ARM9Read32(gameFrameCounter),
                static_cast<unsigned long long>(cpu->NDS.ARM9Timestamp),
                static_cast<unsigned long long>(cpu->NDS.ARM7Timestamp),
                cpu->NDS.ARM9Read32(currentExecutingProcessList),
                cpu->NDS.ARM9Read32(currentProcessNode));
            fflush(cfg.LogFile);
        }

        char filename[1024];
        snprintf(filename, sizeof(filename), "%s/game-tick-probe-%s-frame%u-%s.bin",
            cfg.OutputDir.c_str(), cfg.Role.c_str(), frame, phase);
        if (FILE* file = fopen(filename, "wb"))
        {
            const u32 length = std::min(cpu->NDS.MainRAMMask + 1, 0x400000u);
            fwrite(cpu->NDS.MainRAM, 1, length, file);
            fclose(file);
        }
    };

    if (instrAddr == processListExecute && cpu->R[0] == renderProcessList &&
        state.NeedNextUpdate && state.NextUpdateEntered && !state.NextRenderEntered &&
        cpu->NDS.NumFrames > state.ActualTargetFrame)
    {
        state.NextRenderEntered = true;
        dump(isTarget && !cfg.RestoreAfterExtra
            ? "next-after-skip-before-render"
            : "next-after-update-before-render");
    }

    if (instrAddr == processListExecute && cpu->R[0] == updateProcessList &&
        state.NeedNextUpdate && !state.NextUpdateEntered &&
        cpu->NDS.NumFrames > state.ActualTargetFrame)
    {
        state.NextUpdateEntered = true;
        if (isTarget && !cfg.RestoreAfterExtra)
            cpu->NDS.ARM9Write32(gameFrameCounter, state.ExtraGameFrameCounter);
        dump(isTarget && !cfg.RestoreAfterExtra ? "next-before-skip" : "next-before-update");
        if (isTarget && !cfg.RestoreAfterExtra)
        {
            // The target already executed this gameplay tick inside the prior
            // display frame. Skip exactly one normal update-list pass so it
            // can be compared with the control peer after the next frame.
            cpu->JumpTo(cpu->R[14]);
            return true;
        }
    }

    if (instrAddr != mainLoopAfterProcessLists)
        return false;

    if (state.InExtraUpdate)
    {
        dump("after-extra-update");
        if (cfg.RestoreAfterExtra && !state.SavedMainRAM.empty())
        {
            const u32 length = std::min(cpu->NDS.MainRAMMask + 1, 0x400000u);
            if (state.SavedMainRAM.size() == length)
                memcpy(cpu->NDS.MainRAM, state.SavedMainRAM.data(), length);
        }
        if (cfg.FreezeHardwareDuringExtra)
        {
            cpu->NDS.NSMLGameTickProbeFreezeScheduler = false;
            cpu->NDS.RestoreNSMLGameTickProbeSchedulerState(state.SavedSchedulerState);
            cpu->NDS.ARM9Timestamp = state.SavedARM9Timestamp;
            cpu->NDS.ARM9Target = state.SavedARM9Target;
            cpu->NDS.ARM7Timestamp = state.SavedARM7Timestamp;
            cpu->NDS.ARM7Target = state.SavedARM7Target;
        }
        for (int i = 0; i < 4; i++)
            cpu->R[i] = state.SavedRegs[i];
        cpu->R[12] = state.SavedRegs[4];
        cpu->R[14] = state.SavedLR;
        cpu->NDS.ARM9Write32(currentExecutingProcessList, state.SavedCurrentExecutingProcessList);
        cpu->NDS.ARM9Write32(currentProcessNode, state.SavedCurrentProcessNode);
        state.InExtraUpdate = false;
        state.NeedNextUpdate = true;
        return false;
    }

    if (state.NeedNextUpdate && state.NextUpdateEntered &&
        cpu->NDS.NumFrames > state.ActualTargetFrame)
    {
        dump(isTarget && !cfg.RestoreAfterExtra ? "next-after-skip" : "next-after-update");
        state.Done = true;
        state.NeedNextUpdate = false;
        return false;
    }

    if (state.TargetSeen || state.Done || cpu->NDS.NumFrames < cfg.StartFrame ||
        !IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        return false;

    state.TargetSeen = true;
    state.ActualTargetFrame = cpu->NDS.NumFrames;
    dump("target-after-normal-update");
    state.NeedNextUpdate = true;

    if (!isTarget)
        return false;

    for (int i = 0; i < 4; i++)
        state.SavedRegs[i] = cpu->R[i];
    state.SavedRegs[4] = cpu->R[12];
    state.SavedLR = cpu->R[14];
    state.SavedCurrentExecutingProcessList = cpu->NDS.ARM9Read32(currentExecutingProcessList);
    state.SavedCurrentProcessNode = cpu->NDS.ARM9Read32(currentProcessNode);
    if (cfg.RestoreAfterExtra)
    {
        const u32 length = std::min(cpu->NDS.MainRAMMask + 1, 0x400000u);
        state.SavedMainRAM.assign(cpu->NDS.MainRAM, cpu->NDS.MainRAM + length);
    }
    state.ExtraGameFrameCounter = cpu->NDS.ARM9Read32(gameFrameCounter) + cfg.GameFrameCounterStep;
    cpu->NDS.ARM9Write32(gameFrameCounter, state.ExtraGameFrameCounter);
    if (cfg.FreezeHardwareDuringExtra)
    {
        state.SavedARM9Timestamp = cpu->NDS.ARM9Timestamp;
        state.SavedARM9Target = cpu->NDS.ARM9Target;
        state.SavedARM7Timestamp = cpu->NDS.ARM7Timestamp;
        state.SavedARM7Target = cpu->NDS.ARM7Target;
        cpu->NDS.CaptureNSMLGameTickProbeSchedulerState(state.SavedSchedulerState);
        cpu->NDS.NSMLGameTickProbeFreezeScheduler = true;
        cpu->NDS.ARM9Target = cpu->NDS.ARM9Timestamp + cfg.ExtraArm9CycleBudget;
    }
    state.InExtraUpdate = true;
    cpu->R[0] = updateProcessList;
    cpu->R[14] = mainLoopAfterProcessLists;
    cpu->JumpTo(processListExecute);
    return true;
}

static u32 NSMLFindObjectBaseByID(NDS& nds, u16 objectID)
{
    if (!nds.MainRAM)
        return 0;

    for (u32 off = 0x080000; off + 0x80 <= nds.MainRAMMask + 1; off += 4)
    {
        const u32 base = 0x02000000 + off;
        const u32 vtable = nds.ARM9Read32(base);
        const u16 candidateID = nds.ARM9Read16(base + 0x0C);
        const u16 stateType = nds.ARM9Read16(base + 0x0E);
        const u32 flags = nds.ARM9Read32(base + 0x10);
        if (candidateID != objectID || stateType == 0 || stateType > 2)
            continue;
        if (vtable < 0x02000000 || vtable >= 0x02400000)
            continue;
        if ((flags & 0xFFFF0000u) == 0)
            continue;
        return base;
    }

    return 0;
}

static void HandleNSMLNetReadyHotPatch(ARM* cpu, u32 instrAddr)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const bool mvlExternalSettings = NSMLEnvFlag("MELONDS_NSML_MVL_STAGE")
            || NSMLEnvFlag("MELONDS_NSML_MVL_SCENE_SETTINGS");
        enabled = mvlExternalSettings ? 1 : 0;
        printf("NSMB PacketBridge: net hotpatch config enabled=%d\n", enabled);
        fflush(stdout);
    }
    if (!enabled || !cpu || cpu->Num != 0)
        return;
    if (instrAddr == 0x020068A8
        && (IsNSMLMarioVsLuigiPacketContext(cpu->NDS)
            || (NSMLPacketBridgeEnabled() && cpu->R[0] == 0x0F && cpu->R[2] == 9)))
    {
        if (cpu->R[0] == 0x0F && cpu->R[2] == 9)
        {
            const u32 localPlayer = NSMLPacketBridgeLocalPlayer();
            const u32 sp = cpu->R[13];
            cpu->R[1] = 1; // vs
            cpu->R[3] = NSMLMvlStage();
            cpu->DataWrite32(sp + 0x00, 0); // act
            cpu->DataWrite32(sp + 0x04, localPlayer); // playerID
            cpu->DataWrite32(sp + 0x08, 3); // playerMask
            cpu->DataWrite32(sp + 0x0C, 0); // character1: Mario
            cpu->DataWrite32(sp + 0x10, 1); // character2: Luigi
            cpu->DataWrite32(sp + 0x14, 0); // powerup
            cpu->DataWrite32(sp + 0x18, 0xFF); // entrance
            cpu->DataWrite32(sp + 0x1C, 1); // flag
            cpu->DataWrite32(sp + 0x20, 1); // unused1, matches normal MvL load path
            cpu->DataWrite32(sp + 0x24, 0xFF); // controlOptions
            cpu->DataWrite32(sp + 0x28, 0); // unused2
            cpu->DataWrite32(sp + 0x2C, 0); // challengeMode
            cpu->DataWrite32(sp + 0x30, 0xFFFFFFFFu); // rngSeed: use network/random state
            cpu->NDS.ARM9Write32(0x02088858, 0x00400150);
            static int logCount = 0;
            if (logCount < 8)
            {
                printf("NSMB PacketBridge: force Game::loadLevel MvL args frame=%u playerID=%u stage=%u\n",
                    cpu->NDS.NumFrames,
                    localPlayer,
                    cpu->R[3]);
                fflush(stdout);
                logCount++;
            }
        }
        return;
    }
    if (instrAddr == 0x021514E4 && IsNSMLMarioVsLuigiPacketContext(cpu->NDS))
    {
        const u32 vsConnectBase = NSMLFindObjectBaseByID(cpu->NDS, 0x0006);
        if (vsConnectBase != 0
            && cpu->NDS.ARM9Read32(vsConnectBase + 0x120) == 0x02151E94
            && cpu->NDS.ARM9Read32(vsConnectBase + 0x144) == 6
            && (cpu->NDS.ARM9Read32(vsConnectBase + 0x154) & 0x00030000) == 0x00030000)
        {
            cpu->R[0] = 1;
            static int logCount = 0;
            if (logCount < 8)
            {
                printf("NSMB PacketBridge: force load-game net-ready result frame=%u vsConnect=%08X\n",
                    cpu->NDS.NumFrames,
                    vsConnectBase);
                fflush(stdout);
                logCount++;
            }
        }
        return;
    }
    if (instrAddr != 0x02151E94) // VSConnect::updateLoadGameSM()
        return;
    if (!IsNSMLMarioVsLuigiPacketContext(cpu->NDS))
        return;

    cpu->NDS.ARM9Write32(0x02088A84, 0x00000003); // Net::packetFreeBytesRecvBitmap
    cpu->NDS.ARM9Write32(0x02088A88, 0x00000003);
}

static bool HandleNSMLNetDisconnectBypass(ARM* cpu, u32 instrAddr)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_BYPASS_NET_DISCONNECT") ? 1 : 0;
    if (!enabled || !cpu || cpu->Num != 0)
        return false;
    if (!NSMLPacketBridgeEnabled() || !IsNSMLMarioVsLuigiPacketContext(cpu->NDS))
        return false;

    static u32 startFrame = 0xFFFFFFFF;
    if (startFrame == 0xFFFFFFFF)
    {
        if (const char* value = getenv("MELONDS_NSML_PACKET_BRIDGE_BYPASS_NET_DISCONNECT_START_FRAME"))
            startFrame = static_cast<u32>(strtoul(value, nullptr, 0));
        else
            startFrame = 0;
    }
    if (cpu->NDS.NumFrames < startFrame)
        return false;

    static int mode = -1;
    if (mode < 0)
    {
        mode = 0;
        if (const char* value = getenv("MELONDS_NSML_PACKET_BRIDGE_BYPASS_NET_DISCONNECT_MODE"))
        {
            if (!strcmp(value, "force-active"))
                mode = 1;
        }
    }

    if (mode == 0)
    {
        if (instrAddr != 0x02010174)
            return false;

        static int skipLogCount = 0;
        if (skipLogCount < 16)
        {
            const u32 flags = cpu->NDS.ARM9Read16(0x02088808);
            printf("NSMB PacketBridge: skip Net disconnect branch at %08X frame=%u lr=%08X flags=0x%04X\n",
                instrAddr,
                cpu->NDS.NumFrames,
                cpu->R[14],
                flags);
            skipLogCount++;
        }

        cpu->JumpTo(0x0201019C);
        return true;
    }

    if (instrAddr != 0x02010130)
        return false;

    const u32 flags = cpu->NDS.ARM9Read16(0x02088808);
    if (flags != 0x0002)
        return false;

    static int logCount = 0;
    if (logCount < 16)
    {
        printf("NSMB PacketBridge: force Net active flags at %08X frame=%u lr=%08X\n",
            instrAddr,
            cpu->NDS.NumFrames,
            cpu->R[14]);
        printf("NSMB PacketBridge: Net flags old=0x%04X new=0x%04X\n",
            flags,
            0x0004);
        logCount++;
    }

    cpu->NDS.ARM9Write16(0x02088808, 0x0004);
    return false;
}

static u32 NSMLPacketBridgeEnvFrame(const char* name, u32 fallback)
{
    if (const char* value = getenv(name))
        return static_cast<u32>(strtoul(value, nullptr, 0));
    return fallback;
}

static u32 NSMLPacketBridgeCanonicalTick(NDS& nds)
{
    // The frontend writes this field from the current match's logical frame.
    // Recomputing it from NDS::NumFrames here would reintroduce each peer's
    // local restore-frame offset after a rematch.
    return nds.ARM9Read16(0x020888E0);
}

static bool HandleNSMLTransferPacketBypass(ARM* cpu, u32 instrAddr)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_FORCE_TRANSFER_RESULT") ? 1 : 0;
    if (!enabled || !cpu || cpu->Num != 0 || instrAddr != 0x0200F98C)
        return false;
    if (NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_FORCE_TRANSFER_CLIENT_ONLY"))
    {
        const char* role = getenv("MELONDS_NSML_ROLE");
        if (!role || strcmp(role, "client") != 0)
            return false;
    }
    if (!IsNSMLMarioVsLuigiPacketContext(cpu->NDS))
        return false;

    static u32 startFrame = 0xFFFFFFFF;
    if (startFrame == 0xFFFFFFFF)
        startFrame = NSMLPacketBridgeEnvFrame("MELONDS_NSML_PACKET_BRIDGE_FORCE_TRANSFER_START_FRAME", 0);
    if (cpu->NDS.NumFrames < startFrame)
        return false;

    static u32 result = 0xFFFFFFFF;
    if (result == 0xFFFFFFFF)
        result = NSMLPacketBridgeEnvFrame("MELONDS_NSML_PACKET_BRIDGE_FORCE_TRANSFER_RESULT_VALUE", 8);

    static int logCount = 0;
    if (logCount < 16)
    {
        printf("NSMB PacketBridge: force transferPacket result at %08X frame=%u lr=%08X result=0x%08X\n",
            instrAddr,
            cpu->NDS.NumFrames,
            cpu->R[14],
            result);
        logCount++;
    }

    cpu->R[0] = result;
    cpu->JumpTo(cpu->R[14]);
    return true;
}

static void BuildNSMLMarioVsLuigiPacket(NDS& nds, std::array<u8, 52>& packet, u32& tick, u32& keys)
{
    packet.fill(0);
    tick = NSMLPacketBridgeCanonicalTick(nds);
    keys = nds.ARM9Read16(0x020888E2);
    packet[0] = static_cast<u8>(tick & 0xFF);
    packet[1] = static_cast<u8>((tick >> 8) & 0xFF);
    packet[2] = static_cast<u8>(keys & 0xFF);
    packet[3] = static_cast<u8>((keys >> 8) & 0xFF);
    packet[4] = nds.ARM9Read8(0x020888E4);
    packet[5] = nds.ARM9Read8(0x020888E5);
    packet[6] = nds.ARM9Read8(0x020888E6);
    packet[7] = nds.ARM9Read8(0x020888E7);
    for (u32 i = 0; i < 44; i++)
        packet[8 + i] = nds.ARM9Read8(0x020888E8 + i);
    // NSMB's send path copies the packet-bit byte at 0x02088A4C into packet
    // offset 0x29. Build the WAN packet from the same source so ready-bit
    // waits such as StageScene::onCreate can observe peer progress.
    packet[0x29] = nds.ARM9Read8(0x02088A4C);
}

bool NSML_TakeMarioVsLuigiLocalPacket(NDS* nds, u8 outPacket[52], u32* outTick, u32* outKeys)
{
    if (!nds || !outPacket)
        return false;

    std::lock_guard<std::mutex> lock(NSMLPacketBridgeMutex);
    auto it = NSMLLocalPackets.find(nds);
    if (it == NSMLLocalPackets.end() || !it->second.Available)
        return false;

    memcpy(outPacket, it->second.Packet.data(), it->second.Packet.size());
    if (outTick) *outTick = it->second.Tick;
    if (outKeys) *outKeys = it->second.Keys;
    it->second.Available = false;
    return true;
}

bool NSML_BuildMarioVsLuigiLocalPacket(NDS* nds, u8 outPacket[52], u32* outTick, u32* outKeys)
{
    if (!nds || !outPacket || !IsNSMLMarioVsLuigiPacketContext(*nds))
        return false;

    std::array<u8, 52> packet {};
    u32 tick = 0;
    u32 keys = 0;
    BuildNSMLMarioVsLuigiPacket(*nds, packet, tick, keys);

    memcpy(outPacket, packet.data(), packet.size());
    if (outTick) *outTick = tick;
    if (outKeys) *outKeys = keys;
    return true;
}

void NSML_PushMarioVsLuigiRemotePacket(NDS* nds, u32 player, const u8 packet[52])
{
    if (!nds || !packet || player > 1)
        return;

    const u32 tick = packet[0] | (packet[1] << 8);
    std::lock_guard<std::mutex> lock(NSMLPacketBridgeMutex);
    auto& packets = NSMLLiveReplayPackets[nds];
    auto& entry = packets[tick];
    memcpy(entry.Packet[player].data(), packet, entry.Packet[player].size());
    entry.Valid[player] = true;

    if (packets.size() > 512)
    {
        const u32 keepFrom = tick > 256 ? tick - 256 : 0;
        packets.erase(packets.begin(), packets.lower_bound(keepFrom));
    }
}

bool NSML_HasMarioVsLuigiRemotePacket(NDS* nds, u32 player, u32 tick)
{
    if (!nds || player > 1)
        return false;

    tick &= 0xFFFF;
    std::lock_guard<std::mutex> lock(NSMLPacketBridgeMutex);
    auto ndsIt = NSMLLiveReplayPackets.find(nds);
    if (ndsIt == NSMLLiveReplayPackets.end())
        return false;

    auto packetIt = ndsIt->second.find(tick);
    return packetIt != ndsIt->second.end() && packetIt->second.Valid[player];
}

static bool NSMLLiveReplayLatestBeforeFallbackEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = NSMLEnvFlag("MELONDS_NSML_PACKET_REPLAY_LIVE_FALLBACK_LATEST_BEFORE") ? 1 : 0;

    return enabled != 0;
}

static bool NSMLFindLiveReplayPacketLocked(
    NDS* nds,
    u32 player,
    u32 tick,
    u32 fallbackWindow,
    std::array<u8, 52>& outPacket)
{
    if (!nds || player > 1)
        return false;

    auto ndsIt = NSMLLiveReplayPackets.find(nds);
    if (ndsIt == NSMLLiveReplayPackets.end())
        return false;

    auto liveIt = ndsIt->second.find(tick);
    if (liveIt != ndsIt->second.end() && liveIt->second.Valid[player])
    {
        outPacket = liveIt->second.Packet[player];
        return true;
    }

    if (fallbackWindow == 0)
    {
        if (!NSMLLiveReplayLatestBeforeFallbackEnabled())
            return false;

        u32 bestAge = 0x8000;
        const NSMLPacketReplayEntry* bestEntry = nullptr;
        for (const auto& [packetTick, entry] : ndsIt->second)
        {
            if (!entry.Valid[player])
                continue;

            const u32 age = (tick - packetTick) & 0xFFFF;
            if (age == 0 || age > 0x7FFF || age >= bestAge)
                continue;

            bestAge = age;
            bestEntry = &entry;
        }

        if (!bestEntry)
            return false;

        outPacket = bestEntry->Packet[player];
        return true;
    }

    if (NSMLLiveReplayLatestBeforeFallbackEnabled())
    {
        u32 bestAge = 0x8000;
        const NSMLPacketReplayEntry* bestEntry = nullptr;
        for (const auto& [packetTick, entry] : ndsIt->second)
        {
            if (!entry.Valid[player])
                continue;

            const u32 age = (tick - packetTick) & 0xFFFF;
            if (age == 0 || age > 0x7FFF || age >= bestAge)
                continue;
            if (age > fallbackWindow)
                continue;

            bestAge = age;
            bestEntry = &entry;
        }

        if (bestEntry)
        {
            outPacket = bestEntry->Packet[player];
            return true;
        }
    }

    const u32 window = std::min<u32>(fallbackWindow, 4096);
    for (u32 age = 1; age <= window; age++)
    {
        const u32 fallbackTick = (tick - age) & 0xFFFF;
        auto fallbackIt = ndsIt->second.find(fallbackTick);
        if (fallbackIt != ndsIt->second.end() && fallbackIt->second.Valid[player])
        {
            outPacket = fallbackIt->second.Packet[player];
            return true;
        }

    }

    return false;
}

static bool NSMLSelectBridgePacketForPlayer(
    NDS& nds,
    u32 player,
    u32 tick,
    std::array<u8, 52>& packet)
{
    if (player > 1 || !NSMLPacketBridgeEnabled() || !IsNSMLMarioVsLuigiPacketContext(nds))
        return false;

    static int fallbackWindow = -1;
    static int normalizeTick = -1;
    if (fallbackWindow < 0)
    {
        if (const char* value = getenv("MELONDS_NSML_PACKET_REPLAY_LIVE_FALLBACK_WINDOW"))
            fallbackWindow = std::max(0, atoi(value));
        else
            fallbackWindow = 0;
    }
    if (normalizeTick < 0)
        normalizeTick = NSMLEnvFlag("MELONDS_NSML_PACKET_REPLAY_RETURN_LOOKUP_TICK") ? 1 : 0;

    bool found = false;
    {
        std::lock_guard<std::mutex> lock(NSMLPacketBridgeMutex);
        found = NSMLFindLiveReplayPacketLocked(
            &nds,
            player,
            tick,
            static_cast<u32>(fallbackWindow),
            packet);
    }

    if (found && normalizeTick)
    {
        packet[0] = static_cast<u8>(tick & 0xFF);
        packet[1] = static_cast<u8>((tick >> 8) & 0xFF);
    }

    if (!found && player == NSMLPacketBridgeLocalPlayer())
    {
        u32 ignoredTick = 0;
        u32 ignoredKeys = 0;
        BuildNSMLMarioVsLuigiPacket(nds, packet, ignoredTick, ignoredKeys);
        found = true;
    }

    return found;
}

static u32 NSMLWriteBridgePacketScratch(NDS& nds, u32 player, const std::array<u8, 52>& packet)
{
    constexpr u32 scratchBase = 0x023C1000;
    const u32 addr = scratchBase + (player & 1) * 0x40;
    for (u32 i = 0; i < packet.size(); i++)
        nds.ARM9Write8(addr + i, packet[i]);
    return addr;
}

static bool HandleNSMLLowerMPBridge(ARM* cpu, u32 instrAddr)
{
    if (!cpu || cpu->Num != 0 || !NSMLPacketBridgeEnabled())
        return false;
    if (!IsNSMLMarioVsLuigiPacketContext(cpu->NDS))
        return false;

    static int traceLower = -1;
    if (traceLower < 0)
        traceLower = NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_TRACE") ? 1 : 0;

    const bool oldStatusProbe = instrAddr == 0x0204619C;
    const bool usUpdateSharedData = instrAddr == 0x02046ECC;
    if (oldStatusProbe || usUpdateSharedData)
    {
        // A2DJ used a lower-MP status probe where false kept the packet-copy
        // path active. US A2DE calls Wifi::updateSharedData(), where true
        // enters the packet-copy path in Net::Core::transferPacket().
        const int statusResult = usUpdateSharedData ? 1 : 0;
        static u32 traceCount = 0;
        if (traceLower && (traceCount < 24 || (traceCount % 300) == 0))
            printf("NSMB PacketBridge lower: %s %08X frame=%u tick=0x%04X lr=%08X -> %d\n",
                usUpdateSharedData ? "updateSharedData" : "statusProbe",
                instrAddr,
                cpu->NDS.NumFrames,
                NSMLPacketBridgeCanonicalTick(cpu->NDS) & 0xFFFF,
                cpu->R[14],
                statusResult);
        traceCount++;
        cpu->R[0] = static_cast<u32>(statusResult);
        cpu->JumpTo(cpu->R[14]);
        return true;
    }

    const bool oldHasPacket = instrAddr == 0x0204622C;
    const bool oldGetPacket = instrAddr == 0x02046480;
    const bool usHasPacket = instrAddr == 0x02046C44;
    const bool usGetPacket = instrAddr == 0x02046E98;
    if (!oldHasPacket && !oldGetPacket && !usHasPacket && !usGetPacket)
        return false;

    const u32 player = cpu->R[0] & 0xFF;
    std::array<u8, 52> packet {};
    const bool hasPacket = NSMLSelectBridgePacketForPlayer(cpu->NDS, player, NSMLPacketBridgeCanonicalTick(cpu->NDS), packet);
    const u32 tick = NSMLPacketBridgeCanonicalTick(cpu->NDS) & 0xFFFF;

    if (oldHasPacket || usHasPacket)
    {
        static u32 traceCount[2] {};
        if (traceLower && player < 2 && (traceCount[player] < 32 || (traceCount[player] % 300) == 0))
            printf("NSMB PacketBridge lower: hasPacket %08X player=%u frame=%u tick=0x%04X action=0x%02X pktTick=0x%04X -> %u\n",
                instrAddr,
                player,
                cpu->NDS.NumFrames,
                tick,
                hasPacket ? packet[4] : 0xFF,
                hasPacket ? static_cast<u32>(packet[0] | (packet[1] << 8)) : 0xFFFF,
                hasPacket ? 1 : 0);
        if (player < 2)
            traceCount[player]++;
        cpu->R[0] = hasPacket ? 1 : 0;
        cpu->JumpTo(cpu->R[14]);
        return true;
    }

    const u32 packetPtr = hasPacket ? NSMLWriteBridgePacketScratch(cpu->NDS, player, packet) : 0;
    static u32 traceCount[2] {};
    if (traceLower && player < 2 && (traceCount[player] < 32 || (traceCount[player] % 300) == 0))
        printf("NSMB PacketBridge lower: getPacket %08X player=%u frame=%u tick=0x%04X action=0x%02X pktTick=0x%04X -> %08X\n",
            instrAddr,
            player,
            cpu->NDS.NumFrames,
            tick,
            hasPacket ? packet[4] : 0xFF,
            hasPacket ? static_cast<u32>(packet[0] | (packet[1] << 8)) : 0xFFFF,
            packetPtr);
    if (player < 2)
        traceCount[player]++;
    cpu->R[0] = packetPtr;
    cpu->JumpTo(cpu->R[14]);
    return true;
}

static void NSMLWriteLiveReplayPacketsToLocalMPSlots(
    NDS& nds,
    u32 tick,
    u32 fallbackWindow,
    bool normalizePacketTick)
{
    std::array<std::array<u8, 52>, 2> packets {};
    bool valid[2] {};
    {
        std::lock_guard<std::mutex> lock(NSMLPacketBridgeMutex);
        for (u32 player = 0; player < 2; player++)
            valid[player] = NSMLFindLiveReplayPacketLocked(&nds, player, tick, fallbackWindow, packets[player]);
    }

    for (u32 player = 0; player < 2; player++)
    {
        if (!valid[player])
            continue;

        if (normalizePacketTick)
        {
            packets[player][0] = static_cast<u8>(tick & 0xFF);
            packets[player][1] = static_cast<u8>((tick >> 8) & 0xFF);
        }

        nds.ARM9Write32(0x0208AE50 + player * 4, 1);
        const u32 packetAddr = 0x0208B040 + player * 0x3E;
        for (u32 i = 0; i < packets[player].size(); i++)
            nds.ARM9Write8(packetAddr + i, packets[player][i]);
    }
}

static void NSMLWriteReplayEntryToLocalMPSlots(
    NDS& nds,
    const NSMLPacketReplayEntry& entry,
    u32 tick,
    bool normalizePacketTick)
{
    for (u32 player = 0; player < 2; player++)
    {
        if (!entry.Valid[player])
            continue;

        std::array<u8, 52> packet = entry.Packet[player];
        if (normalizePacketTick)
        {
            packet[0] = static_cast<u8>(tick & 0xFF);
            packet[1] = static_cast<u8>((tick >> 8) & 0xFF);
        }

        nds.ARM9Write32(0x0208AE50 + player * 4, 1);
        const u32 packetAddr = 0x0208B040 + player * 0x3E;
        for (u32 i = 0; i < packet.size(); i++)
            nds.ARM9Write8(packetAddr + i, packet[i]);
    }
}

static bool NSMLFindReplayEntryForTick(
    const std::map<u32, NSMLPacketReplayEntry>& packets,
    u32 tick,
    u32 fallbackWindow,
    const NSMLPacketReplayEntry** outEntry,
    u32* outTick)
{
    auto it = packets.find(tick);
    if (it != packets.end())
    {
        if (outEntry) *outEntry = &it->second;
        if (outTick) *outTick = tick;
        return true;
    }

    const u32 window = std::min<u32>(fallbackWindow, 4096);
    for (u32 age = 1; age <= window; age++)
    {
        const u32 fallbackTick = (tick - age) & 0xFFFF;
        it = packets.find(fallbackTick);
        if (it != packets.end())
        {
            if (outEntry) *outEntry = &it->second;
            if (outTick) *outTick = fallbackTick;
            return true;
        }
    }

    return false;
}

void NSML_RefreshMarioVsLuigiPacketSlots(NDS* nds)
{
    if (!nds || !NSMLPacketBridgeEnabled() || !IsNSMLMarioVsLuigiPacketContext(*nds))
        return;

    static int fallbackWindow = -1;
    static int normalizeTick = -1;
    static int suppressDisconnect = -1;
    if (fallbackWindow < 0)
    {
        if (const char* value = getenv("MELONDS_NSML_PACKET_REPLAY_LIVE_FALLBACK_WINDOW"))
            fallbackWindow = std::max(0, atoi(value));
        else
            fallbackWindow = 0;
    }
    if (normalizeTick < 0)
        normalizeTick = NSMLEnvFlag("MELONDS_NSML_PACKET_REPLAY_RETURN_LOOKUP_TICK") ? 1 : 0;
    if (suppressDisconnect < 0)
        suppressDisconnect = NSMLEnvFlag("MELONDS_NSML_PACKET_BRIDGE_SUPPRESS_DISCONNECT") ? 1 : 0;

    const u32 tick = nds->ARM9Read16(0x020888E0);

    // The old local-MP slot mirror uses 0x0208B040 + player * 0x3E. In US
    // direct MvL gameplay the player1 range overlaps Entrance globals
    // (spawnEntranceID/transitionFlags/spawnEntrance at 0x0208B094+), so writing
    // it corrupts entrance transition state and eventually crashes
    // Player::viewTransitState. During gameplay, prefer the lower packet API
    // hooks that return scratch packets from 0x023C1000.
    if (!IsNSMLMarioVsLuigiGameplay(*nds))
    {
        NSMLWriteLiveReplayPacketsToLocalMPSlots(
            *nds,
            tick,
            static_cast<u32>(fallbackWindow),
            normalizeTick != 0);
    }

    if (suppressDisconnect)
    {
        const u16 flags = nds->ARM9Read16(0x0208883C);
        nds->ARM9Write16(0x0208883C, flags & static_cast<u16>(~0xC390));
        if (nds->ARM9Read8(0x02088804) == 9)
            nds->ARM9Write8(0x02088804, 6);
    }

}

static std::vector<std::string> SplitNSMLCsvLine(const std::string& line)
{
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ','))
        out.push_back(item);
    return out;
}

static int FindNSMLCsvColumn(const std::vector<std::string>& header, const char* name)
{
    for (int i = 0; i < static_cast<int>(header.size()); i++)
    {
        if (header[i] == name)
            return i;
    }
    return -1;
}

static bool ParseNSMLHexPacket(const std::string& hex, std::array<u8, 52>& out)
{
    if (hex.size() < out.size() * 2)
        return false;

    for (size_t i = 0; i < out.size(); i++)
    {
        char tmp[3] { hex[i * 2], hex[i * 2 + 1], '\0' };
        out[i] = static_cast<u8>(strtoul(tmp, nullptr, 16));
    }
    return true;
}

static bool HandleNSMLPacketReplay(ARM* cpu, u32 instrAddr)
{
    struct PacketReplayConfig
    {
        bool Checked = false;
        bool Enabled = false;
        u32 LookupTickDelay = 0;
        u32 LiveFallbackWindow = 0;
        bool ReturnLookupTick = false;
        bool ReplayOpEnabled[4] { true, true, true, true };
        FILE* LogFile = nullptr;
        std::map<u32, NSMLPacketReplayEntry> Packets;
    };

    static PacketReplayConfig cfg;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            if (const char* lookupTickDelay = getenv("MELONDS_NSML_PACKET_REPLAY_LOOKUP_TICK_DELAY"))
                cfg.LookupTickDelay = static_cast<u32>(strtoul(lookupTickDelay, nullptr, 0));
            if (const char* liveFallbackWindow = getenv("MELONDS_NSML_PACKET_REPLAY_LIVE_FALLBACK_WINDOW"))
                cfg.LiveFallbackWindow = static_cast<u32>(strtoul(liveFallbackWindow, nullptr, 0));
            cfg.ReturnLookupTick = getenv("MELONDS_NSML_PACKET_REPLAY_RETURN_LOOKUP_TICK") != nullptr;
            if (const char* replayOps = getenv("MELONDS_NSML_PACKET_REPLAY_OPS"))
            {
                cfg.ReplayOpEnabled[0] = false;
                cfg.ReplayOpEnabled[1] = false;
                cfg.ReplayOpEnabled[2] = false;
                cfg.ReplayOpEnabled[3] = false;
                char buf[64];
                strncpy(buf, replayOps, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                for (char* tok = strtok(buf, ", \t\r\n"); tok; tok = strtok(nullptr, ", \t\r\n"))
                {
                    if (!_stricmp(tok, "keys")) cfg.ReplayOpEnabled[0] = true;
                    else if (!_stricmp(tok, "byte")) cfg.ReplayOpEnabled[1] = true;
                    else if (!_stricmp(tok, "tick")) cfg.ReplayOpEnabled[2] = true;
                    else if (!_stricmp(tok, "action")) cfg.ReplayOpEnabled[3] = true;
                }
            }
            const char* path = getenv("MELONDS_NSML_PACKET_REPLAY_FILE");
            const bool bridgeEnabled = NSMLPacketBridgeEnabled();
            if (const char* logPath = getenv("MELONDS_NSML_PACKET_REPLAY_LOG"))
            {
                if (logPath[0])
                    cfg.LogFile = fopen(logPath, "w");
                if (cfg.LogFile)
                    fprintf(cfg.LogFile, "frame,pc,tick,player,op,offset,value,hit,pktTick,pktKeys,pktAction,pktByte5,pktByte6,pktByte7,pktBit\n");
            }
            if (path && path[0])
            {
                std::ifstream file(path);
                std::string line;
                if (std::getline(file, line))
                {
                    const auto header = SplitNSMLCsvLine(line);
                    const int tickCol = FindNSMLCsvColumn(header, "tick");
                    const int playerCol = FindNSMLCsvColumn(header, "player");
                    const int packetCol = FindNSMLCsvColumn(header, "packet_hex");
                    while (tickCol >= 0 && playerCol >= 0 && packetCol >= 0 && std::getline(file, line))
                    {
                        const auto cols = SplitNSMLCsvLine(line);
                        if (tickCol >= static_cast<int>(cols.size()) ||
                            playerCol >= static_cast<int>(cols.size()) ||
                            packetCol >= static_cast<int>(cols.size()))
                            continue;

                        const u32 tick = static_cast<u32>(strtoul(cols[tickCol].c_str(), nullptr, 0));
                        const u32 player = static_cast<u32>(strtoul(cols[playerCol].c_str(), nullptr, 0));
                        if (player > 1)
                            continue;

                        auto& entry = cfg.Packets[tick];
                        if (ParseNSMLHexPacket(cols[packetCol], entry.Packet[player]))
                            entry.Valid[player] = true;
                    }
                }
                cfg.Enabled = !cfg.Packets.empty();
                if (cfg.Enabled)
                    Log(LogLevel::Info, "NSMB packet replay: loaded %zu ticks from %s\n", cfg.Packets.size(), path);
                else
                    Log(LogLevel::Warn, "NSMB packet replay: no packets loaded from %s\n", path);
            }
            if (bridgeEnabled)
                cfg.Enabled = true;
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0)
        return false;

    enum class Op
    {
        None,
        Keys,
        Byte,
        Tick,
        Action,
    };

    Op op = Op::None;
    if (instrAddr == 0x0200E854) // Net::getConsoleKeys(unsigned short)
        op = Op::Keys;
    else if (instrAddr == 0x0200EACC) // Net::getPacketByte(unsigned short, unsigned long)
        op = Op::Byte;
    else if (instrAddr == 0x0200EB10) // Net::getPacketTick(unsigned short)
        op = Op::Tick;
    else if (instrAddr == 0x0200EB30) // Net::getPacketAction(unsigned short)
        op = Op::Action;
    else
        return false;

    const int opIndex =
        op == Op::Keys ? 0 :
        op == Op::Byte ? 1 :
        op == Op::Tick ? 2 :
        op == Op::Action ? 3 : -1;
    if (opIndex < 0 || !cfg.ReplayOpEnabled[opIndex])
        return false;

    const u32 player = cpu->R[0] & 0xFFFF;
    const u32 offset = cpu->R[1];
    if (!IsNSMLMarioVsLuigiPacketContext(cpu->NDS))
        return false;

    const u32 currentTick = NSMLPacketBridgeCanonicalTick(cpu->NDS);
    const u32 tick = (currentTick - cfg.LookupTickDelay) & 0xFFFF;
    if (NSMLPacketBridgeEnabled() && !IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        NSMLWriteLiveReplayPacketsToLocalMPSlots(cpu->NDS, tick, cfg.LiveFallbackWindow, cfg.ReturnLookupTick);

    const NSMLPacketReplayEntry* replaySlotEntry = nullptr;
    u32 replaySlotTick = tick;
    if (NSMLFindReplayEntryForTick(cfg.Packets, tick, cfg.LiveFallbackWindow, &replaySlotEntry, &replaySlotTick))
    {
        if (!IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        {
            NSMLWriteReplayEntryToLocalMPSlots(
                cpu->NDS,
                *replaySlotEntry,
                cfg.ReturnLookupTick ? tick : replaySlotTick,
                cfg.ReturnLookupTick);
        }
    }

    u32 value = 0;
    bool hit = false;

    std::array<u8, 52> selectedPacket {};
    bool packetValid = false;
    if (player <= 1)
    {
        if (NSMLPacketBridgeEnabled())
            packetValid = NSMLSelectBridgePacketForPlayer(cpu->NDS, player, tick, selectedPacket);

        if (!packetValid)
        {
            std::lock_guard<std::mutex> lock(NSMLPacketBridgeMutex);
            packetValid = NSMLFindLiveReplayPacketLocked(
                &cpu->NDS,
                player,
                tick,
                cfg.LiveFallbackWindow,
                selectedPacket);
        }

        if (!packetValid)
        {
            const NSMLPacketReplayEntry* replayEntry = nullptr;
            u32 replayTick = tick;
            if (NSMLFindReplayEntryForTick(cfg.Packets, tick, cfg.LiveFallbackWindow, &replayEntry, &replayTick)
                && replayEntry->Valid[player])
            {
                selectedPacket = replayEntry->Packet[player];
                if (cfg.ReturnLookupTick)
                {
                    selectedPacket[0] = static_cast<u8>(tick & 0xFF);
                    selectedPacket[1] = static_cast<u8>((tick >> 8) & 0xFF);
                }
                packetValid = true;
            }
        }
    }

    if (packetValid)
    {
        const auto& packet = selectedPacket;
        switch (op)
        {
        case Op::Keys:
            value = packet[2] | (packet[3] << 8);
            hit = true;
            break;
        case Op::Byte:
            if (offset < 44)
            {
                value = packet[8 + offset];
                hit = true;
            }
            break;
        case Op::Tick:
            value = cfg.ReturnLookupTick ? tick : (packet[0] | (packet[1] << 8));
            hit = true;
            break;
        case Op::Action:
            value = packet[4];
            hit = true;
            break;
        case Op::None:
            break;
        }
    }

    if (cfg.LogFile)
    {
        std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
        const char* opname =
            op == Op::Keys ? "keys" :
            op == Op::Byte ? "byte" :
            op == Op::Tick ? "tick" :
            op == Op::Action ? "action" : "none";
        fprintf(cfg.LogFile, "%u,%08X,%04X,%u,%s,%u,%08X,%d,%04X,%04X,%02X,%02X,%02X,%02X,%02X\n",
            cpu->NDS.NumFrames,
            instrAddr,
            tick,
            player,
            opname,
            offset,
            value,
            hit ? 1 : 0,
            static_cast<u32>(selectedPacket[0] | (selectedPacket[1] << 8)),
            static_cast<u32>(selectedPacket[2] | (selectedPacket[3] << 8)),
            selectedPacket[4],
            selectedPacket[5],
            selectedPacket[6],
            selectedPacket[7],
            selectedPacket[0x29]);
        fflush(cfg.LogFile);
    }

    if (!hit)
        return false;

    cpu->R[0] = value;
    cpu->JumpTo(cpu->R[14]);
    return true;
}

static void TraceNSMLPacketCapture(ARM* cpu, u32 instrAddr)
{
    struct PacketCaptureConfig
    {
        bool Checked = false;
        bool Enabled = false;
        bool BridgeEnabled = false;
        FILE* LogFile = nullptr;
        std::map<void*, u32> LastTickByNDS;
    };

    static PacketCaptureConfig cfg;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            if (const char* logPath = getenv("MELONDS_NSML_PACKET_CAPTURE_LOG"))
            {
                if (logPath[0])
                    cfg.LogFile = fopen(logPath, "w");
                if (cfg.LogFile)
                    fprintf(cfg.LogFile, "nds,frame,tick,keys,action,packet_hex\n");
            }
            cfg.BridgeEnabled = NSMLPacketBridgeEnabled();
            cfg.Enabled = cfg.LogFile != nullptr || cfg.BridgeEnabled;
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0)
        return;
    if (instrAddr != 0x02011428) // Net::Core::processSendPacket()
        return;

    if (!IsNSMLMarioVsLuigiPacketContext(cpu->NDS))
        return;

    std::array<u8, 52> packet {};
    u32 builtTick = 0;
    u32 keys = 0;
    BuildNSMLMarioVsLuigiPacket(cpu->NDS, packet, builtTick, keys);

    const void* ndsKey = static_cast<const void*>(&cpu->NDS);
    auto last = cfg.LastTickByNDS.find(const_cast<void*>(ndsKey));
    if (last != cfg.LastTickByNDS.end() && last->second == builtTick)
        return;
    cfg.LastTickByNDS[const_cast<void*>(ndsKey)] = builtTick;

    if (cfg.BridgeEnabled)
    {
        std::lock_guard<std::mutex> lock(NSMLPacketBridgeMutex);
        auto& local = NSMLLocalPackets[&cpu->NDS];
        local.Available = true;
        local.Tick = builtTick;
        local.Keys = keys;
        local.Frame = cpu->NDS.NumFrames;
        local.Packet = packet;
    }

    if (cfg.LogFile)
    {
        std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
        fprintf(cfg.LogFile, "%p,%u,0x%04X,0x%04X,0x%02X,",
            static_cast<void*>(&cpu->NDS),
            cpu->NDS.NumFrames,
            builtTick,
            keys,
            packet[4]);
        for (u8 byte : packet)
            fprintf(cfg.LogFile, "%02X", byte);
        fputc('\n', cfg.LogFile);
        fflush(cfg.LogFile);
    }
}

static bool TraceNSMLRandomCallImpl(ARM* cpu, u32 instrAddr, u32 lr, bool hasLR)
{
    struct RandomTraceConfig
    {
        bool Checked = false;
        bool Enabled = false;
        u32 Addr = 0x0200E5A0;
        u32 Addrs[256] {};
        int AddrCount = 0;
        u32 RandomValueAddr = 0x02088A68;
        u32 RandomCallCountAddr = 0x02088A48;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
        FILE* LogFile = nullptr;
    };

    static RandomTraceConfig cfg;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = getenv("MELONDS_NSML_RANDOM_TRACE") != nullptr;
            if (const char* addr = getenv("MELONDS_NSML_RANDOM_TRACE_ADDR"))
                cfg.Addr = static_cast<u32>(strtoul(addr, nullptr, 0));
            cfg.Addrs[cfg.AddrCount++] = cfg.Addr;
            if (const char* addrs = getenv("MELONDS_NSML_RANDOM_TRACE_ADDRS"))
            {
                char buf[4096];
                strncpy(buf, addrs, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                for (char* tok = strtok(buf, ", \t\r\n"); tok && cfg.AddrCount < 256; tok = strtok(nullptr, ", \t\r\n"))
                    cfg.Addrs[cfg.AddrCount++] = static_cast<u32>(strtoul(tok, nullptr, 0));
            }
            if (const char* randomValueAddr = getenv("MELONDS_NSML_RANDOM_TRACE_VALUE_ADDR"))
                cfg.RandomValueAddr = static_cast<u32>(strtoul(randomValueAddr, nullptr, 0));
            if (const char* randomCallCountAddr = getenv("MELONDS_NSML_RANDOM_TRACE_CALLCOUNT_ADDR"))
                cfg.RandomCallCountAddr = static_cast<u32>(strtoul(randomCallCountAddr, nullptr, 0));
            if (const char* startFrame = getenv("MELONDS_NSML_RANDOM_TRACE_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_RANDOM_TRACE_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            if (const char* logPath = getenv("MELONDS_NSML_RANDOM_TRACE_LOG"))
            {
                if (logPath[0])
                {
                    cfg.LogFile = fopen(logPath, "w");
                    if (cfg.LogFile)
                        fprintf(cfg.LogFile, "nds,frame,pc,caller,lr,random_value,random_call_count\n");
                }
            }
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0) return false;
    bool matched = false;
    for (int i = 0; i < cfg.AddrCount; i++)
    {
        if (instrAddr == cfg.Addrs[i])
        {
            matched = true;
            break;
        }
    }
    if (!matched) return false;
    if (cpu->NDS.NumFrames < cfg.StartFrame || cpu->NDS.NumFrames > cfg.EndFrame) return false;

    const u32 randomValue = cpu->NDS.ARM9Read32(cfg.RandomValueAddr);
    const u8 randomCallCount = cpu->NDS.ARM9Read8(cfg.RandomCallCountAddr);
    if (!hasLR)
        lr = cpu->R[14];
    const u32 caller = lr >= 4 ? lr - 4 : lr;
    if (cfg.LogFile)
    {
        std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
        fprintf(cfg.LogFile,
            "%p,%u,%08X,%08X,%08X,%08X,%02X\n",
            static_cast<void*>(&cpu->NDS),
            cpu->NDS.NumFrames,
            instrAddr,
            caller,
            lr,
            randomValue,
            randomCallCount);
        fflush(cfg.LogFile);
    }
    else
    {
        std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
        printf("NSMB Random: nds=%p frame=%u pc=%08X caller=%08X lr=%08X random=%08X count=%02X\n",
            static_cast<void*>(&cpu->NDS),
            cpu->NDS.NumFrames,
            instrAddr,
            caller,
            lr,
            randomValue,
            randomCallCount);
    }
    return true;
}

static bool IsNSMLMainRAMAddress(u32 addr)
{
    return (addr & 0xFF000000) == 0x02000000;
}

static bool IsNSMLDTCMAddress(ARM* cpu, u32 addr)
{
    return cpu && cpu->Num == 0 && cpu->NDS.ARM9.DTCM && ((addr & cpu->NDS.ARM9.DTCMMask) == cpu->NDS.ARM9.DTCMBase);
}

static bool IsNSMLITCMAddress(ARM* cpu, u32 addr)
{
    return cpu && cpu->Num == 0 && addr < cpu->NDS.ARM9.ITCMSize;
}

static u8 ReadNSMLTraceByte(ARM* cpu, u32 addr)
{
    if (IsNSMLITCMAddress(cpu, addr))
        return cpu->NDS.ARM9.ITCM[addr & (ITCMPhysicalSize - 1)];
    if (IsNSMLDTCMAddress(cpu, addr))
        return cpu->NDS.ARM9.DTCM[addr & 0x3FFF];
    return cpu->NDS.ARM9Read8(addr);
}

static u32 ReadNSMLTrace32(ARM* cpu, u32 addr)
{
    return static_cast<u32>(ReadNSMLTraceByte(cpu, addr))
        | (static_cast<u32>(ReadNSMLTraceByte(cpu, addr + 1)) << 8)
        | (static_cast<u32>(ReadNSMLTraceByte(cpu, addr + 2)) << 16)
        | (static_cast<u32>(ReadNSMLTraceByte(cpu, addr + 3)) << 24);
}

static u64 HashNSMLTraceRange(ARM* cpu, u32 addr, u32 len)
{
    if (!cpu || len == 0)
        return 0;

    u64 hash = 1469598103934665603ULL;
    for (u32 i = 0; i < len; i++)
    {
        hash ^= ReadNSMLTraceByte(cpu, addr + i);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void WriteNSMLHexDump(FILE* file, ARM* cpu, u32 addr, u32 len)
{
    if (!file || !cpu || (!IsNSMLMainRAMAddress(addr) && !IsNSMLDTCMAddress(cpu, addr) && !IsNSMLITCMAddress(cpu, addr)) || len == 0)
    {
        fputc('-', file);
        return;
    }

    for (u32 i = 0; i < len; i++)
        fprintf(file, "%02X", ReadNSMLTraceByte(cpu, addr + i));
}

static bool ParseNSMLU32List(const char* value, std::vector<u32>& out)
{
    out.clear();
    if (!value || !value[0])
        return false;

    char buf[1024];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char* tok = strtok(buf, ", \t\r\n"); tok; tok = strtok(nullptr, ", \t\r\n"))
        out.push_back(static_cast<u32>(strtoul(tok, nullptr, 0)));
    return !out.empty();
}

static void TraceNSMLWrite(ARM* cpu, u32 addr, u32 value, u32 size)
{
    struct WriteTraceConfig
    {
        bool Checked = false;
        bool Enabled = false;
        u32 StartFrame = 0;
        u32 EndFrame = 0;
        std::vector<u32> Addrs;
        FILE* LogFile = nullptr;
    };

    static WriteTraceConfig cfg;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = getenv("MELONDS_NSML_WRITE_TRACE") != nullptr;
            ParseNSMLU32List(getenv("MELONDS_NSML_WRITE_TRACE_ADDRS"), cfg.Addrs);
            if (const char* startFrame = getenv("MELONDS_NSML_WRITE_TRACE_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_WRITE_TRACE_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            if (const char* logPath = getenv("MELONDS_NSML_WRITE_TRACE_LOG"))
            {
                if (logPath[0])
                    cfg.LogFile = fopen(logPath, "w");
                if (cfg.LogFile)
                    fprintf(cfg.LogFile, "nds,frame,pc,lr,sp,cpsr,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,addr,size,value,old,pc_dump,sp_dump\n");
            }
            cfg.Enabled = cfg.Enabled && cfg.LogFile && !cfg.Addrs.empty();
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0)
        return;
    if (cpu->NDS.NumFrames < cfg.StartFrame)
        return;
    if (cfg.EndFrame != 0 && cpu->NDS.NumFrames > cfg.EndFrame)
        return;

    bool matched = false;
    for (u32 watchAddr : cfg.Addrs)
    {
        if (addr <= watchAddr && watchAddr < addr + (size / 8))
        {
            matched = true;
            break;
        }
    }
    if (!matched)
        return;

    u32 oldValue = 0;
    if (size == 8)
        oldValue = cpu->NDS.ARM9Read8(addr);
    else if (size == 16)
        oldValue = cpu->NDS.ARM9Read16(addr);
    else
        oldValue = cpu->NDS.ARM9Read32(addr);

    const u32 pc = cpu->R[15] - ((cpu->CPSR & 0x20) ? 2 : 4);
    std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
    fprintf(cfg.LogFile,
        "%p,%u,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%u,%08X,%08X,",
        static_cast<void*>(&cpu->NDS),
        cpu->NDS.NumFrames,
        pc,
        cpu->R[14],
        cpu->R[13],
        cpu->CPSR,
        cpu->R[0],
        cpu->R[1],
        cpu->R[2],
        cpu->R[3],
        cpu->R[4],
        cpu->R[5],
        cpu->R[6],
        cpu->R[7],
        cpu->R[8],
        cpu->R[9],
        cpu->R[10],
        cpu->R[11],
        cpu->R[12],
        addr,
        size,
        value,
        oldValue);
    WriteNSMLHexDump(cfg.LogFile, cpu, pc >= 32 ? pc - 32 : pc, 96);
    fputc(',', cfg.LogFile);
    WriteNSMLHexDump(cfg.LogFile, cpu, cpu->R[13], 64);
    fputc('\n', cfg.LogFile);
    fflush(cfg.LogFile);
}

static bool NSMLWriteTraceMaybeEnabled()
{
#ifdef NSML_CORE_RUNTIME_HOOKS_DISABLED
    return false;
#else
    static const bool enabled = getenv("MELONDS_NSML_WRITE_TRACE") != nullptr;
    return enabled;
#endif
}

static bool TraceNSMLCallImpl(ARM* cpu, u32 instrAddr)
{
    struct CallTraceConfig
    {
        bool Checked = false;
        bool Enabled = false;
        u32 Addrs[256] {};
        int AddrCount = 0;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
        u32 DumpLen = 32;
        bool UseR2AsDumpLen = false;
        FILE* LogFile = nullptr;
    };

    static CallTraceConfig cfg;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = getenv("MELONDS_NSML_CALL_TRACE") != nullptr;

            // Useful A2DJ Net entry points by default. Override or extend with
            // MELONDS_NSML_CALL_TRACE_ADDRS while narrowing the packet boundary.
            cfg.Addrs[cfg.AddrCount++] = 0x0200E5E8; // Net::syncRandomFull()
            cfg.Addrs[cfg.AddrCount++] = 0x0200E5F4; // Net::syncRandomFast()
            cfg.Addrs[cfg.AddrCount++] = 0x02010810; // Net::onPacketPollingDefault()
            cfg.Addrs[cfg.AddrCount++] = 0x02010828; // Net::onRenderSignalStrengthDefault()
            cfg.Addrs[cfg.AddrCount++] = 0x02010930; // Net::setDefaultHandlers()
            cfg.Addrs[cfg.AddrCount++] = 0x0200F98C; // Net::Core::transferPacket()
            cfg.Addrs[cfg.AddrCount++] = 0x020101E4; // Net::updatePacket()
            cfg.Addrs[cfg.AddrCount++] = 0x02010D0C; // Net::Core::createPacketSequencer()
            cfg.Addrs[cfg.AddrCount++] = 0x02010DAC; // Net::Core::readPacketInt()
            cfg.Addrs[cfg.AddrCount++] = 0x02010E14; // Net::Core::readPacketByte()
            cfg.Addrs[cfg.AddrCount++] = 0x02010E4C; // Net::Core::writePacketInt()
            cfg.Addrs[cfg.AddrCount++] = 0x02010E80; // Net::Core::writePacketByte()
            cfg.Addrs[cfg.AddrCount++] = 0x02010E90; // Net::Core::freePacketBytes()
            cfg.Addrs[cfg.AddrCount++] = 0x02010EBC; // Net::Core::allocPacketBytes()
            cfg.Addrs[cfg.AddrCount++] = 0x02010F04; // Net::Core::shareRandomSeed()
            cfg.Addrs[cfg.AddrCount++] = 0x020110E4; // Net::Core::checkAllPacketBits()
            cfg.Addrs[cfg.AddrCount++] = 0x0201122C; // Net::Core::advancePacketSequencer()
            cfg.Addrs[cfg.AddrCount++] = 0x02011360; // Net::Core::processRecvPacket()
            cfg.Addrs[cfg.AddrCount++] = 0x02011428; // Net::Core::processSendPacket()
            cfg.Addrs[cfg.AddrCount++] = 0x02011504; // Net::Core::clearPacket()
            cfg.Addrs[cfg.AddrCount++] = 0x020115A8; // Net::Core::initPacket()

            if (const char* addrs = getenv("MELONDS_NSML_CALL_TRACE_ADDRS"))
            {
                cfg.AddrCount = 0;
                char buf[4096];
                strncpy(buf, addrs, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                for (char* tok = strtok(buf, ", \t\r\n"); tok && cfg.AddrCount < 256; tok = strtok(nullptr, ", \t\r\n"))
                    cfg.Addrs[cfg.AddrCount++] = static_cast<u32>(strtoul(tok, nullptr, 0));
            }
            if (const char* startFrame = getenv("MELONDS_NSML_CALL_TRACE_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_CALL_TRACE_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            if (const char* dumpLen = getenv("MELONDS_NSML_CALL_TRACE_DUMP_LEN"))
                cfg.DumpLen = static_cast<u32>(strtoul(dumpLen, nullptr, 0));
            if (cfg.DumpLen > 512) cfg.DumpLen = 512;
            cfg.UseR2AsDumpLen = NSMLEnvFlag("MELONDS_NSML_CALL_TRACE_USE_R2_DUMP_LEN");
            if (const char* logPath = getenv("MELONDS_NSML_CALL_TRACE_LOG"))
            {
                if (logPath[0])
                {
                    cfg.LogFile = fopen(logPath, "w");
                    if (cfg.LogFile)
                        fprintf(cfg.LogFile, "nds,frame,pc,caller,lr,sp,cpsr,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,net_tick,net_action,net_seq_ids,net_seq_cursors,net_send_bitmap,net_seq_lengths,net_recv_bitmap,net_random,vs_step,vs_timer,vs_flags,r0_dump,r1_dump,r2_dump,r3_dump,sp_dump\n");
                }
            }
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0) return false;
    if (cpu->NDS.NumFrames < cfg.StartFrame || cpu->NDS.NumFrames > cfg.EndFrame) return false;

    bool matched = false;
    for (int i = 0; i < cfg.AddrCount; i++)
    {
        if (instrAddr == cfg.Addrs[i])
        {
            matched = true;
            break;
        }
    }
    if (!matched) return false;

    const u32 lr = cpu->R[14];
    const u32 caller = lr >= 4 ? lr - 4 : lr;
    const u32 r0 = cpu->R[0];
    const u32 r1 = cpu->R[1];
    const u32 r2 = cpu->R[2];
    const u32 r3 = cpu->R[3];
    const u32 sp = cpu->R[13];
    const u32 cpsr = cpu->CPSR;
    const u32 netTick = cpu->NDS.ARM9Read16(0x020888E0);
    const u32 netAction = cpu->NDS.ARM9Read8(0x020888E4);
    const u32 netSeqIDs = cpu->NDS.ARM9Read32(0x02088A58);
    const u32 netSeqCursors = cpu->NDS.ARM9Read32(0x02088A5C);
    const u32 netSendBitmap = cpu->NDS.ARM9Read32(0x02088A60);
    const u32 netSeqLengths = cpu->NDS.ARM9Read32(0x02088A64);
    const u32 netRecvBitmap = cpu->NDS.ARM9Read32(0x02088A84);
    const u32 netRandom = cpu->NDS.ARM9Read32(0x02088A68);
    const bool r0IsVsConnect = IsNSMLMainRAMAddress(r0) && cpu->NDS.ARM9Read16(r0 + 0x0C) == 0x0006;
    const u32 vsStep = r0IsVsConnect ? cpu->NDS.ARM9Read32(r0 + 0x144) : 0;
    const u32 vsTimer = r0IsVsConnect ? cpu->NDS.ARM9Read32(r0 + 0x148) : 0;
    const u32 vsFlags = r0IsVsConnect ? cpu->NDS.ARM9Read32(r0 + 0x154) : 0;
    u32 dumpLen = cfg.DumpLen;
    if (cfg.UseR2AsDumpLen && r2 > 0 && r2 < dumpLen) dumpLen = r2;

    FILE* out = cfg.LogFile ? cfg.LogFile : stdout;
    std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
    fprintf(out,
        "%p,%u,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%04X,%02X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,",
        static_cast<void*>(&cpu->NDS),
        cpu->NDS.NumFrames,
        instrAddr,
        caller,
        lr,
        sp,
        cpsr,
        r0,
        r1,
        r2,
        r3,
        cpu->R[4],
        cpu->R[5],
        cpu->R[6],
        cpu->R[7],
        cpu->R[8],
        cpu->R[9],
        cpu->R[10],
        cpu->R[11],
        cpu->R[12],
        netTick,
        netAction,
        netSeqIDs,
        netSeqCursors,
        netSendBitmap,
        netSeqLengths,
        netRecvBitmap,
        netRandom,
        vsStep,
        vsTimer,
        vsFlags);
    WriteNSMLHexDump(out, cpu, r0, dumpLen);
    fputc(',', out);
    WriteNSMLHexDump(out, cpu, r1, dumpLen);
    fputc(',', out);
    WriteNSMLHexDump(out, cpu, r2, dumpLen);
    fputc(',', out);
    WriteNSMLHexDump(out, cpu, r3, dumpLen);
    fputc(',', out);
    WriteNSMLHexDump(out, cpu, sp, cfg.DumpLen);
    fputc('\n', out);
    fflush(out);
    return true;
}

bool TraceNSMLRandomCall(ARM* cpu, u32 instrAddr)
{
    return TraceNSMLRandomCallImpl(cpu, instrAddr, 0, false);
}

static void PatchNSMLPlayerModelRenderPtrs(ARM* cpu, u32 instrAddr)
{
    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
    };

    static Config cfg;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_GUARD_PLAYER_MODEL_RENDER_PTRS");
            if (const char* startFrame = getenv("MELONDS_NSML_GUARD_PLAYER_MODEL_RENDER_PTRS_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_GUARD_PLAYER_MODEL_RENDER_PTRS_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0) return;
    if (cpu->NDS.NumFrames < cfg.StartFrame || cpu->NDS.NumFrames > cfg.EndFrame) return;
    if (instrAddr != 0x02128AB4 && instrAddr != 0x02128AC0) return;

    const u32 playerModel = cpu->R[10];
    if (!IsNSMLMainRAMAddress(playerModel)) return;

    const s32 modelState = static_cast<s8>(ReadNSMLTraceByte(cpu, playerModel + 0x3C1));
    u32 headState = ReadNSMLTraceByte(cpu, playerModel + 0x3C2);
    if (modelState < 0 || modelState > 1) return;
    if (headState > 3) headState = 0;

    const u32 modelCollection = playerModel + 0x40 + (static_cast<u32>(modelState) * 0x14);
    const u32 bodyModel = ReadNSMLTrace32(cpu, modelCollection);
    const u32 headModel = ReadNSMLTrace32(cpu, modelCollection + 0x04 + (headState * 0x04));
    if (!IsNSMLMainRAMAddress(headModel)) return;

    bool patched = false;
    const u32 oldR4 = cpu->R[4];
    const u32 oldR0 = cpu->R[0];
    if (!IsNSMLMainRAMAddress(cpu->R[4]))
    {
        cpu->R[4] = headModel;
        patched = true;
    }
    if (instrAddr == 0x02128AC0 && !IsNSMLMainRAMAddress(cpu->R[0]))
    {
        cpu->R[0] = headModel + 0x04;
        patched = true;
    }

    if (patched)
    {
        Log(LogLevel::Warn,
            "NSMB guard: repaired PlayerModel render ptr frame=%u pc=%08X playerModel=%08X modelState=%d headState=%u body=%08X head=%08X oldR4=%08X newR4=%08X oldR0=%08X newR0=%08X\n",
            cpu->NDS.NumFrames,
            instrAddr,
            playerModel,
            modelState,
            headState,
            bodyModel,
            headModel,
            oldR4,
            cpu->R[4],
            oldR0,
            cpu->R[0]);
    }
}

static s32 NSMLCameraRingDelta(u32 target, u32 current)
{
    constexpr s32 span = 0x00400000;
    s32 delta = static_cast<s32>((target - current) & (span - 1));
    if (delta > (span / 2))
        delta -= span;
    return delta;
}

static u32 NSMLCameraRingAdd(u32 value, s32 delta)
{
    constexpr s32 span = 0x00400000;
    s32 wrapped = (static_cast<s32>(value & (span - 1)) + delta) % span;
    if (wrapped < 0)
        wrapped += span;
    return static_cast<u32>(wrapped);
}

static void PatchNSMLDynamicCameraLead(ARM* cpu, u32 instrAddr)
{
    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
        u32 RightLead = 0x00058000;
        u32 LeftLead = 0x000A8000;
        u32 NeutralLead = 0x00080000;
        u32 MinStep = 0x00001000;
        u32 MaxStep = 0x00006000;
        u32 BaseStep = 0x00004000;
        u32 VelocityThreshold = 0x40;
    };

    static Config cfg;
    static bool logged = false;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_DYNAMIC_CAMERA_LEAD");
            if (const char* startFrame = getenv("MELONDS_NSML_DYNAMIC_CAMERA_LEAD_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_DYNAMIC_CAMERA_LEAD_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            if (cfg.EndFrame == 0)
                cfg.EndFrame = 0xFFFFFFFF;
            if (const char* rightLead = getenv("MELONDS_NSML_DYNAMIC_CAMERA_RIGHT_LEAD"))
                cfg.RightLead = static_cast<u32>(strtoul(rightLead, nullptr, 0));
            if (const char* leftLead = getenv("MELONDS_NSML_DYNAMIC_CAMERA_LEFT_LEAD"))
                cfg.LeftLead = static_cast<u32>(strtoul(leftLead, nullptr, 0));
            if (const char* neutralLead = getenv("MELONDS_NSML_DYNAMIC_CAMERA_NEUTRAL_LEAD"))
                cfg.NeutralLead = static_cast<u32>(strtoul(neutralLead, nullptr, 0));
            if (const char* minStep = getenv("MELONDS_NSML_DYNAMIC_CAMERA_MIN_STEP"))
                cfg.MinStep = static_cast<u32>(strtoul(minStep, nullptr, 0));
            if (const char* maxStep = getenv("MELONDS_NSML_DYNAMIC_CAMERA_MAX_STEP"))
                cfg.MaxStep = static_cast<u32>(strtoul(maxStep, nullptr, 0));
            if (const char* baseStep = getenv("MELONDS_NSML_DYNAMIC_CAMERA_BASE_STEP"))
                cfg.BaseStep = static_cast<u32>(strtoul(baseStep, nullptr, 0));
            if (const char* threshold = getenv("MELONDS_NSML_DYNAMIC_CAMERA_VELOCITY_THRESHOLD"))
                cfg.VelocityThreshold = static_cast<u32>(strtoul(threshold, nullptr, 0));
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0)
        return;
    if (instrAddr != 0x020CE304)
        return;
    const u32 frame = cpu->NDS.NumFrames;
    if (frame < cfg.StartFrame || frame > cfg.EndFrame)
        return;
    if (!IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        return;

    constexpr u32 stageCameraX = 0x020CAE1C;
    constexpr u32 stageDisplayCameraX = 0x02085AB4;
    constexpr u32 gameLocalPlayerID = 0x02085A7C;
    constexpr u32 playerCameraFocusPosX = 0x020CAEBC;
    constexpr u32 playerCameraFocusVelX = 0x020CAEEC;
    constexpr u32 focusStride = 0x10;

    const u32 localPlayerID = cpu->NDS.ARM9Read32(gameLocalPlayerID) & 1u;
    for (u32 playerID = 0; playerID < 2; playerID++)
    {
        const u32 focusOffset = playerID * focusStride;
        const u32 focusX = cpu->NDS.ARM9Read32(playerCameraFocusPosX + focusOffset);
        const s32 velocityX = static_cast<s32>(cpu->NDS.ARM9Read32(playerCameraFocusVelX + focusOffset));
        const s32 threshold = static_cast<s32>(cfg.VelocityThreshold);
        const u32 lead = velocityX > threshold ? cfg.RightLead
            : (velocityX < -threshold ? cfg.LeftLead : cfg.NeutralLead);
        const u32 target = (focusX - lead) & 0x003FFFFF;
        const u32 cameraAddr = stageCameraX + playerID * sizeof(u32);
        const u32 current = cpu->NDS.ARM9Read32(cameraAddr) & 0x003FFFFF;
        s32 delta = NSMLCameraRingDelta(target, current);
        if (delta == 0)
            continue;

        const u32 absVelocity = static_cast<u32>(velocityX < 0 ? -velocityX : velocityX);
        u32 step = cfg.BaseStep + std::min<u32>(absVelocity, cfg.MaxStep);
        if (cfg.MaxStep != 0)
            step = std::min(step, cfg.MaxStep);
        if (cfg.MinStep != 0)
            step = std::max(step, cfg.MinStep);
        if (step != 0)
        {
            const s32 signedStep = static_cast<s32>(step);
            delta = std::clamp(delta, -signedStep, signedStep);
        }
        const u32 next = NSMLCameraRingAdd(current, delta);
        cpu->NDS.ARM9Write32(cameraAddr, next);
        if (playerID == localPlayerID)
            cpu->NDS.ARM9Write32(stageDisplayCameraX, next);

        if (!logged)
        {
            Log(LogLevel::Debug,
                "NSMB dynamic camera lead: frame=%u pc=%08X player=%u focusX=%08X vel=%08X lead=%08X step=%08X current=%08X target=%08X next=%08X\n",
                frame,
                instrAddr,
                playerID,
                focusX,
                static_cast<u32>(velocityX),
                lead,
                step,
                current,
                target,
                next);
            logged = true;
        }
    }
}

static void PatchNSMLRenderCameraAlias(ARM* cpu, u32 instrAddr)
{
    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        bool ClientOnly = true;
        u32 SourcePlayer = 1;
        u32 DestPlayer = 0;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
    };

    static Config cfg;
    static bool logged = false;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_RENDER_CAMERA_ALIAS");
            cfg.ClientOnly = !NSMLEnvFlag("MELONDS_NSML_RENDER_CAMERA_ALIAS_ALL_ROLES");
            if (const char* source = getenv("MELONDS_NSML_RENDER_CAMERA_ALIAS_SOURCE_PLAYER"))
                cfg.SourcePlayer = static_cast<u32>(strtoul(source, nullptr, 0)) & 1u;
            if (const char* dest = getenv("MELONDS_NSML_RENDER_CAMERA_ALIAS_DEST_PLAYER"))
                cfg.DestPlayer = static_cast<u32>(strtoul(dest, nullptr, 0)) & 1u;
            if (const char* startFrame = getenv("MELONDS_NSML_RENDER_CAMERA_ALIAS_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_RENDER_CAMERA_ALIAS_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0) return;
    if (instrAddr != 0x020CE238 && instrAddr != 0x020CE304 &&
        instrAddr != 0x020CE42C && instrAddr != 0x020CE46C)
        return;
    if (cpu->NDS.NumFrames < cfg.StartFrame || cpu->NDS.NumFrames > cfg.EndFrame) return;
    if (!IsNSMLMarioVsLuigiGameplay(cpu->NDS)) return;
    if (cfg.ClientOnly)
    {
        const char* role = getenv("MELONDS_NSML_ROLE");
        if (!role || strcmp(role, "client") != 0)
        {
            role = getenv("MELONDS_NSML_LAN_ROLE");
            if (!role || strcmp(role, "client") != 0)
                return;
        }
    }

    constexpr u32 stageCameraX = 0x020CAE1C;
    constexpr u32 stageCameraY = 0x020CAD94;
    constexpr u32 stageCameraWidth = 0x020CADA4;
    constexpr u32 stageCameraHeight = 0x020CAD8C;
    constexpr u32 stageDisplayCameraX = 0x02085AB4;
    const u32 sourceOffset = cfg.SourcePlayer * 4u;
    const u32 destOffset = cfg.DestPlayer * 4u;
    const u32 sourceX = cpu->NDS.ARM9Read32(stageCameraX + sourceOffset);
    cpu->NDS.ARM9Write32(stageCameraX + destOffset, sourceX);
    cpu->NDS.ARM9Write32(stageCameraY + destOffset, cpu->NDS.ARM9Read32(stageCameraY + sourceOffset));
    cpu->NDS.ARM9Write32(stageCameraWidth + destOffset, cpu->NDS.ARM9Read32(stageCameraWidth + sourceOffset));
    cpu->NDS.ARM9Write32(stageCameraHeight + destOffset, cpu->NDS.ARM9Read32(stageCameraHeight + sourceOffset));
    if (cfg.DestPlayer == 0)
        cpu->NDS.ARM9Write32(stageDisplayCameraX, sourceX);
    if (!logged)
    {
        Log(LogLevel::Debug,
            "NSMB render camera alias: frame=%u pc=%08X src=%u dst=%u x=%08X\n",
            cpu->NDS.NumFrames,
            instrAddr,
            cfg.SourcePlayer,
            cfg.DestPlayer,
            sourceX);
        logged = true;
    }
}

static void PatchNSMLCameraFocusLoopCount(ARM* cpu, u32 instrAddr)
{
    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        bool HostOnly = false;
        bool ClientOnly = false;
        u32 Count = 2;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
    };

    static Config cfg;
    static std::map<NDS*, u32> loggedFrame;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_FORCE_CAMERA_FOCUS_LOOP_COUNT");
            cfg.HostOnly = NSMLEnvFlag("MELONDS_NSML_FORCE_CAMERA_FOCUS_LOOP_COUNT_HOST_ONLY");
            cfg.ClientOnly = NSMLEnvFlag("MELONDS_NSML_FORCE_CAMERA_FOCUS_LOOP_COUNT_CLIENT_ONLY");
            if (const char* count = getenv("MELONDS_NSML_FORCE_CAMERA_FOCUS_LOOP_COUNT_VALUE"))
                cfg.Count = std::max<u32>(1, static_cast<u32>(strtoul(count, nullptr, 0)));
            if (const char* startFrame = getenv("MELONDS_NSML_FORCE_CAMERA_FOCUS_LOOP_COUNT_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_FORCE_CAMERA_FOCUS_LOOP_COUNT_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            if (cfg.EndFrame == 0)
                cfg.EndFrame = 0xFFFFFFFF;
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0)
        return;
    if (instrAddr != 0x020BAAE8 && instrAddr != 0x020BAC1C)
        return;

    const u32 frame = cpu->NDS.NumFrames;
    if (frame < cfg.StartFrame || frame > cfg.EndFrame)
        return;
    if (!IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        return;
    if (cfg.HostOnly || cfg.ClientOnly)
    {
        const char* role = getenv("MELONDS_NSML_ROLE");
        if (!role || !role[0])
            role = getenv("MELONDS_NSML_LAN_ROLE");
        const bool isHost = role && strcmp(role, "host") == 0;
        const bool isClient = role && strcmp(role, "client") == 0;
        if ((cfg.HostOnly && !isHost) || (cfg.ClientOnly && !isClient))
            return;
    }
    if (cpu->R[0] >= cfg.Count)
        return;

    const u32 oldCount = cpu->R[0];
    cpu->R[0] = cfg.Count;
    if (loggedFrame[&cpu->NDS] != frame)
    {
        Log(LogLevel::Debug,
            "NSMB camera focus loop count forced: frame=%u pc=%08X old=%u new=%u sb=%u\n",
            frame,
            instrAddr,
            oldCount,
            cfg.Count,
            cpu->R[9]);
        loggedFrame[&cpu->NDS] = frame;
    }
}

static void TraceNSMLStageCamera(ARM* cpu, u32 instrAddr)
{
    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
        u32 Interval = 1;
    };

    static Config cfg;
    static std::map<NDS*, std::map<u32, u32>> lastLoggedFrame;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_TRACE_STAGE_CAMERA");
            if (const char* startFrame = getenv("MELONDS_NSML_TRACE_STAGE_CAMERA_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_TRACE_STAGE_CAMERA_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            if (const char* interval = getenv("MELONDS_NSML_TRACE_STAGE_CAMERA_INTERVAL"))
                cfg.Interval = std::max<u32>(1, static_cast<u32>(strtoul(interval, nullptr, 0)));
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0)
        return;
    if (instrAddr != 0x020CDF78 && instrAddr != 0x020CE238 &&
        instrAddr != 0x020CE304 && instrAddr != 0x020CE42C &&
        instrAddr != 0x020CE46C)
        return;

    const u32 frame = cpu->NDS.NumFrames;
    if (frame < cfg.StartFrame || frame > cfg.EndFrame)
        return;
    if ((frame % cfg.Interval) != 0)
        return;
    if (!IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        return;

    {
        std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
        u32& lastFrame = lastLoggedFrame[&cpu->NDS][instrAddr];
        if (lastFrame == frame)
            return;
        lastFrame = frame;
    }

    constexpr u32 stageCameraX = 0x020CAE1C;
    constexpr u32 stageCameraY = 0x020CAD94;
    constexpr u32 stageCameraWidth = 0x020CADA4;
    constexpr u32 stageCameraHeight = 0x020CAD8C;
    constexpr u32 stageDisplayCameraX = 0x02085AB4;
    constexpr u32 gameLocalPlayerID = 0x02085A7C;
    constexpr u32 gameViewMatrix = 0x02085B20;

    u32 cameraBase = 0;
    if (IsNSMLMainRAMAddress(cpu->R[0]) && cpu->NDS.ARM9Read16(cpu->R[0] + 0x0C) == 0x013C)
        cameraBase = cpu->R[0];
    else if (IsNSMLMainRAMAddress(cpu->R[4]) && cpu->NDS.ARM9Read16(cpu->R[4] + 0x0C) == 0x013C)
        cameraBase = cpu->R[4];
    else
        cameraBase = NSMLFindObjectBaseByID(cpu->NDS, 0x013C);

    auto readObj = [&](u32 offset) -> u32
    {
        return IsNSMLMainRAMAddress(cameraBase) ? cpu->NDS.ARM9Read32(cameraBase + offset) : 0;
    };

    const char* role = getenv("MELONDS_NSML_ROLE");
    if (!role || !role[0])
        role = getenv("MELONDS_NSML_LAN_ROLE");
    if (!role || !role[0])
        role = "-";

    const u64 viewHash = HashNSMLTraceRange(cpu, gameViewMatrix, 0x40);
    const u64 renderHash = HashNSMLTraceRange(cpu, 0x023F8300, 0x240);
    std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
    std::printf(
        "NSMB StageCameraTrace: role=%s frame=%u pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X "
        "local=%u base=%08X state=%08X/%08X/%08X target=%08X,%08X,%08X pos=%08X,%08X,%08X up=%08X,%08X,%08X "
        "unk=%08X,%08X,%08X,%08X,%08X,%08X words=%08X,%08X,%08X,%08X "
        "camX=%08X/%08X camY=%08X/%08X camW=%08X/%08X camH=%08X/%08X displayX=%08X "
        "viewHash=%016llX view=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X "
        "renderHash=%016llX dispcnt=%08X/%08X bgofsA=%04X,%04X,%04X,%04X,%04X,%04X,%04X,%04X bgofsB=%04X,%04X,%04X,%04X,%04X,%04X,%04X,%04X\n",
        role,
        frame,
        instrAddr,
        cpu->R[0],
        cpu->R[1],
        cpu->R[2],
        cpu->R[3],
        cpu->R[4],
        cpu->NDS.ARM9Read32(gameLocalPlayerID),
        cameraBase,
        readObj(0x120),
        readObj(0x124),
        readObj(0x128),
        readObj(0x0CC),
        readObj(0x0D0),
        readObj(0x0D4),
        readObj(0x0DC),
        readObj(0x0E0),
        readObj(0x0E4),
        readObj(0x0EC),
        readObj(0x0F0),
        readObj(0x0F4),
        readObj(0x114),
        readObj(0x118),
        readObj(0x11C),
        readObj(0x128),
        readObj(0x12C),
        readObj(0x130),
        readObj(0x190),
        readObj(0x194),
        readObj(0x19C),
        readObj(0x1A0),
        cpu->NDS.ARM9Read32(stageCameraX),
        cpu->NDS.ARM9Read32(stageCameraX + 4),
        cpu->NDS.ARM9Read32(stageCameraY),
        cpu->NDS.ARM9Read32(stageCameraY + 4),
        cpu->NDS.ARM9Read32(stageCameraWidth),
        cpu->NDS.ARM9Read32(stageCameraWidth + 4),
        cpu->NDS.ARM9Read32(stageCameraHeight),
        cpu->NDS.ARM9Read32(stageCameraHeight + 4),
        cpu->NDS.ARM9Read32(stageDisplayCameraX),
        static_cast<unsigned long long>(viewHash),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x00),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x04),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x08),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x0C),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x10),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x14),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x18),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x1C),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x20),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x24),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x28),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x2C),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x30),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x34),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x38),
        cpu->NDS.ARM9Read32(gameViewMatrix + 0x3C),
        static_cast<unsigned long long>(renderHash),
        cpu->NDS.ARM9Read32(0x04000000),
        cpu->NDS.ARM9Read32(0x04001000),
        cpu->NDS.ARM9Read16(0x04000010),
        cpu->NDS.ARM9Read16(0x04000012),
        cpu->NDS.ARM9Read16(0x04000014),
        cpu->NDS.ARM9Read16(0x04000016),
        cpu->NDS.ARM9Read16(0x04000018),
        cpu->NDS.ARM9Read16(0x0400001A),
        cpu->NDS.ARM9Read16(0x0400001C),
        cpu->NDS.ARM9Read16(0x0400001E),
        cpu->NDS.ARM9Read16(0x04001010),
        cpu->NDS.ARM9Read16(0x04001012),
        cpu->NDS.ARM9Read16(0x04001014),
        cpu->NDS.ARM9Read16(0x04001016),
        cpu->NDS.ARM9Read16(0x04001018),
        cpu->NDS.ARM9Read16(0x0400101A),
        cpu->NDS.ARM9Read16(0x0400101C),
        cpu->NDS.ARM9Read16(0x0400101E));
}

static void TraceNSMLPlayerRender(ARM* cpu, u32 instrAddr)
{
    struct Config
    {
        bool Checked = false;
        bool Enabled = false;
        u32 StartFrame = 0;
        u32 EndFrame = 0xFFFFFFFF;
    };

    static Config cfg;
    static bool header = false;
    static std::map<NDS*, std::map<u64, bool>> logged;
    if (!cfg.Checked)
    {
        std::lock_guard<std::mutex> configLock(NSMLTraceConfigMutex);
        if (!cfg.Checked)
        {
            cfg.Enabled = NSMLEnvFlag("MELONDS_NSML_TRACE_PLAYER_RENDER");
            if (const char* startFrame = getenv("MELONDS_NSML_TRACE_PLAYER_RENDER_START_FRAME"))
                cfg.StartFrame = static_cast<u32>(strtoul(startFrame, nullptr, 0));
            if (const char* endFrame = getenv("MELONDS_NSML_TRACE_PLAYER_RENDER_END_FRAME"))
                cfg.EndFrame = static_cast<u32>(strtoul(endFrame, nullptr, 0));
            if (cfg.EndFrame == 0)
                cfg.EndFrame = 0xFFFFFFFF;
            cfg.Checked = true;
        }
    }

    if (!cfg.Enabled || !cpu || cpu->Num != 0)
        return;
    if (instrAddr != 0x020FCACC && instrAddr != 0x020FCF6C && instrAddr != 0x020FD00C)
        return;
    const u32 frame = cpu->NDS.NumFrames;
    if (frame < cfg.StartFrame || frame > cfg.EndFrame)
        return;
    if (!IsNSMLMarioVsLuigiGameplay(cpu->NDS))
        return;

    const u32 actor = cpu->R[0];
    if (!IsNSMLMainRAMAddress(actor))
        return;
    const u64 key = (static_cast<u64>(frame) << 32) ^ (static_cast<u64>(instrAddr) << 8) ^ actor;
    {
        std::lock_guard<std::mutex> outputLock(NSMLTraceOutputMutex);
        if (logged[&cpu->NDS][key])
            return;
        logged[&cpu->NDS][key] = true;
        if (!header)
        {
            std::printf("NSMB PlayerRender header: frame,pc,lr,r1,actor,objectID,playerID,characterID,visibleFlag,x,y,z,baseAction,subAction,physics,transition,collision,environment,localPlayerID,displayCameraX,displayVec,displayVecAux,modelBase0,modelBase1,modelBase2,modelBase3\n");
            header = true;
        }
        std::printf(
            "NSMB PlayerRender: %u,%08X,%08X,%08X,%08X,%04X,%u,%u,%u,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%u,%08X,%08X/%08X/%08X,%08X/%08X/%08X,%08X,%08X,%08X,%08X\n",
            frame,
            instrAddr,
            cpu->R[14],
            cpu->R[1],
            actor,
            cpu->NDS.ARM9Read16(actor + 0x0C),
            static_cast<u32>(ReadNSMLTraceByte(cpu, actor + 0x7B4)),
            static_cast<u32>(cpu->NDS.ARM9Read16(actor + 0x7AA)),
            static_cast<u32>(ReadNSMLTraceByte(cpu, actor + 0x7B5)),
            cpu->NDS.ARM9Read32(actor + 0x60),
            cpu->NDS.ARM9Read32(actor + 0x64),
            cpu->NDS.ARM9Read32(actor + 0x68),
            cpu->NDS.ARM9Read32(actor + 0x778),
            cpu->NDS.ARM9Read32(actor + 0x77C),
            cpu->NDS.ARM9Read32(actor + 0x780),
            cpu->NDS.ARM9Read32(actor + 0x784),
            cpu->NDS.ARM9Read32(actor + 0x788),
            cpu->NDS.ARM9Read32(actor + 0x790),
            cpu->NDS.ARM9Read32(0x02085A7C),
            cpu->NDS.ARM9Read32(0x02085AB4),
            cpu->NDS.ARM9Read32(0x0212AFC4),
            cpu->NDS.ARM9Read32(0x0212AFC8),
            cpu->NDS.ARM9Read32(0x0212AFCC),
            cpu->NDS.ARM9Read32(0x0212AFD4),
            cpu->NDS.ARM9Read32(0x0212AFD8),
            cpu->NDS.ARM9Read32(0x0212AFDC),
            cpu->NDS.ARM9Read32(actor + 0x2C4),
            cpu->NDS.ARM9Read32(actor + 0x2C8),
            cpu->NDS.ARM9Read32(actor + 0x2CC),
            cpu->NDS.ARM9Read32(actor + 0x2D0));
        std::fflush(stdout);
    }
}

bool TraceNSMLRandomCallFromJIT(ARM* cpu, u32 instrAddr, u32 lr)
{
    return TraceNSMLRandomCallImpl(cpu, instrAddr, lr, true);
}

bool TraceNSMLPlayerLifeCallFromJIT(ARM* cpu, u32 targetAddr, u32 lr)
{
    static bool checked = false;
    static bool enabled = false;
    static bool header = false;
    if (!checked)
    {
        enabled = NSMLEnvFlag("MELONDS_NSML_TRACE_PLAYER_LIFE_CALLS");
        checked = true;
    }
    if (!enabled || !cpu || cpu->Num != 0)
        return false;

    const u32 player = cpu->R[0];
    const u32 value = cpu->R[1];
    const u32 lives0 = cpu->NDS.ARM9Read32(0x0208B364);
    const u32 lives1 = cpu->NDS.ARM9Read32(0x0208B368);
    const u32 deaths0 = cpu->NDS.ARM9Read32(0x0208B394);
    const u32 deaths1 = cpu->NDS.ARM9Read32(0x0208B398);
    const char* name = "unknown";
    switch (targetAddr)
    {
    case 0x0202048C: name = "Game::addPlayerDeath"; break;
    case 0x020204D0: name = "Game::setPlayerDeaths"; break;
    case 0x020204E0: name = "Game::losePlayerLife"; break;
    case 0x02020580: name = "Game::setPlayerLives"; break;
    default: break;
    }
    if (!header)
    {
        std::printf("NSMB LifeCall header: frame,target,name,caller,player,value,lives0,lives1,deaths0,deaths1,stageGroup,vsMode,localPlayerID\n");
        header = true;
    }
    std::printf(
        "NSMB LifeCall: %u,%08X,%s,%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        cpu->NDS.NumFrames,
        targetAddr,
        name,
        lr,
        player,
        value,
        lives0,
        lives1,
        deaths0,
        deaths1,
        cpu->NDS.ARM9Read32(0x02085A18),
        cpu->NDS.ARM9Read32(0x02085A84),
        cpu->NDS.ARM9Read32(0x02085A7C));
    return true;
}

static void TraceNSMLPlayerDefeatedCall(ARM* cpu, u32 targetAddr, u32 lr)
{
    static bool checked = false;
    static bool enabled = false;
    if (!checked)
    {
        enabled = NSMLEnvFlag("MELONDS_NSML_TRACE_PLAYER_DEFEATED");
        checked = true;
    }
    if (!enabled || !cpu || cpu->Num != 0 || targetAddr != 0x0212B2BC)
        return;

    const u32 actor = cpu->R[0];
    u32 player = 0xFFFFFFFF;
    u32 x = 0;
    u32 y = 0;
    u32 flags = 0;
    u32 defeatedFlag = 0;
    u32 linkedActor = 0;
    if (IsNSMLMainRAMAddress(actor))
    {
        player = static_cast<u32>(static_cast<s8>(ReadNSMLTraceByte(cpu, actor + 0x11E)));
        x = cpu->NDS.ARM9Read32(actor + 0x60);
        y = cpu->NDS.ARM9Read32(actor + 0x64);
        flags = cpu->NDS.ARM9Read32(actor + 0x18);
        defeatedFlag = ReadNSMLTraceByte(cpu, actor + 0x7B3);
        linkedActor = cpu->NDS.ARM9Read32(actor + 0x688);
    }
    std::printf(
        "NSMB DefeatedCall: frame=%u caller=%08X actor=%08X player=%u x=%08X y=%08X flags=%08X defeatedFlag=%u linkedActor=%08X lives=%u/%u deaths=%u/%u trans=%u/%u\n",
        cpu->NDS.NumFrames,
        lr,
        actor,
        player,
        x,
        y,
        flags,
        defeatedFlag,
        linkedActor,
        cpu->NDS.ARM9Read32(0x0208B364),
        cpu->NDS.ARM9Read32(0x0208B368),
        cpu->NDS.ARM9Read32(0x0208B394),
        cpu->NDS.ARM9Read32(0x0208B398),
        cpu->NDS.ARM9Read32(0x0208B354),
        cpu->NDS.ARM9Read32(0x0208B358));
}

bool TraceNSMLBranchRegFromJIT(ARM* cpu, u32 targetAddr, u32 lr)
{
    TraceNSMLPlayerDefeatedCall(cpu, targetAddr, lr);
    return false;
}

static void TraceNSMLPlayerDefeatedEntry(ARM* cpu, u32 instrAddr)
{
    TraceNSMLPlayerDefeatedCall(cpu, instrAddr, cpu ? cpu->R[14] : 0);
}

#ifdef GDBSTUB_ENABLED
void ARM::GdbCheckA()
{
    if (!IsSingleStep && !BreakReq)
    { // check if eg. break signal is incoming etc.
        Gdb::StubState st = GdbStub.Enter(false, Gdb::TgtStatus::NoEvent, ~(u32)0u, BreakOnStartup);
        BreakOnStartup = false;
        IsSingleStep = st == Gdb::StubState::Step;
        BreakReq = st == Gdb::StubState::Attach || st == Gdb::StubState::Break;
    }
}
void ARM::GdbCheckB()
{
    if (IsSingleStep || BreakReq)
    { // use else here or we single-step the same insn twice in gdb
        u32 pc_real = R[15] - ((CPSR & 0x20) ? 2 : 4);
        Gdb::StubState st = GdbStub.Enter(true, Gdb::TgtStatus::SingleStep, pc_real);
        IsSingleStep = st == Gdb::StubState::Step;
        BreakReq = st == Gdb::StubState::Attach || st == Gdb::StubState::Break;
    }
}
void ARM::GdbCheckC()
{
    u32 pc_real = R[15] - ((CPSR & 0x20) ? 2 : 4);
    Gdb::StubState st = GdbStub.CheckBkpt(pc_real, true, true);
    if (st != Gdb::StubState::CheckNoHit)
    {
        IsSingleStep = st == Gdb::StubState::Step;
        BreakReq = st == Gdb::StubState::Attach || st == Gdb::StubState::Break;
    }
    else GdbCheckB();
}
#else
void ARM::GdbCheckA() {}
void ARM::GdbCheckB() {}
void ARM::GdbCheckC() {}
#endif


// instruction timing notes
//
// * simple instruction: 1S (code)
// * LDR: 1N+1N+1I (code/data/internal)
// * STR: 1N+1N (code/data)
// * LDM: 1N+1N+(n-1)S+1I
// * STM: 1N+1N+(n-1)S
// * MUL/etc: 1N+xI (code/internal)
// * branch: 1N+1S (code/code) (pipeline refill)
//
// MUL/MLA seems to take 1I on ARM9



const u32 ARM::ConditionTable[16] =
{
    0xF0F0, // EQ
    0x0F0F, // NE
    0xCCCC, // CS
    0x3333, // CC
    0xFF00, // MI
    0x00FF, // PL
    0xAAAA, // VS
    0x5555, // VC
    0x0C0C, // HI
    0xF3F3, // LS
    0xAA55, // GE
    0x55AA, // LT
    0x0A05, // GT
    0xF5FA, // LE
    0xFFFF, // AL
    0x0000  // NE
};

ARM::ARM(u32 num, bool jit, std::optional<GDBArgs> gdb, melonDS::NDS& nds) :
#ifdef GDBSTUB_ENABLED
    GdbStub(this),
    BreakOnStartup(false),
#endif
    Num(num), // well uh
    NDS(nds)
{
    SetGdbArgs(jit ? std::nullopt : gdb);
}

ARM::~ARM()
{
    // dorp
}

ARMv5::ARMv5(melonDS::NDS& nds, std::optional<GDBArgs> gdb, bool jit) : ARM(0, jit, gdb, nds)
{
    DTCM = NDS.JIT.Memory.GetARM9DTCM();

    PU_Map = PU_PrivMap;
}

ARMv4::ARMv4(melonDS::NDS& nds, std::optional<GDBArgs> gdb, bool jit) : ARM(1, jit, gdb, nds)
{
    //
}

ARMv5::~ARMv5()
{
    // DTCM is owned by Memory, not going to delete it
}

void ARM::SetGdbArgs(std::optional<GDBArgs> gdb)
{
#ifdef GDBSTUB_ENABLED
    GdbStub.Close();
    if (gdb)
    {
        int port = Num ? gdb->PortARM7 : gdb->PortARM9;
        GdbStub.Init(port);
        BreakOnStartup = Num ? gdb->ARM7BreakOnStartup : gdb->ARM9BreakOnStartup;
    }
    IsSingleStep = false;
#endif
}

void ARM::Reset()
{
    Cycles = 0;
    Halted = 0;

    IRQ = 0;

    for (int i = 0; i < 16; i++)
        R[i] = 0;

    CPSR = 0x000000D3;

    for (int i = 0; i < 7; i++)
        R_FIQ[i] = 0;
    for (int i = 0; i < 2; i++)
    {
        R_SVC[i] = 0;
        R_ABT[i] = 0;
        R_IRQ[i] = 0;
        R_UND[i] = 0;
    }

    R_FIQ[7] = 0x00000010;
    R_SVC[2] = 0x00000010;
    R_ABT[2] = 0x00000010;
    R_IRQ[2] = 0x00000010;
    R_UND[2] = 0x00000010;

    ExceptionBase = Num ? 0x00000000 : 0xFFFF0000;

    CodeMem.Mem = NULL;

#ifdef JIT_ENABLED
    FastBlockLookup = NULL;
    FastBlockLookupStart = 0;
    FastBlockLookupSize = 0;
    JitCodeBase = reinterpret_cast<u8*>(NDS.JIT.JITCompiler.AddEntryOffset(0));
#endif

#ifdef GDBSTUB_ENABLED
    IsSingleStep = false;
    BreakReq = false;
#endif

    // zorp
    JumpTo(ExceptionBase);
}

void ARMv5::Reset()
{
    PU_Map = PU_PrivMap;

    ARM::Reset();
}


void ARM::DoSavestate(Savestate* file)
{
    file->Section((char*)(Num ? "ARM7" : "ARM9"));

    file->Var32((u32*)&Cycles);
    //file->Var32((u32*)&CyclesToRun);

    // hack to make save states compatible
    u32 halted = Halted;
    file->Var32(&halted);
    Halted = halted;

    file->VarArray(R, 16*sizeof(u32));
    file->Var32(&CPSR);
    file->VarArray(R_FIQ, 8*sizeof(u32));
    file->VarArray(R_SVC, 3*sizeof(u32));
    file->VarArray(R_ABT, 3*sizeof(u32));
    file->VarArray(R_IRQ, 3*sizeof(u32));
    file->VarArray(R_UND, 3*sizeof(u32));
    file->Var32(&CurInstr);
#ifdef JIT_ENABLED
    if (file->Saving && NDS.IsJITEnabled())
    {
        // hack, the JIT doesn't really pipeline
        // but we still want JIT save states to be
        // loaded while running the interpreter
        FillPipeline();
    }
#endif
    file->VarArray(NextInstr, 2*sizeof(u32));

    file->Var32(&ExceptionBase);

    if (!file->Saving)
    {
        CPSR |= 0x00000010;
        R_FIQ[7] |= 0x00000010;
        R_SVC[2] |= 0x00000010;
        R_ABT[2] |= 0x00000010;
        R_IRQ[2] |= 0x00000010;
        R_UND[2] |= 0x00000010;

        if (!Num)
        {
            SetupCodeMem(R[15]); // should fix it
            ((ARMv5*)this)->RegionCodeCycles = ((ARMv5*)this)->MemTimings[R[15] >> 12][0];

            if ((CPSR & 0x1F) == 0x10)
                ((ARMv5*)this)->PU_Map = ((ARMv5*)this)->PU_UserMap;
            else
                ((ARMv5*)this)->PU_Map = ((ARMv5*)this)->PU_PrivMap;
        }
        else
        {
            CodeRegion = R[15] >> 24;
            CodeCycles = R[15] >> 15; // cheato
        }
    }
}

void ARMv5::DoSavestate(Savestate* file)
{
    ARM::DoSavestate(file);
    CP15DoSavestate(file);
}


void ARM::SetupCodeMem(u32 addr)
{
    if (!Num)
    {
        ((ARMv5*)this)->GetCodeMemRegion(addr, &CodeMem);
    }
    else
    {
        // not sure it's worth it for the ARM7
        // esp. as everything there generally runs on WRAM
        // and due to how it's mapped, we can't use this optimization
        //NDS::ARM7GetMemRegion(addr, false, &CodeMem);
    }
}

void ARMv5::JumpTo(u32 addr, bool restorecpsr)
{
    const bool traceBadJump = Num == 0 && NSMLBadJumpTraceEnabled();
    u32 sourcePC = 0;
    u32 sourceLR = 0;
    u32 sourceSP = 0;
    u32 sourceCPSR = 0;
    if (traceBadJump)
    {
        sourcePC = R[15];
        sourceLR = R[14];
        sourceSP = R[13];
        sourceCPSR = CPSR;
    }
    if (restorecpsr)
    {
        RestoreCPSR();

        if (CPSR & 0x20)    addr |= 0x1;
        else                addr &= ~0x1;
    }

    // aging cart debug crap
    //if (addr == 0x0201764C) printf("capture test %d: R1=%08X\n", R[6], R[1]);
    //if (addr == 0x020175D8) printf("capture test %d: res=%08X\n", R[6], R[0]);

    u32 oldregion = R[15] >> 24;
    u32 newregion = addr >> 24;

    if (traceBadJump)
    {
        const u32 targetBase = addr & ((addr & 0x1) ? ~0x1u : ~0x3u);
        if (!(PU_Map[targetBase >> 12] & 0x04))
        {
            Log(LogLevel::Warn,
                "NSMB BadJump: frame=%u from=%08X target=%08X lr=%08X sp=%08X cpsr=%08X instr=%08X "
                "r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X restore=%d\n",
                NDS.NumFrames,
                sourcePC,
                addr,
                sourceLR,
                sourceSP,
                sourceCPSR,
                CurInstr,
                R[0],
                R[1],
                R[2],
                R[3],
                R[4],
                R[5],
                R[6],
                R[7],
                R[8],
                R[9],
                R[10],
                R[11],
                R[12],
                restorecpsr ? 1 : 0);
            fputs("NSMB BadJump from_dump=", stdout);
            WriteNSMLHexDump(stdout, this, sourcePC >= 512 ? sourcePC - 512 : sourcePC, 768);
            fputs(" stack_dump=", stdout);
            WriteNSMLHexDump(stdout, this, sourceSP >= 128 ? sourceSP - 128 : sourceSP, 256);
            fputc('\n', stdout);
            fflush(stdout);
        }
    }

    RegionCodeCycles = MemTimings[addr >> 12][0];

    if (addr & 0x1)
    {
        addr &= ~0x1;
        R[15] = addr+2;

        if (newregion != oldregion) SetupCodeMem(addr);

        // two-opcodes-at-once fetch
        // doesn't matter if we put garbage in the MSbs there
        if (addr & 0x2)
        {
            NextInstr[0] = CodeRead32(addr-2, true) >> 16;
            Cycles += CodeCycles;
            NextInstr[1] = CodeRead32(addr+2, false);
            Cycles += CodeCycles;
        }
        else
        {
            NextInstr[0] = CodeRead32(addr, true);
            NextInstr[1] = NextInstr[0] >> 16;
            Cycles += CodeCycles;
        }

        CPSR |= 0x20;
    }
    else
    {
        addr &= ~0x3;
        R[15] = addr+4;

        if (newregion != oldregion) SetupCodeMem(addr);

        NextInstr[0] = CodeRead32(addr, true);
        Cycles += CodeCycles;
        NextInstr[1] = CodeRead32(addr+4, false);
        Cycles += CodeCycles;

        CPSR &= ~0x20;
    }

    if (!(PU_Map[addr>>12] & 0x04))
    {
        PrefetchAbort();
        return;
    }

    NDS.MonitorARM9Jump(addr);
}

void ARMv4::JumpTo(u32 addr, bool restorecpsr)
{
    if (restorecpsr)
    {
        RestoreCPSR();

        if (CPSR & 0x20)    addr |= 0x1;
        else                addr &= ~0x1;
    }

    CodeRegion = addr >> 24;
    CodeCycles = addr >> 15; // cheato

    if (addr & 0x1)
    {
        addr &= ~0x1;
        R[15] = addr+2;

        //if (newregion != oldregion) SetupCodeMem(addr);

        NextInstr[0] = CodeRead16(addr);
        NextInstr[1] = CodeRead16(addr+2);
        Cycles += NDS.ARM7MemTimings[CodeCycles][0] + NDS.ARM7MemTimings[CodeCycles][1];

        CPSR |= 0x20;
    }
    else
    {
        addr &= ~0x3;
        R[15] = addr+4;

        //if (newregion != oldregion) SetupCodeMem(addr);

        NextInstr[0] = CodeRead32(addr);
        NextInstr[1] = CodeRead32(addr+4);
        Cycles += NDS.ARM7MemTimings[CodeCycles][2] + NDS.ARM7MemTimings[CodeCycles][3];

        CPSR &= ~0x20;
    }
}

void ARM::RestoreCPSR()
{
    u32 oldcpsr = CPSR;

    switch (CPSR & 0x1F)
    {
    case 0x11:
        CPSR = R_FIQ[7];
        break;

    case 0x12:
        CPSR = R_IRQ[2];
        break;

    case 0x13:
        CPSR = R_SVC[2];
        break;

    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
        CPSR = R_ABT[2];
        break;

    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
        CPSR = R_UND[2];
        break;

    default:
        Log(LogLevel::Warn, "!! attempt to restore CPSR under bad mode %02X, %08X\n", CPSR&0x1F, R[15]);
        break;
    }

    CPSR |= 0x00000010;

    UpdateMode(oldcpsr, CPSR);
}

void ARM::UpdateMode(u32 oldmode, u32 newmode, bool phony)
{
    if ((oldmode & 0x1F) == (newmode & 0x1F)) return;

    switch (oldmode & 0x1F)
    {
    case 0x11:
        std::swap(R[8], R_FIQ[0]);
        std::swap(R[9], R_FIQ[1]);
        std::swap(R[10], R_FIQ[2]);
        std::swap(R[11], R_FIQ[3]);
        std::swap(R[12], R_FIQ[4]);
        std::swap(R[13], R_FIQ[5]);
        std::swap(R[14], R_FIQ[6]);
        break;

    case 0x12:
        std::swap(R[13], R_IRQ[0]);
        std::swap(R[14], R_IRQ[1]);
        break;

    case 0x13:
        std::swap(R[13], R_SVC[0]);
        std::swap(R[14], R_SVC[1]);
        break;

    case 0x17:
        std::swap(R[13], R_ABT[0]);
        std::swap(R[14], R_ABT[1]);
        break;

    case 0x1B:
        std::swap(R[13], R_UND[0]);
        std::swap(R[14], R_UND[1]);
        break;
    }

    switch (newmode & 0x1F)
    {
    case 0x11:
        std::swap(R[8], R_FIQ[0]);
        std::swap(R[9], R_FIQ[1]);
        std::swap(R[10], R_FIQ[2]);
        std::swap(R[11], R_FIQ[3]);
        std::swap(R[12], R_FIQ[4]);
        std::swap(R[13], R_FIQ[5]);
        std::swap(R[14], R_FIQ[6]);
        break;

    case 0x12:
        std::swap(R[13], R_IRQ[0]);
        std::swap(R[14], R_IRQ[1]);
        break;

    case 0x13:
        std::swap(R[13], R_SVC[0]);
        std::swap(R[14], R_SVC[1]);
        break;

    case 0x17:
        std::swap(R[13], R_ABT[0]);
        std::swap(R[14], R_ABT[1]);
        break;

    case 0x1B:
        std::swap(R[13], R_UND[0]);
        std::swap(R[14], R_UND[1]);
        break;
    }

    if ((!phony) && (Num == 0))
    {
        if ((newmode & 0x1F) == 0x10)
            ((ARMv5*)this)->PU_Map = ((ARMv5*)this)->PU_UserMap;
        else
            ((ARMv5*)this)->PU_Map = ((ARMv5*)this)->PU_PrivMap;
    }
}

void ARM::TriggerIRQ()
{
    if (CPSR & 0x80)
        return;

    u32 oldcpsr = CPSR;
    CPSR &= ~0xFF;
    CPSR |= 0xD2;
    UpdateMode(oldcpsr, CPSR);

    R_IRQ[2] = oldcpsr;
    R[14] = R[15] + (oldcpsr & 0x20 ? 2 : 0);
    JumpTo(ExceptionBase + 0x18);

    // ARDS cheat support
    // normally, those work by hijacking the ARM7 VBlank handler
    if (Num == 1)
    {
        if ((NDS.IF[1] & NDS.IE[1]) & (1<<IRQ_VBlank))
            NDS.AREngine.RunCheats();
    }
}

void ARMv5::PrefetchAbort()
{
    Log(LogLevel::Warn,
        "ARM%d: prefetch abort (frame=%u pc=%08X lr=%08X sp=%08X cpsr=%08X "
        "r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X "
        "r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X)\n",
        Num == 1 ? 7 : 9,
        NDS.NumFrames,
        R[15],
        R[14],
        R[13],
        CPSR,
        R[0],
        R[1],
        R[2],
        R[3],
        R[4],
        R[5],
        R[6],
        R[7],
        R[8],
        R[9],
        R[10],
        R[11],
        R[12]);

    u32 oldcpsr = CPSR;
    CPSR &= ~0xBF;
    CPSR |= 0x97;
    UpdateMode(oldcpsr, CPSR);

    // this shouldn't happen, but if it does, we're stuck in some nasty endless loop
    // so better take care of it
    if (!(PU_Map[ExceptionBase>>12] & 0x04))
    {
        Log(LogLevel::Error, "!!!!! EXCEPTION REGION NOT EXECUTABLE. THIS IS VERY BAD!!\n");
        NDS.Stop(Platform::StopReason::BadExceptionRegion);
        return;
    }

    R_ABT[2] = oldcpsr;
    R[14] = R[15] + (oldcpsr & 0x20 ? 2 : 0);
    JumpTo(ExceptionBase + 0x0C);
}

void ARMv5::DataAbort()
{
    Log(LogLevel::Warn,
        "ARM%d: data abort (frame=%u pc=%08X lr=%08X sp=%08X cpsr=%08X instr=%08X fault=%08X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X)\n",
        Num == 1 ? 7 : 9,
        NDS.NumFrames,
        R[15],
        R[14],
        R[13],
        CPSR,
        CurInstr,
        DataRegion,
        R[0],
        R[1],
        R[2],
        R[3],
        R[4],
        R[5],
        R[6],
        R[7],
        R[8],
        R[9],
        R[10],
        R[11],
        R[12]);
    if (Num == 0)
    {
        Log(LogLevel::Warn,
            "ARM9: abort stack frame=%u sp=%08X [%08X %08X %08X %08X %08X %08X %08X %08X]\n",
            NDS.NumFrames,
            R[13],
            ReadNSMLTrace32(this, R[13] + 0x00),
            ReadNSMLTrace32(this, R[13] + 0x04),
            ReadNSMLTrace32(this, R[13] + 0x08),
            ReadNSMLTrace32(this, R[13] + 0x0C),
            ReadNSMLTrace32(this, R[13] + 0x10),
            ReadNSMLTrace32(this, R[13] + 0x14),
            ReadNSMLTrace32(this, R[13] + 0x18),
            ReadNSMLTrace32(this, R[13] + 0x1C));
        Log(LogLevel::Warn,
            "ARM9: abort refs frame=%u r9=%08X [%08X %08X %08X %08X] r10=%08X [%08X %08X %08X %08X]\n",
            NDS.NumFrames,
            R[9],
            ReadNSMLTrace32(this, R[9] + 0x00),
            ReadNSMLTrace32(this, R[9] + 0x04),
            ReadNSMLTrace32(this, R[9] + 0x08),
            ReadNSMLTrace32(this, R[9] + 0x0C),
            R[10],
            ReadNSMLTrace32(this, R[10] + 0x00),
            ReadNSMLTrace32(this, R[10] + 0x04),
            ReadNSMLTrace32(this, R[10] + 0x08),
            ReadNSMLTrace32(this, R[10] + 0x0C));
        Log(LogLevel::Warn,
            "ARM9: abort args frame=%u r1=%08X [%08X %08X %08X %08X %08X %08X %08X %08X] r1+20 [%08X %08X %08X %08X %08X %08X %08X %08X]\n",
            NDS.NumFrames,
            R[1],
            ReadNSMLTrace32(this, R[1] + 0x00),
            ReadNSMLTrace32(this, R[1] + 0x04),
            ReadNSMLTrace32(this, R[1] + 0x08),
            ReadNSMLTrace32(this, R[1] + 0x0C),
            ReadNSMLTrace32(this, R[1] + 0x10),
            ReadNSMLTrace32(this, R[1] + 0x14),
            ReadNSMLTrace32(this, R[1] + 0x18),
            ReadNSMLTrace32(this, R[1] + 0x1C),
            ReadNSMLTrace32(this, R[1] + 0x20),
            ReadNSMLTrace32(this, R[1] + 0x24),
            ReadNSMLTrace32(this, R[1] + 0x28),
            ReadNSMLTrace32(this, R[1] + 0x2C),
            ReadNSMLTrace32(this, R[1] + 0x30),
            ReadNSMLTrace32(this, R[1] + 0x34),
            ReadNSMLTrace32(this, R[1] + 0x38),
            ReadNSMLTrace32(this, R[1] + 0x3C));
        Log(LogLevel::Warn,
            "ARM9: abort tcm frame=%u itcmSize=%08X itcmSetting=%08X dtcmBase=%08X dtcmMask=%08X dtcmSetting=%08X\n",
            NDS.NumFrames,
            ITCMSize,
            ITCMSetting,
            DTCMBase,
            DTCMMask,
            DTCMSetting);
        Log(LogLevel::Warn,
            "ARM9: abort code frame=%u pc=%08X [%08X %08X %08X %08X %08X %08X %08X %08X]\n",
            NDS.NumFrames,
            R[15],
            ReadNSMLTrace32(this, R[15] - 0x10),
            ReadNSMLTrace32(this, R[15] - 0x0C),
            ReadNSMLTrace32(this, R[15] - 0x08),
            ReadNSMLTrace32(this, R[15] - 0x04),
            ReadNSMLTrace32(this, R[15] + 0x00),
            ReadNSMLTrace32(this, R[15] + 0x04),
            ReadNSMLTrace32(this, R[15] + 0x08),
            ReadNSMLTrace32(this, R[15] + 0x0C));
        Log(LogLevel::Warn,
            "ARM9: abort updateHintVec frame=%u [%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X]\n",
            NDS.NumFrames,
            ReadNSMLTrace32(this, 0x01FF8BE0),
            ReadNSMLTrace32(this, 0x01FF8BE4),
            ReadNSMLTrace32(this, 0x01FF8BE8),
            ReadNSMLTrace32(this, 0x01FF8BEC),
            ReadNSMLTrace32(this, 0x01FF8BF0),
            ReadNSMLTrace32(this, 0x01FF8BF4),
            ReadNSMLTrace32(this, 0x01FF8BF8),
            ReadNSMLTrace32(this, 0x01FF8BFC),
            ReadNSMLTrace32(this, 0x01FF8C00),
            ReadNSMLTrace32(this, 0x01FF8C04),
            ReadNSMLTrace32(this, 0x01FF8C08),
            ReadNSMLTrace32(this, 0x01FF8C0C),
            ReadNSMLTrace32(this, 0x01FF8C10),
            ReadNSMLTrace32(this, 0x01FF8C14),
            ReadNSMLTrace32(this, 0x01FF8C18),
            ReadNSMLTrace32(this, 0x01FF8C1C));
    }

    u32 oldcpsr = CPSR;
    CPSR &= ~0xBF;
    CPSR |= 0x97;
    UpdateMode(oldcpsr, CPSR);

    R_ABT[2] = oldcpsr;
    R[14] = R[15] + (oldcpsr & 0x20 ? 4 : 0);
    JumpTo(ExceptionBase + 0x10);
}

void ARM::CheckGdbIncoming()
{
    GdbCheckA();
}

#ifdef JIT_ENABLED
extern "C" const u8 ARM_JitExactBlockChainEnabled = [] {
    const bool allowRomProbe = NSMLEnvFlag("MELONDS_NSML_JIT_EXACT_BLOCK_CHAIN_ALLOW_ROM_PROBE")
        && (NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE_FRAME_BOUNDARY") ||
            NSMLEnvFlag("MELONDS_NSML_ROM_GAME_TICK_PROBE_GAME_RAM_ROLLBACK"));
    return NSMLEnvFlag("MELONDS_NSML_JIT_EXACT_BLOCK_CHAIN")
        && (!NSMLRuntimeHooksMaybeEnabled() || allowRomProbe) ? u8{1} : u8{0};
}();

#if defined(__x86_64__)
static_assert(offsetof(ARM, Num) == ARM_Num_offset);
static_assert(offsetof(ARM, R[15]) == ARM_R15_offset);
static_assert(offsetof(ARM, FastBlockLookupStart) == ARM_FastBlockLookupStart_offset);
static_assert(offsetof(ARM, FastBlockLookupSize) == ARM_FastBlockLookupSize_offset);
static_assert(offsetof(ARM, FastBlockLookup) == ARM_FastBlockLookup_offset);
static_assert(offsetof(ARM, JitCodeBase) == ARM_JitCodeBase_offset);
static_assert(offsetof(ARM, NDS) == ARM_NDS_offset);
static_assert(offsetof(NDS, ARM9Timestamp) == NDS_ARM9Timestamp_offset);
static_assert(offsetof(NDS, ARM9Target) == NDS_ARM9Target_offset);
#endif
#endif

template <CPUExecuteMode mode>
void ARMv5::Execute()
{
    if constexpr (mode == CPUExecuteMode::InterpreterGDB)
        GdbCheckB();

    if (Halted)
    {
        if (Halted == 2)
        {
            Halted = 0;
        }
        else if (NDS.HaltInterrupted(0))
        {
            Halted = 0;
            if (NDS.IME[0] & 0x1)
                TriggerIRQ();
        }
        else
        {
            NDS.ARM9Timestamp = NDS.ARM9Target;
            return;
        }
    }

    const bool nsmlRuntimeHooksEnabled = NSMLRuntimeHooksMaybeEnabled();
    while (NDS.ARM9Timestamp < NDS.ARM9Target)
    {
#ifdef JIT_ENABLED
        if constexpr (mode == CPUExecuteMode::JIT)
        {
            u32 instrAddr = R[15] - ((CPSR&0x20)?2:4);
            if (nsmlRuntimeHooksEnabled)
            {
                HandleNSMLRomGameTickProbe(this, instrAddr);
                if (HandleNSMLGameTickProbe(this, instrAddr))
                {
                    NDS.ARM9Timestamp++;
                    continue;
                }
                HandleNSMLNetReadyHotPatch(this, instrAddr);
                TraceNSMLPacketCapture(this, instrAddr);
                if (HandleNSMLLowerMPBridge(this, instrAddr))
                {
                    NDS.ARM9Timestamp++;
                    continue;
                }
                PatchNSMLPlayerModelRenderPtrs(this, instrAddr);
                PatchNSMLDynamicCameraLead(this, instrAddr);
                PatchNSMLRenderCameraAlias(this, instrAddr);
                PatchNSMLCameraFocusLoopCount(this, instrAddr);
                TraceNSMLStageCamera(this, instrAddr);
                TraceNSMLPlayerDefeatedEntry(this, instrAddr);
                TraceNSMLCallImpl(this, instrAddr);
                TraceNSMLRandomCall(this, instrAddr);
            }

            if ((instrAddr < FastBlockLookupStart || instrAddr >= (FastBlockLookupStart + FastBlockLookupSize))
                && !NDS.JIT.SetupExecutableRegion(0, instrAddr, FastBlockLookup, FastBlockLookupStart, FastBlockLookupSize))
            {
                NDS.ARM9Timestamp = NDS.ARM9Target;
                Log(LogLevel::Error, "ARMv5 PC in non executable region %08X\n", R[15]);
                return;
            }

            JitBlockEntry block = NDS.JIT.LookUpBlock(0, FastBlockLookup,
                instrAddr - FastBlockLookupStart, instrAddr);
            if (block)
                ARM_Dispatch(this, block);
            else
                NDS.JIT.CompileBlock(this);

            if (StopExecution)
            {
                // this order is crucial otherwise idle loops waiting for an IRQ won't function
                if (IRQ)
                    TriggerIRQ();

                if (Halted || IdleLoop)
                {
                    if ((Halted == 1 || IdleLoop) && NDS.ARM9Timestamp < NDS.ARM9Target)
                    {
                        Cycles = 0;
                        NDS.ARM9Timestamp = NDS.ARM9Target;
                    }
                    IdleLoop = 0;
                    break;
                }
            }
        }
        else
#endif
        {
            if (CPSR & 0x20) // THUMB
            {
                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();
                const u32 instrAddr = R[15] - 2;
                if (nsmlRuntimeHooksEnabled)
                {
                    HandleNSMLRomGameTickProbe(this, instrAddr);
                    if (HandleNSMLGameTickProbe(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    HandleNSMLNetReadyHotPatch(this, instrAddr);
                    TraceNSMLPacketCapture(this, instrAddr);
                    if (HandleNSMLLowerMPBridge(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    if (HandleNSMLTransferPacketBypass(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    if (HandleNSMLNetDisconnectBypass(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    if (HandleNSMLPacketReplay(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    PatchNSMLPlayerModelRenderPtrs(this, instrAddr);
                    PatchNSMLDynamicCameraLead(this, instrAddr);
                    PatchNSMLRenderCameraAlias(this, instrAddr);
                    PatchNSMLCameraFocusLoopCount(this, instrAddr);
                    TraceNSMLStageCamera(this, instrAddr);
                    TraceNSMLPlayerRender(this, instrAddr);
                    TraceNSMLPlayerDefeatedEntry(this, instrAddr);
                    TraceNSMLCallImpl(this, instrAddr);
                    TraceNSMLRandomCall(this, instrAddr);
                }

                // prefetch
                R[15] += 2;
                CurInstr = NextInstr[0];
                NextInstr[0] = NextInstr[1];
                if (R[15] & 0x2) { NextInstr[1] >>= 16; CodeCycles = 0; }
                else             NextInstr[1] = CodeRead32(R[15], false);

                // actually execute
                u32 icode = (CurInstr >> 6) & 0x3FF;
                ARMInterpreter::THUMBInstrTable[icode](this);
            }
            else
            {
                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();
                const u32 instrAddr = R[15] - 4;
                if (nsmlRuntimeHooksEnabled)
                {
                    HandleNSMLRomGameTickProbe(this, instrAddr);
                    if (HandleNSMLGameTickProbe(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    HandleNSMLNetReadyHotPatch(this, instrAddr);
                    TraceNSMLPacketCapture(this, instrAddr);
                    if (HandleNSMLLowerMPBridge(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    if (HandleNSMLTransferPacketBypass(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    if (HandleNSMLNetDisconnectBypass(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    if (HandleNSMLPacketReplay(this, instrAddr))
                    {
                        NDS.ARM9Timestamp++;
                        continue;
                    }
                    PatchNSMLPlayerModelRenderPtrs(this, instrAddr);
                    PatchNSMLDynamicCameraLead(this, instrAddr);
                    PatchNSMLRenderCameraAlias(this, instrAddr);
                    PatchNSMLCameraFocusLoopCount(this, instrAddr);
                    TraceNSMLStageCamera(this, instrAddr);
                    TraceNSMLPlayerRender(this, instrAddr);
                    TraceNSMLPlayerDefeatedEntry(this, instrAddr);
                    TraceNSMLCallImpl(this, instrAddr);
                    TraceNSMLRandomCall(this, instrAddr);
                }

                // prefetch
                R[15] += 4;
                CurInstr = NextInstr[0];
                NextInstr[0] = NextInstr[1];
                NextInstr[1] = CodeRead32(R[15], false);

                // actually execute
                if (CheckCondition(CurInstr >> 28))
                {
                    u32 icode = ((CurInstr >> 4) & 0xF) | ((CurInstr >> 16) & 0xFF0);
                    ARMInterpreter::ARMInstrTable[icode](this);
                }
                else if ((CurInstr & 0xFE000000) == 0xFA000000)
                {
                    ARMInterpreter::A_BLX_IMM(this);
                }
                else
                    AddCycles_C();
            }

            // TODO optimize this shit!!!
            if (Halted)
            {
                if (Halted == 1 && NDS.ARM9Timestamp < NDS.ARM9Target)
                {
                    NDS.ARM9Timestamp = NDS.ARM9Target;
                }
                break;
            }
            /*if (NDS::IF[0] & NDS::IE[0])
            {
                if (NDS::IME[0] & 0x1)
                    TriggerIRQ();
            }*/
            if (IRQ) TriggerIRQ();

        }

        NDS.ARM9Timestamp += Cycles;
        Cycles = 0;
    }

    if (Halted == 2)
        Halted = 0;
}
template void ARMv5::Execute<CPUExecuteMode::Interpreter>();
template void ARMv5::Execute<CPUExecuteMode::InterpreterGDB>();
#ifdef JIT_ENABLED
template void ARMv5::Execute<CPUExecuteMode::JIT>();
#endif

template <CPUExecuteMode mode>
void ARMv4::Execute()
{
    if constexpr (mode == CPUExecuteMode::InterpreterGDB)
        GdbCheckB();

    if (Halted)
    {
        if (Halted == 2)
        {
            Halted = 0;
        }
        else if (NDS.HaltInterrupted(1))
        {
            Halted = 0;
            if (NDS.IME[1] & 0x1)
                TriggerIRQ();
        }
        else
        {
            NDS.ARM7Timestamp = NDS.ARM7Target;
            return;
        }
    }

    const bool nsmlRuntimeHooksEnabled = NSMLRuntimeHooksMaybeEnabled();
    while (NDS.ARM7Timestamp < NDS.ARM7Target)
    {
#ifdef JIT_ENABLED
        if constexpr (mode == CPUExecuteMode::JIT)
        {
            u32 instrAddr = R[15] - ((CPSR&0x20)?2:4);
            if (nsmlRuntimeHooksEnabled)
                TraceNSMLRandomCall(this, instrAddr);

            if ((instrAddr < FastBlockLookupStart || instrAddr >= (FastBlockLookupStart + FastBlockLookupSize))
                && !NDS.JIT.SetupExecutableRegion(1, instrAddr, FastBlockLookup, FastBlockLookupStart, FastBlockLookupSize))
            {
                NDS.ARM7Timestamp = NDS.ARM7Target;
                Log(LogLevel::Error, "ARMv4 PC in non executable region %08X\n", R[15]);
                return;
            }

            JitBlockEntry block = NDS.JIT.LookUpBlock(1, FastBlockLookup,
                instrAddr - FastBlockLookupStart, instrAddr);
            if (block)
                ARM_Dispatch(this, block);
            else
                NDS.JIT.CompileBlock(this);

            if (StopExecution)
            {
                if (IRQ)
                    TriggerIRQ();

                if (Halted || IdleLoop)
                {
                    if ((Halted == 1 || IdleLoop) && NDS.ARM7Timestamp < NDS.ARM7Target)
                    {
                        Cycles = 0;
                        NDS.ARM7Timestamp = NDS.ARM7Target;
                    }
                    IdleLoop = 0;
                    break;
                }
            }
        }
        else
#endif
        {
            if (CPSR & 0x20) // THUMB
            {
                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();

                // prefetch
                R[15] += 2;
                CurInstr = NextInstr[0];
                NextInstr[0] = NextInstr[1];
                NextInstr[1] = CodeRead16(R[15]);

                // actually execute
                u32 icode = (CurInstr >> 6);
                ARMInterpreter::THUMBInstrTable[icode](this);
            }
            else
            {
                if constexpr (mode == CPUExecuteMode::InterpreterGDB)
                    GdbCheckC();

                // prefetch
                R[15] += 4;
                CurInstr = NextInstr[0];
                NextInstr[0] = NextInstr[1];
                NextInstr[1] = CodeRead32(R[15]);

                // actually execute
                if (CheckCondition(CurInstr >> 28))
                {
                    u32 icode = ((CurInstr >> 4) & 0xF) | ((CurInstr >> 16) & 0xFF0);
                    ARMInterpreter::ARMInstrTable[icode](this);
                }
                else
                    AddCycles_C();
            }

            // TODO optimize this shit!!!
            if (Halted)
            {
                if (Halted == 1 && NDS.ARM7Timestamp < NDS.ARM7Target)
                {
                    NDS.ARM7Timestamp = NDS.ARM7Target;
                }
                break;
            }
            /*if (NDS::IF[1] & NDS::IE[1])
            {
                if (NDS::IME[1] & 0x1)
                    TriggerIRQ();
            }*/
            if (IRQ) TriggerIRQ();
        }

        NDS.ARM7Timestamp += Cycles;
        Cycles = 0;
    }

    if (Halted == 2)
        Halted = 0;

    if (Halted == 4)
    {
        assert(NDS.ConsoleType == 1);
        auto& dsi = dynamic_cast<melonDS::DSi&>(NDS);
        dsi.SoftReset();
        Halted = 2;
    }
}

template void ARMv4::Execute<CPUExecuteMode::Interpreter>();
template void ARMv4::Execute<CPUExecuteMode::InterpreterGDB>();
#ifdef JIT_ENABLED
template void ARMv4::Execute<CPUExecuteMode::JIT>();
#endif

void ARMv5::FillPipeline()
{
    SetupCodeMem(R[15]);

    if (CPSR & 0x20)
    {
        if ((R[15] - 2) & 0x2)
        {
            NextInstr[0] = CodeRead32(R[15] - 4, false) >> 16;
            NextInstr[1] = CodeRead32(R[15], false);
        }
        else
        {
            NextInstr[0] = CodeRead32(R[15] - 2, false);
            NextInstr[1] = NextInstr[0] >> 16;
        }
    }
    else
    {
        NextInstr[0] = CodeRead32(R[15] - 4, false);
        NextInstr[1] = CodeRead32(R[15], false);
    }
}

void ARMv4::FillPipeline()
{
    SetupCodeMem(R[15]);

    if (CPSR & 0x20)
    {
        NextInstr[0] = CodeRead16(R[15] - 2);
        NextInstr[1] = CodeRead16(R[15]);
    }
    else
    {
        NextInstr[0] = CodeRead32(R[15] - 4);
        NextInstr[1] = CodeRead32(R[15]);
    }
}

#ifdef GDBSTUB_ENABLED
u32 ARM::ReadReg(Gdb::Register reg)
{
    using Gdb::Register;
    int r = static_cast<int>(reg);

    if (reg < Register::pc) return R[r];
    else if (reg == Register::pc)
    {
        return R[r] - ((CPSR & 0x20) ? 2 : 4);
    }
    else if (reg == Register::cpsr) return CPSR;
    else if (reg == Register::sp_usr || reg == Register::lr_usr)
    {
        r -= static_cast<int>(Register::sp_usr);
        if (ModeIs(0x10) || ModeIs(0x1f))
        {
            return R[13 + r];
        }
        else switch (CPSR & 0x1f)
        {
        case 0x11: return R_FIQ[5 + r];
        case 0x12: return R_IRQ[0 + r];
        case 0x13: return R_SVC[0 + r];
        case 0x17: return R_ABT[0 + r];
        case 0x1b: return R_UND[0 + r];
        }
    }
    else if (reg >= Register::r8_fiq && reg <= Register::lr_fiq)
    {
        r -= static_cast<int>(Register::r8_fiq);
        return ModeIs(0x11) ? R[ 8 + r] : R_FIQ[r];
    }
    else if (reg == Register::sp_irq || reg == Register::lr_irq)
    {
        r -= static_cast<int>(Register::sp_irq);
        return ModeIs(0x12) ? R[13 + r] : R_IRQ[r];
    }
    else if (reg == Register::sp_svc || reg == Register::lr_svc)
    {
        r -= static_cast<int>(Register::sp_svc);
        return ModeIs(0x13) ? R[13 + r] : R_SVC[r];
    }
    else if (reg == Register::sp_abt || reg == Register::lr_abt)
    {
        r -= static_cast<int>(Register::sp_abt);
        return ModeIs(0x17) ? R[13 + r] : R_ABT[r];
    }
    else if (reg == Register::sp_und || reg == Register::lr_und)
    {
        r -= static_cast<int>(Register::sp_und);
        return ModeIs(0x1b) ? R[13 + r] : R_UND[r];
    }
    else if (reg == Register::spsr_fiq) return ModeIs(0x11) ? CPSR : R_FIQ[7];
    else if (reg == Register::spsr_irq) return ModeIs(0x12) ? CPSR : R_IRQ[2];
    else if (reg == Register::spsr_svc) return ModeIs(0x13) ? CPSR : R_SVC[2];
    else if (reg == Register::spsr_abt) return ModeIs(0x17) ? CPSR : R_ABT[2];
    else if (reg == Register::spsr_und) return ModeIs(0x1b) ? CPSR : R_UND[2];

    Log(LogLevel::Warn, "GDB reg read: unknown reg no %d\n", r);
    return 0xdeadbeef;
}
void ARM::WriteReg(Gdb::Register reg, u32 v)
{
    using Gdb::Register;
    int r = static_cast<int>(reg);

    if (reg < Register::pc) R[r] = v;
    else if (reg == Register::pc) JumpTo(v);
    else if (reg == Register::cpsr) CPSR = v;
    else if (reg == Register::sp_usr || reg == Register::lr_usr)
    {
        r -= static_cast<int>(Register::sp_usr);
        if (ModeIs(0x10) || ModeIs(0x1f))
        {
            R[13 + r] = v;
        }
        else switch (CPSR & 0x1f)
        {
        case 0x11: R_FIQ[5 + r] = v; break;
        case 0x12: R_IRQ[0 + r] = v; break;
        case 0x13: R_SVC[0 + r] = v; break;
        case 0x17: R_ABT[0 + r] = v; break;
        case 0x1b: R_UND[0 + r] = v; break;
        }
    }
    else if (reg >= Register::r8_fiq && reg <= Register::lr_fiq)
    {
        r -= static_cast<int>(Register::r8_fiq);
        *(ModeIs(0x11) ? &R[ 8 + r] : &R_FIQ[r]) = v;
    }
    else if (reg == Register::sp_irq || reg == Register::lr_irq)
    {
        r -= static_cast<int>(Register::sp_irq);
        *(ModeIs(0x12) ? &R[13 + r] : &R_IRQ[r]) = v;
    }
    else if (reg == Register::sp_svc || reg == Register::lr_svc)
    {
        r -= static_cast<int>(Register::sp_svc);
        *(ModeIs(0x13) ? &R[13 + r] : &R_SVC[r]) = v;
    }
    else if (reg == Register::sp_abt || reg == Register::lr_abt)
    {
        r -= static_cast<int>(Register::sp_abt);
        *(ModeIs(0x17) ? &R[13 + r] : &R_ABT[r]) = v;
    }
    else if (reg == Register::sp_und || reg == Register::lr_und)
    {
        r -= static_cast<int>(Register::sp_und);
        *(ModeIs(0x1b) ? &R[13 + r] : &R_UND[r]) = v;
    }
    else if (reg == Register::spsr_fiq)
    {
        *(ModeIs(0x11) ? &CPSR : &R_FIQ[7]) = v;
    }
    else if (reg == Register::spsr_irq)
    {
        *(ModeIs(0x12) ? &CPSR : &R_IRQ[2]) = v;
    }
    else if (reg == Register::spsr_svc)
    {
        *(ModeIs(0x13) ? &CPSR : &R_SVC[2]) = v;
    }
    else if (reg == Register::spsr_abt)
    {
        *(ModeIs(0x17) ? &CPSR : &R_ABT[2]) = v;
    }
    else if (reg == Register::spsr_und)
    {
        *(ModeIs(0x1b) ? &CPSR : &R_UND[2]) = v;
    }
    else Log(LogLevel::Warn, "GDB reg write: unknown reg no %d (write 0x%08x)\n", r, v);
}
u32 ARM::ReadMem(u32 addr, int size)
{
    if (size == 8) return BusRead8(addr);
    else if (size == 16) return BusRead16(addr);
    else if (size == 32) return BusRead32(addr);
    else return 0xfeedface;
}
void ARM::WriteMem(u32 addr, int size, u32 v)
{
    if (size == 8) BusWrite8(addr, (u8)v);
    else if (size == 16) BusWrite16(addr, (u16)v);
    else if (size == 32) BusWrite32(addr, v);
}

void ARM::ResetGdb()
{
    NDS.Reset();
    NDS.GPU.StartFrame(); // need this to properly kick off the scheduler & frame output
}
int ARM::RemoteCmd(const u8* cmd, size_t len)
{
    (void)len;

    Log(LogLevel::Info, "[ARMGDB] Rcmd: \"%s\"\n", cmd);
    if (!strcmp((const char*)cmd, "reset") || !strcmp((const char*)cmd, "r"))
    {
        Reset();
        return 0;
    }

    return 1; // not implemented (yet)
}

void ARMv5::WriteMem(u32 addr, int size, u32 v)
{
    if (addr < ITCMSize)
    {
        if (size == 8) *(u8*)&ITCM[addr & (ITCMPhysicalSize - 1)] = (u8)v;
        else if (size == 16) *(u16*)&ITCM[addr & (ITCMPhysicalSize - 1)] = (u16)v;
        else if (size == 32) *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)] = (u32)v;
        else {}
        return;
    }
    else if ((addr & DTCMMask) == DTCMBase)
    {
        if (size == 8) *(u8*)&DTCM[addr & (DTCMPhysicalSize - 1)] = (u8)v;
        else if (size == 16) *(u16*)&DTCM[addr & (DTCMPhysicalSize - 1)] = (u16)v;
        else if (size == 32) *(u32*)&DTCM[addr & (DTCMPhysicalSize - 1)] = (u32)v;
        else {}
        return;
    }

    ARM::WriteMem(addr, size, v);
}
u32 ARMv5::ReadMem(u32 addr, int size)
{
    if (addr < ITCMSize)
    {
        if (size == 8) return *(u8*)&ITCM[addr & (ITCMPhysicalSize - 1)];
        else if (size == 16) return *(u16*)&ITCM[addr & (ITCMPhysicalSize - 1)];
        else if (size == 32) return *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)];
        else return 0xfeedface;
    }
    else if ((addr & DTCMMask) == DTCMBase)
    {
        if (size == 8) return *(u8*)&DTCM[addr & (DTCMPhysicalSize - 1)];
        else if (size == 16) return *(u16*)&DTCM[addr & (DTCMPhysicalSize - 1)];
        else if (size == 32) return *(u32*)&DTCM[addr & (DTCMPhysicalSize - 1)];
        else return 0xfeedface;
    }

    return ARM::ReadMem(addr, size);
}
#endif

void ARMv4::DataRead8(u32 addr, u32* val)
{
    *val = BusRead8(addr);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

void ARMv4::DataRead16(u32 addr, u32* val)
{
    addr &= ~1;

    *val = BusRead16(addr);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

void ARMv4::DataRead32(u32 addr, u32* val)
{
    addr &= ~3;

    *val = BusRead32(addr);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][2];
}

void ARMv4::DataRead32S(u32 addr, u32* val)
{
    addr &= ~3;

    *val = BusRead32(addr);
    DataCycles += NDS.ARM7MemTimings[addr >> 15][3];
}

void ARMv4::DataWrite8(u32 addr, u8 val)
{
    BusWrite8(addr, val);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

void ARMv4::DataWrite16(u32 addr, u16 val)
{
    addr &= ~1;

    BusWrite16(addr, val);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

void ARMv4::DataWrite32(u32 addr, u32 val)
{
    addr &= ~3;

    BusWrite32(addr, val);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][2];
}

void ARMv4::DataWrite32S(u32 addr, u32 val)
{
    addr &= ~3;

    BusWrite32(addr, val);
    DataCycles += NDS.ARM7MemTimings[addr >> 15][3];
}


void ARMv4::AddCycles_C()
{
    // code only. this code fetch is sequential.
    Cycles += NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?1:3];
}

void ARMv4::AddCycles_CI(s32 num)
{
    // code+internal. results in a nonseq code fetch.
    Cycles += NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?0:2] + num;
}

void ARMv4::AddCycles_CDI()
{
    // LDR/LDM cycles.
    s32 numC = NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?0:2];
    s32 numD = DataCycles;

    if ((DataRegion >> 24) == 0x02) // mainRAM
    {
        if (CodeRegion == 0x02)
            Cycles += numC + numD;
        else
        {
            numC++;
            Cycles += std::max(numC + numD - 3, std::max(numC, numD));
        }
    }
    else if (CodeRegion == 0x02)
    {
        numD++;
        Cycles += std::max(numC + numD - 3, std::max(numC, numD));
    }
    else
    {
        Cycles += numC + numD + 1;
    }
}

void ARMv4::AddCycles_CD()
{
    // TODO: max gain should be 5c when writing to mainRAM
    s32 numC = NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?0:2];
    s32 numD = DataCycles;

    if ((DataRegion >> 24) == 0x02)
    {
        if (CodeRegion == 0x02)
            Cycles += numC + numD;
        else
            Cycles += std::max(numC + numD - 3, std::max(numC, numD));
    }
    else if (CodeRegion == 0x02)
    {
        Cycles += std::max(numC + numD - 3, std::max(numC, numD));
    }
    else
    {
        Cycles += numC + numD;
    }
}

u8 ARMv5::BusRead8(u32 addr)
{
    return NDS.ARM9Read8(addr);
}

u16 ARMv5::BusRead16(u32 addr)
{
    return NDS.ARM9Read16(addr);
}

u32 ARMv5::BusRead32(u32 addr)
{
    return NDS.ARM9Read32(addr);
}

void ARMv5::BusWrite8(u32 addr, u8 val)
{
    if (NSMLWriteTraceMaybeEnabled())
        TraceNSMLWrite(this, addr, val, 8);
    NDS.ARM9Write8(addr, val);
}

void ARMv5::BusWrite16(u32 addr, u16 val)
{
    if (NSMLWriteTraceMaybeEnabled())
        TraceNSMLWrite(this, addr, val, 16);
    NDS.ARM9Write16(addr, val);
}

void ARMv5::BusWrite32(u32 addr, u32 val)
{
    if (NSMLWriteTraceMaybeEnabled())
        TraceNSMLWrite(this, addr, val, 32);
    NDS.ARM9Write32(addr, val);
}

u8 ARMv4::BusRead8(u32 addr)
{
    return NDS.ARM7Read8(addr);
}

u16 ARMv4::BusRead16(u32 addr)
{
    return NDS.ARM7Read16(addr);
}

u32 ARMv4::BusRead32(u32 addr)
{
    return NDS.ARM7Read32(addr);
}

void ARMv4::BusWrite8(u32 addr, u8 val)
{
    NDS.ARM7Write8(addr, val);
}

void ARMv4::BusWrite16(u32 addr, u16 val)
{
    NDS.ARM7Write16(addr, val);
}

void ARMv4::BusWrite32(u32 addr, u32 val)
{
    NDS.ARM7Write32(addr, val);
}
}

