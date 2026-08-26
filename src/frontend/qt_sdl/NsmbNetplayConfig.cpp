#include "NsmbNetplayConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace NsmbMvlNetplay::Config {

namespace {

class ProcessEnvironment final : public Environment {
public:
  const char *Get(const char *name) const override { return std::getenv(name); }
};

std::uint32_t ComposeMvlSceneSettingsForStage(int stage) {
  const auto clampedStage = static_cast<std::uint32_t>(std::clamp(stage, 0, 4));
  return ((0xB4u + clampedStage) << 16) | 0xFF00u;
}

} // namespace

const Environment &GetProcessEnvironment() {
  static const ProcessEnvironment environment;
  return environment;
}

bool ParseFlag(const char *value) {
  return value && value[0] && std::strcmp(value, "0") != 0;
}

const char *ValueOr(const char *value, const char *fallback) {
  return value && value[0] ? value : fallback;
}

int ParseInt(const char *value, int fallback) {
  if (!value || !value[0])
    return fallback;

  char *end = nullptr;
  const long parsed = std::strtol(value, &end, 0);
  if (end == value)
    return fallback;
  return static_cast<int>(parsed);
}

double ParseDouble(const char *value, double fallback) {
  if (!value || !value[0])
    return fallback;

  char *end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value)
    return fallback;
  return parsed;
}

std::uint32_t ParseU32(const char *value, std::uint32_t fallback) {
  if (!value || !value[0])
    return fallback;
  return static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0));
}

std::vector<std::uint32_t> ParseU32List(const char *value) {
  std::vector<std::uint32_t> values;
  if (!value || !value[0])
    return values;

  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, ',')) {
    token.erase(std::remove_if(token.begin(), token.end(),
                               [](unsigned char character) {
                                 return std::isspace(character) != 0;
                               }),
                token.end());
    if (token.empty())
      continue;
    values.push_back(
        static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, 0)));
  }
  return values;
}

bool ParseFrameRanges(const char *value, std::vector<FrameRange> &ranges) {
  if (!value || !value[0])
    return true;

  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, ',')) {
    const auto first = token.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    const auto last = token.find_last_not_of(" \t\r\n");
    token = token.substr(first, last - first + 1);

    const auto parse = [](const std::string &text, std::uint32_t &result) {
      char *end = nullptr;
      const unsigned long value = std::strtoul(text.c_str(), &end, 0);
      if (!end || *end != '\0')
        return false;
      result = static_cast<std::uint32_t>(value);
      return true;
    };

    std::uint32_t start = 0;
    std::uint32_t end = 0;
    const auto dash = token.find('-');
    if (dash == std::string::npos) {
      if (!parse(token, start))
        return false;
      end = start;
    } else if (!parse(token.substr(0, dash), start) ||
               !parse(token.substr(dash + 1), end) || end < start) {
      return false;
    }
    ranges.emplace_back(start, end);
  }
  return true;
}

bool HasValue(const char *value) { return value && value[0]; }

bool ReadFlag(const Environment &environment, const char *name) {
  return ParseFlag(environment.Get(name));
}

const char *ReadCString(const Environment &environment, const char *name,
                        const char *fallback) {
  return ValueOr(environment.Get(name), fallback);
}

int ReadInt(const Environment &environment, const char *name, int fallback) {
  return ParseInt(environment.Get(name), fallback);
}

double ReadDouble(const Environment &environment, const char *name,
                  double fallback) {
  return ParseDouble(environment.Get(name), fallback);
}

std::uint32_t ReadU32(const Environment &environment, const char *name,
                      std::uint32_t fallback) {
  return ParseU32(environment.Get(name), fallback);
}

bool ReadHasValue(const Environment &environment, const char *name) {
  return HasValue(environment.Get(name));
}

std::vector<std::uint32_t> ReadU32List(const Environment &environment,
                                       const char *name) {
  return ParseU32List(environment.Get(name));
}

BootstrapConfig LoadBootstrapConfig(const Environment &environment) {
  BootstrapConfig config;
  const char *netplayEnabled = environment.Get("MELONDS_NSML_NETPLAY");
  config.Enabled = HasValue(netplayEnabled)
                       ? ParseFlag(netplayEnabled)
                       : ReadFlag(environment, "MELONDS_NSML_POC");
  config.TestEnabled = ReadFlag(environment, "MELONDS_NSML_TEST");
  config.TestFrames = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment, "MELONDS_NSML_TEST_FRAMES", 0)));
  config.TestInstanceCount =
      std::clamp(ReadInt(environment, "MELONDS_NSML_TEST_INSTANCES", 1), 1, 16);
  config.HashEnabled = !ReadFlag(environment, "MELONDS_NSML_DISABLE_HASH");
  config.HashInterval =
      std::max(1, ReadInt(environment, "MELONDS_NSML_HASH_INTERVAL", 60));
  config.WaitTimeoutMs =
      std::max(0, ReadInt(environment, "MELONDS_NSML_WAIT_TIMEOUT_MS", 60000));
  config.QuitGraceMs =
      std::max(0, ReadInt(environment, "MELONDS_NSML_QUIT_GRACE_MS", 0));
  config.InputTraceEnabled = ReadFlag(environment, "MELONDS_NSML_INPUT_TRACE");
  config.InputTraceInterval = std::max(
      1, ReadInt(environment, "MELONDS_NSML_INPUT_TRACE_INTERVAL", 60));
  return config;
}

BootstrapConfig LoadBootstrapConfig() {
  return LoadBootstrapConfig(GetProcessEnvironment());
}

ConnectionConfig LoadConnectionConfig(const Environment &environment,
                                      bool testEnabled) {
  ConnectionConfig config;
  const char *role = environment.Get("MELONDS_NSML_ROLE");
  if (!HasValue(role))
    role = environment.Get("MELONDS_NSML_LAN_ROLE");
  config.Client = HasValue(role) && std::strcmp(role, "client") == 0;

  config.Delay = std::max(0, ReadInt(environment, "MELONDS_NSML_DELAY", 6));
  config.WarmupFrames =
      std::max(0, ReadInt(environment, "MELONDS_NSML_NETPLAY_WARMUP_FRAMES",
                          testEnabled ? config.Delay * 2 : 0));
  config.Port = ReadInt(environment, "MELONDS_NSML_PORT", 8065);
  config.LocalInstance = ReadInt(environment, "MELONDS_NSML_LOCAL_INSTANCE",
                                 config.Client ? 1 : 0);
  config.StartFrame = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment, "MELONDS_NSML_NETPLAY_START_FRAME", 0)));
  config.LocalStartupRawFrame = config.StartFrame;
  config.SharedLogicalEpoch = config.StartFrame;
  config.LocalWaitsForRemote =
      !ReadFlag(environment, "MELONDS_NSML_NO_LOCAL_WAIT");
  config.RemoteInputTimeoutFatal =
      ReadFlag(environment, "MELONDS_NSML_REMOTE_INPUT_TIMEOUT_FATAL");
  config.PeerHost = ReadCString(environment, "MELONDS_NSML_PEER", "127.0.0.1");
  return config;
}

ConnectionConfig LoadConnectionConfig(bool testEnabled) {
  return LoadConnectionConfig(GetProcessEnvironment(), testEnabled);
}

InputConfig LoadInputConfig(const Environment &environment,
                            bool netplayOnlyForMaxFrameLeadDefault) {
  InputConfig config;
  config.SendDelayFrames = std::max(
      0, ReadInt(environment, "MELONDS_NSML_INPUT_SEND_DELAY_FRAMES", 0));
  config.SendJitterFrames = std::max(
      0, ReadInt(environment, "MELONDS_NSML_INPUT_SEND_JITTER_FRAMES", 0));
  config.SendDelayStartFrame = static_cast<std::uint32_t>(std::clamp(
      ReadInt(environment, "MELONDS_NSML_INPUT_SEND_DELAY_START_FRAME", 0), 0,
      1000000));
  config.SendDelayEndFrame = static_cast<std::uint32_t>(std::clamp(
      ReadInt(environment, "MELONDS_NSML_INPUT_SEND_DELAY_END_FRAME", 0), 0,
      1000000));
  if (config.SendDelayEndFrame != 0 &&
      config.SendDelayEndFrame < config.SendDelayStartFrame)
    config.SendDelayEndFrame = config.SendDelayStartFrame;

  config.UseHistoryBundle =
      ReadFlag(environment, "MELONDS_NSML_INPUT_UNRELIABLE");
  config.BundleHistory = std::clamp(
      ReadInt(environment, "MELONDS_NSML_INPUT_BUNDLE_HISTORY", 0), 0, 31);
  config.DropModulo =
      std::max(0, ReadInt(environment, "MELONDS_NSML_INPUT_DROP_MODULO", 0));
  config.DropOffset =
      std::max(0, ReadInt(environment, "MELONDS_NSML_INPUT_DROP_OFFSET", 0));
  if (config.DropModulo > 0)
    config.DropOffset %= config.DropModulo;
  config.DropStartFrame = static_cast<std::uint32_t>(
      std::clamp(ReadInt(environment, "MELONDS_NSML_INPUT_DROP_START_FRAME", 0),
                 0, 1000000));
  config.DropEndFrame = static_cast<std::uint32_t>(
      std::clamp(ReadInt(environment, "MELONDS_NSML_INPUT_DROP_END_FRAME", 0),
                 0, 1000000));
  if (config.DropEndFrame > 0 && config.DropEndFrame < config.DropStartFrame)
    config.DropEndFrame = config.DropStartFrame;

  config.MaxFrameLead =
      ReadInt(environment, "MELONDS_NSML_INPUT_MAX_FRAME_LEAD",
              netplayOnlyForMaxFrameLeadDefault ? 2 : -1);
  config.NetplayOnly = ReadFlag(environment, "MELONDS_NSML_INPUT_NETPLAY_ONLY");
  config.NetplayTrace =
      ReadFlag(environment, "MELONDS_NSML_INPUT_NETPLAY_TRACE");
  config.HealthTrace = ReadFlag(environment, "MELONDS_NSML_INPUT_HEALTH_TRACE");
  config.HealthTraceInterval = std::clamp(
      ReadInt(environment, "MELONDS_NSML_INPUT_HEALTH_TRACE_INTERVAL", 120), 1,
      3600);
  config.HealthTraceWaitThresholdMs = std::clamp(
      ReadInt(environment, "MELONDS_NSML_INPUT_HEALTH_TRACE_WAIT_THRESHOLD_MS",
              16),
      1, 5000);
  config.WaitPollUs = std::clamp(
      ReadInt(environment, "MELONDS_NSML_INPUT_WAIT_POLL_US", 100), 50, 5000);
  return config;
}

InputConfig LoadInputConfig(bool netplayOnlyForMaxFrameLeadDefault) {
  return LoadInputConfig(GetProcessEnvironment(),
                         netplayOnlyForMaxFrameLeadDefault);
}

RuntimePatchConfig LoadRuntimePatchConfig(const Environment &environment) {
  RuntimePatchConfig config;
  config.PlayerStickToStarStartFrame = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_PLAYER_STICK_TO_STAR_START_FRAME", 0)));
  config.PlayerStickToStarEndFrame = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_PLAYER_STICK_TO_STAR_END_FRAME", 0)));
  if (config.PlayerStickToStarEndFrame == 0)
    config.PlayerStickToStarEndFrame = config.PlayerStickToStarStartFrame;
  if (config.PlayerStickToStarEndFrame < config.PlayerStickToStarStartFrame)
    std::swap(config.PlayerStickToStarStartFrame,
              config.PlayerStickToStarEndFrame);
  config.PlayerStickToStarSlot = std::clamp(
      ReadInt(environment, "MELONDS_NSML_PLAYER_STICK_TO_STAR_SLOT", 0), 0, 1);
  config.ForcePlayerDeathCountersEnabled =
      ReadFlag(environment, "MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS");
  config.ForcePlayerDeathCountersHostOnly = ReadFlag(
      environment, "MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_HOST_ONLY");
  config.ForcePlayerDeathCountersClientOnly = ReadFlag(
      environment, "MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_CLIENT_ONLY");
  config.ForcePlayerDeathCountersStartFrame =
      static_cast<std::uint32_t>(std::max(
          0,
          ReadInt(environment,
                  "MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_START_FRAME", 0)));
  config.ForcePlayerDeathCountersEndFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment,
                 "MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_END_FRAME", 0)));
  config.ForcePlayerDeathCounter0 = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTER0", 0)));
  config.ForcePlayerDeathCounter1 = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTER1", 0)));
  config.ForcePlayerLivesEnabled =
      ReadFlag(environment, "MELONDS_NSML_FORCE_PLAYER_LIVES");
  config.ForcePlayerLife0 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_LIFE0", 5)));
  config.ForcePlayerLife1 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_LIFE1", 5)));
  config.ForcePlayerPowerupsEnabled =
      ReadFlag(environment, "MELONDS_NSML_FORCE_PLAYER_POWERUPS");
  config.ForcePlayerPowerupsStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_POWERUPS_START_FRAME",
                 0)));
  config.ForcePlayerPowerupsEndFrame = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_POWERUPS_END_FRAME", 0)));
  config.ForcePlayerPowerup0 = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_POWERUP0", 0)));
  config.ForcePlayerPowerup1 = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_POWERUP1", 0)));
  config.ForcePlayerInventoryPowerupsEnabled =
      ReadFlag(environment, "MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUPS");
  config.ForcePlayerInventoryPowerupsStartFrame =
      static_cast<std::uint32_t>(std::max(
          0, ReadInt(environment,
                     "MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUPS_START_FRAME",
                     0)));
  config.ForcePlayerInventoryPowerupsEndFrame =
      static_cast<std::uint32_t>(std::max(
          0, ReadInt(environment,
                     "MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUPS_END_FRAME",
                     0)));
  config.ForcePlayerInventoryPowerup0 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUP0", 0)));
  config.ForcePlayerInventoryPowerup1 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUP1", 0)));
  config.ForcePlayerCoinsEnabled =
      ReadFlag(environment, "MELONDS_NSML_FORCE_PLAYER_COINS");
  config.ForcePlayerCoinsStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_COINS_START_FRAME",
                 0)));
  config.ForcePlayerCoinsEndFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_COINS_END_FRAME", 0)));
  config.ForcePlayerCoins0 = static_cast<std::uint32_t>(std::clamp(
      ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_COINS0", 0), 0, 7));
  config.ForcePlayerCoins1 = static_cast<std::uint32_t>(std::clamp(
      ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_COINS1", 0), 0, 7));
  config.ForcePlayerStarCountersEnabled =
      ReadFlag(environment, "MELONDS_NSML_FORCE_PLAYER_STAR_COUNTERS");
  config.ForcePlayerStarCountersStartFrame = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_STAR_COUNTERS_START_FRAME",
                          0)));
  config.ForcePlayerStarCountersEndFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment,
                 "MELONDS_NSML_FORCE_PLAYER_STAR_COUNTERS_END_FRAME", 0)));
  config.ForcePlayerBattleStars0 = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_BATTLE_STARS0", 0)));
  config.ForcePlayerBattleStars1 = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_FORCE_PLAYER_BATTLE_STARS1", 0)));
  config.ForcePlayerDisplayedStars0 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_DISPLAYED_STARS0", 0)));
  config.ForcePlayerDisplayedStars1 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_DISPLAYED_STARS1", 0)));
  config.ForcePlayerCollectedStars0 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_COLLECTED_STARS0", 0)));
  config.ForcePlayerCollectedStars1 = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_FORCE_PLAYER_COLLECTED_STARS1", 0)));
  config.TracePlayerLifeChanges =
      ReadFlag(environment, "MELONDS_NSML_TRACE_PLAYER_LIFE_CHANGES");
  config.PacketBridgeJitHelperPatchEnabled =
      ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE_JIT_HELPER_PATCH");
  config.PacketBridgeJitHelperPatchFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment,
                 "MELONDS_NSML_PACKET_BRIDGE_JIT_HELPER_PATCH_FRAME", 0)));
  return config;
}

RuntimePatchConfig LoadRuntimePatchConfig() {
  return LoadRuntimePatchConfig(GetProcessEnvironment());
}

HarnessConfig LoadHarnessConfig(const Environment &environment) {
  HarnessConfig config;
  config.InputScriptPath =
      ReadCString(environment, "MELONDS_NSML_INPUT_SCRIPT", "");
  config.FrameBarrierEnabled =
      ReadFlag(environment, "MELONDS_NSML_FRAME_BARRIER");
  config.SerialRunEnabled = ReadFlag(environment, "MELONDS_NSML_SERIAL_RUN");
  config.SeedWaitTimeoutMs = std::max(
      0, ReadInt(environment, "MELONDS_NSML_SEED_WAIT_TIMEOUT_MS", 10000));
  config.WaitForPeerBeforeStart =
      ReadFlag(environment, "MELONDS_NSML_WAIT_FOR_PEER");
  config.WaitForPeerAtNetplayStart =
      ReadFlag(environment, "MELONDS_NSML_WAIT_FOR_PEER_AT_NETPLAY_START");
  config.DeferNetworkUntilStart =
      ReadFlag(environment, "MELONDS_NSML_DEFER_NETWORK_UNTIL_START");
  config.NetplayFrameBarrierEnabled =
      ReadFlag(environment, "MELONDS_NSML_NETPLAY_FRAME_BARRIER");
  config.NeutralizePolledInput =
      ReadFlag(environment, "MELONDS_NSML_NEUTRALIZE_POLLED_INPUT");
  config.NeutralizePolledInputPreserveTouch = ReadFlag(
      environment, "MELONDS_NSML_NEUTRALIZE_POLLED_INPUT_PRESERVE_TOUCH");
  config.NetworkPumpThreadEnabled =
      ReadFlag(environment, "MELONDS_NSML_NET_PUMP_THREAD");
  config.NetworkPumpSleepUs = std::clamp(
      ReadInt(environment, "MELONDS_NSML_NET_PUMP_SLEEP_US", 250), 50, 5000);


  config.StateSaveDir =
      ReadCString(environment, "MELONDS_NSML_STATE_SAVE_DIR", "");
  config.StateSaveFrame = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment, "MELONDS_NSML_STATE_SAVE_FRAME", 0)));
  config.StateLoadDir =
      ReadCString(environment, "MELONDS_NSML_STATE_LOAD_DIR", "");
  config.StateLoadFrameSet =
      ReadHasValue(environment, "MELONDS_NSML_STATE_LOAD_FRAME");
  if (config.StateLoadFrameSet) {
    config.StateLoadFrame = static_cast<std::uint32_t>(std::max(
        0, std::atoi(environment.Get("MELONDS_NSML_STATE_LOAD_FRAME"))));
  }
  config.TestEmulationPauseFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_TEST_EMULATION_PAUSE_FRAME", 0)));
  config.TestEmulationPauseDurationMs = std::clamp(
      ReadInt(environment, "MELONDS_NSML_TEST_EMULATION_PAUSE_MS", 0),
      0, 10000);
  config.TestEmulationPauseRole = ReadCString(
      environment, "MELONDS_NSML_TEST_EMULATION_PAUSE_ROLE", "");
  return config;
}

HarnessConfig LoadHarnessConfig() {
  return LoadHarnessConfig(GetProcessEnvironment());
}

PacketBridgeConfig LoadPacketBridgeConfig(const Environment &environment) {
  PacketBridgeConfig config;
  config.Enabled = ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE");
  config.Only = ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE_ONLY");
  config.AllowPreGame =
      ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE_ALLOW_PRE_GAME");
  config.TraceEnabled =
      ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE_TRACE");
  if (const char *localPlayer =
          environment.Get("MELONDS_NSML_PACKET_BRIDGE_LOCAL_PLAYER")) {
    config.LocalPlayerOverride = std::clamp(std::atoi(localPlayer), 0, 1);
  }
  config.DirectCaptureEnabled =
      ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE_DIRECT_CAPTURE");
  config.ForceTickEnabled =
      ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE_FORCE_TICK");
  config.ForceTickStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment,
                 "MELONDS_NSML_PACKET_BRIDGE_FORCE_TICK_START_FRAME", 0)));
  config.ForceTickBase =
      ReadInt(environment, "MELONDS_NSML_PACKET_BRIDGE_FORCE_TICK_BASE", -1);
  config.ForceGameLocalPlayerID = ReadInt(
      environment, "MELONDS_NSML_PACKET_BRIDGE_FORCE_GAME_LOCAL_PLAYER_ID", -1);
  config.ForceGameLocalPlayerIDStartFrame = static_cast<std::uint32_t>(std::max(
      0,
      ReadInt(
          environment,
          "MELONDS_NSML_PACKET_BRIDGE_FORCE_GAME_LOCAL_PLAYER_ID_START_FRAME",
          0)));
  config.ForceGameLocalPlayerIDEarly =
      ReadFlag(environment,
               "MELONDS_NSML_PACKET_BRIDGE_FORCE_GAME_LOCAL_PLAYER_ID_EARLY");
  config.MaxFrameLead =
      ReadInt(environment, "MELONDS_NSML_PACKET_BRIDGE_MAX_FRAME_LEAD", -1);
  config.ThrottleTimeoutMs = std::max(
      0, ReadInt(environment, "MELONDS_NSML_PACKET_BRIDGE_THROTTLE_TIMEOUT_MS",
                 5000));
  config.ThrottleStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_PACKET_BRIDGE_THROTTLE_START_FRAME",
                 0)));
  config.LocalInputDelay =
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_PACKET_BRIDGE_LOCAL_INPUT_DELAY", 0));
  config.NeutralizeLocalInput = ReadFlag(
      environment, "MELONDS_NSML_PACKET_BRIDGE_NEUTRALIZE_LOCAL_INPUT");
  config.PreserveLocalTouch =
      ReadFlag(environment, "MELONDS_NSML_PACKET_BRIDGE_PRESERVE_LOCAL_TOUCH");
  config.SendDelayFrames =
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_PACKET_BRIDGE_SEND_DELAY_FRAMES", 0));
  config.SendJitterFrames =
      std::max(0, ReadInt(environment,
                          "MELONDS_NSML_PACKET_BRIDGE_SEND_JITTER_FRAMES", 0));
  return config;
}

PacketBridgeConfig LoadPacketBridgeConfig() {
  return LoadPacketBridgeConfig(GetProcessEnvironment());
}

RollbackConfig LoadRollbackConfig(const Environment &environment) {
  RollbackConfig config;
  config.Enabled = ReadFlag(environment, "MELONDS_NSML_ROLLBACK");
  config.Resimulate = ReadFlag(environment, "MELONDS_NSML_ROLLBACK_RESIMULATE");
  config.SkipRenderDuringResim =
      ReadFlag(environment, "MELONDS_NSML_ROLLBACK_RESIM_SKIP_RENDER");
  config.SkipIntermediateResimCheckpoints = ReadFlag(
      environment, "MELONDS_NSML_ROLLBACK_RESIM_SKIP_INTERMEDIATE_CHECKPOINTS");
  config.SkipJitReset =
      environment.Get("MELONDS_NSML_ROLLBACK_SKIP_JIT_RESET") != nullptr;
  config.InputWaitUs = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_INPUT_WAIT_US", 0), 0, 20000);
  config.RestoreProbe =
      ReadFlag(environment, "MELONDS_NSML_ROLLBACK_RESTORE_PROBE");
  config.PredictionProbeModulo = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_MODULO", 0),
      0, 600);
  config.PredictionProbeOffset = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_OFFSET", 0),
      0, std::max(0, config.PredictionProbeModulo - 1));
  config.PredictionProbeLimit = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_LIMIT", -1),
      -1, 10000);
  config.PredictionProbeStartFrame = static_cast<std::uint32_t>(std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_START_FRAME",
              0),
      0, 1000000));
  config.PredictionProbeEndFrame = static_cast<std::uint32_t>(
      std::clamp(ReadInt(environment,
                         "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_END_FRAME", 0),
                 0, 1000000));
  if (config.PredictionProbeEndFrame != 0 &&
      config.PredictionProbeEndFrame < config.PredictionProbeStartFrame)
    config.PredictionProbeEndFrame = config.PredictionProbeStartFrame;
  config.PredictionProbeKeyMask = static_cast<std::uint32_t>(std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_KEY_MASK",
              0x1),
      1, 0xFFF));
  config.PredictionProbeConfirmDelayFrames = std::clamp(
      ReadInt(environment,
              "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_DELAY_FRAMES",
              0),
      0, 180);
  if (config.PredictionProbeConfirmDelayFrames == 0 &&
      ReadFlag(environment,
               "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_AFTER_ONE_FRAME"))
    config.PredictionProbeConfirmDelayFrames = 1;

  const char *backend =
      ReadCString(environment, "MELONDS_NSML_ROLLBACK_BACKEND", "savestate");
  if (!std::strcmp(backend, "corelite") || !std::strcmp(backend, "core-lite"))
    config.Backend = RollbackBackend::CoreLite;
  else if (!std::strcmp(backend, "coresparse") ||
           !std::strcmp(backend, "core-sparse"))
    config.Backend = RollbackBackend::CoreSparse;
  else if (!std::strcmp(backend, "coredelta") ||
           !std::strcmp(backend, "core-delta"))
    config.Backend = RollbackBackend::CoreDelta;
  else if (!std::strcmp(backend, "coreframedelta") ||
           !std::strcmp(backend, "core-frame-delta"))
    config.Backend = RollbackBackend::CoreFrameDelta;
  else if (!std::strcmp(backend, "corepreimage") ||
           !std::strcmp(backend, "core-preimage"))
    config.Backend = RollbackBackend::CorePreimage;
  else if (!std::strcmp(backend, "tinycorepreimage") ||
           !std::strcmp(backend, "tiny-core-preimage"))
    config.Backend = RollbackBackend::TinyCorePreimage;
  else if (!std::strcmp(backend, "romloop") ||
           !std::strcmp(backend, "rom-loop") ||
           !std::strcmp(backend, "slippi"))
    config.Backend = RollbackBackend::RomLoop;

  config.Window = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_WINDOW", 20), 1, 180);
  config.CheckpointInterval = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_CHECKPOINT_INTERVAL", 1), 1,
      30);
  config.DeltaKeyframeInterval = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_DELTA_KEYFRAME_INTERVAL", 10),
      1, 60);
  config.MainRAMPageSize = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_MAIN_RAM_PAGE_SIZE", 4096),
      256, 4096);
  if ((config.MainRAMPageSize & (config.MainRAMPageSize - 1)) != 0)
    config.MainRAMPageSize = 4096;
  config.CoreSkipMask = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_CORE_SKIP_MASK", 0), 0, 31);
  config.TinyCoreFlags = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_TINY_CORE_FLAGS", 0), 0,
      2047);
  config.ResimulateDelayFrames = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_RESIMULATE_DELAY_FRAMES", 0),
      0, 30);
  config.MaxResimFrames = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_MAX_RESIM_FRAMES", 0), 0, 30);
  config.PredictionHorizonFrames = std::clamp(
      ReadInt(environment,
              "MELONDS_NSML_ROLLBACK_PREDICTION_HORIZON_FRAMES", 0),
      // The 12-entry ROM-loop history includes both the restored frame and
      // the gate after the current frame, leaving at most ten rollback frames.
      0, 10);
  config.PredictionHorizonTimeoutMs = std::clamp(
      ReadInt(environment, "MELONDS_NSML_ROLLBACK_HORIZON_TIMEOUT_MS", 7000),
      100, 60000);
  config.PhaseRecoveryEnabled =
      ReadFlag(environment, "MELONDS_NSML_ROLLBACK_PHASE_RECOVERY");
  return config;
}

RollbackConfig LoadRollbackConfig() {
  return LoadRollbackConfig(GetProcessEnvironment());
}

MvlConfig LoadMvlConfig(const Environment &environment) {
  MvlConfig config;
  config.Stage = std::clamp(
      ReadInt(environment, "MELONDS_NSML_MVL_STAGE", 0), 0, 4);
  for (const std::uint32_t stage :
       ReadU32List(environment, "MELONDS_NSML_MVL_STAGE_SEQUENCE"))
    config.StageSequence.push_back(std::clamp(static_cast<int>(stage), 0, 4));
  if (!config.StageSequence.empty())
    config.Stage = config.StageSequence.front();

  if (ReadHasValue(environment, "MELONDS_NSML_MVL_SCENE_SETTINGS")) {
    config.StageSceneSettings =
        ReadU32(environment, "MELONDS_NSML_MVL_SCENE_SETTINGS", 0x00B4FF00);
  } else {
    const auto sceneStage = static_cast<int>(
        ReadU32(environment, "MELONDS_NSML_MVL_STAGE", 0));
    config.StageSceneSettings = ComposeMvlSceneSettingsForStage(sceneStage);
  }

  config.CourseMode =
      ReadCString(environment, "MELONDS_NSML_MVL_COURSE_MODE", "fixed");
  if (config.CourseMode != "fixed" && config.CourseMode != "random" &&
      config.CourseMode != "select") {
    config.InvalidCourseMode = config.CourseMode;
    config.CourseMode = "fixed";
  }
  config.TargetWins =
      std::clamp(ReadInt(environment, "MELONDS_NSML_MVL_WINS", 2), 1, 3);
  config.BigStarTarget =
      std::clamp(ReadInt(environment, "MELONDS_NSML_MVL_BIG_STARS", 5), 3, 10);
  config.RuntimeConfigEnabled =
      ReadHasValue(environment, "MELONDS_NSML_MVL_STAGE") ||
      ReadHasValue(environment, "MELONDS_NSML_MVL_SCENE_SETTINGS") ||
      ReadHasValue(environment, "MELONDS_NSML_MVL_BIG_STARS") ||
      ReadHasValue(environment, "MELONDS_NSML_MVL_LIVES");
  const std::string lives =
      ReadCString(environment, "MELONDS_NSML_MVL_LIVES", "endless");
  config.InitialLives = lives == "5" ? 5u : 3u;
  config.LifeModeSelector = lives == "endless" || lives == "Endless" ? 2u : 0u;
  config.BigStarSelector = config.BigStarTarget == 3    ? 0u
                           : config.BigStarTarget == 10 ? 2u
                                                        : 1u;
  config.AutoRestartAfterResult =
      ReadFlag(environment, "MELONDS_NSML_MVL_AUTO_RESTART_AFTER_RESULT");
  config.AutoRestartDelayFrames = static_cast<std::uint32_t>(
      std::max(1, ReadInt(environment,
                          "MELONDS_NSML_MVL_AUTO_RESTART_DELAY_FRAMES", 120)));
  config.AutoRestartBootstrapFrame = static_cast<std::uint32_t>(
      std::clamp(ReadInt(environment,
                         "MELONDS_NSML_MVL_AUTO_RESTART_BOOTSTRAP_FRAME", 120),
                 0, 1000000));

  config.CameraInitHold.Enabled =
      ReadFlag(environment, "MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD");
  config.CameraInitHold.HostOnly =
      ReadFlag(environment, "MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_HOST_ONLY");
  config.CameraInitHold.ClientOnly =
      ReadFlag(environment, "MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_CLIENT_ONLY");
  config.CameraInitHold.StartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment,
                 "MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_START_FRAME", 840)));
  config.CameraInitHold.EndFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment,
                 "MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_END_FRAME", 0)));

  if (ReadHasValue(environment, "MELONDS_NSML_NET_RANDOM_VALUE")) {
    config.NetRandom.Enabled = true;
    config.NetRandom.Auto =
        ReadFlag(environment, "MELONDS_NSML_NET_RANDOM_AUTO");
    config.NetRandom.Value =
        ReadU32(environment, "MELONDS_NSML_NET_RANDOM_VALUE", 0);
    config.NetRandom.Frame = static_cast<std::uint32_t>(std::max(
        0, ReadInt(environment, "MELONDS_NSML_NET_RANDOM_FRAME", 0)));
    config.MatchSeed = config.NetRandom.Value;
    config.MatchSeedConfigured = true;
  }

  if (ReadHasValue(environment, "MELONDS_NSML_MATCH_SEED")) {
    config.MatchSeed = ReadU32(environment, "MELONDS_NSML_MATCH_SEED", 0);
    config.MatchSeedConfigured = true;
  }
  config.MatchSeedSequence =
      ReadU32List(environment, "MELONDS_NSML_MATCH_SEED_SEQUENCE");
  if (!config.MatchSeedSequence.empty()) {
    config.MatchSeed = config.MatchSeedSequence.front();
    config.MatchSeedConfigured = true;
    config.NetRandom.Value = config.MatchSeed;
  }

  return config;
}

MvlConfig LoadMvlConfig() { return LoadMvlConfig(GetProcessEnvironment()); }

DiagnosticsConfig LoadDiagnosticsConfig(const Environment &environment,
                                        int diagnosticRingCapacity) {
  DiagnosticsConfig config;
  config.HangDiagnosticsEnabled =
      ReadFlag(environment, "MELONDS_NSML_HANG_DIAGNOSTICS");
  config.HangWatchdogIntervalMs = std::clamp(
      ReadInt(environment, "MELONDS_NSML_WATCHDOG_INTERVAL_MS", 1000), 100,
      60000);
  config.HangThresholdMs =
      std::clamp(ReadInt(environment, "MELONDS_NSML_HANG_THRESHOLD_MS", 8000),
                 1000, 300000);
  config.HangWatchdogPath =
      ReadCString(environment, "MELONDS_NSML_WATCHDOG_FILE", "");
  config.HangPhaseEventsPath =
      ReadCString(environment, "MELONDS_NSML_PHASE_EVENTS_FILE", "");
  config.HangDumpPath =
      ReadCString(environment, "MELONDS_NSML_HANG_DUMP_FILE", "");
  config.ActiveFpsStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_ACTIVE_FPS_START_FRAME", 0)));
  config.ActiveFrameSpikeThresholdUs =
      std::clamp(
          ReadInt(environment, "MELONDS_NSML_FPS_SPIKE_THRESHOLD_MS", 25), 1,
          1000) *
      1000;
  config.ActiveFrameSpikeTrace =
      ReadFlag(environment, "MELONDS_NSML_FPS_SPIKE_TRACE");
  config.FrameHeartbeatInterval = std::clamp(
      ReadInt(environment, "MELONDS_NSML_FRAME_HEARTBEAT_INTERVAL", 0), 0,
      3600);
  config.GameplayHeartbeatInterval = std::clamp(
      ReadInt(environment, "MELONDS_NSML_GAMEPLAY_HEARTBEAT_INTERVAL", 0), 0,
      3600);
  config.FrameHeartbeatPath =
      ReadCString(environment, "MELONDS_NSML_FRAME_HEARTBEAT_FILE", "");
  config.InputRecordPath =
      ReadCString(environment, "MELONDS_NSML_INPUT_RECORD_FILE", "");
  if (!config.InputRecordPath.empty()) {
    config.InputRecordStartFrame = static_cast<std::uint32_t>(std::max(
        0, ReadInt(environment, "MELONDS_NSML_INPUT_RECORD_START_FRAME", 0)));
    config.InputRecordEndFrame = static_cast<std::uint32_t>(std::max(
        0, ReadInt(environment, "MELONDS_NSML_INPUT_RECORD_END_FRAME", 0)));
    if (config.InputRecordEndFrame != 0 &&
        config.InputRecordEndFrame < config.InputRecordStartFrame)
      config.InputRecordEndFrame = config.InputRecordStartFrame;
    config.InputRecordInstance =
        ReadInt(environment, "MELONDS_NSML_INPUT_RECORD_INSTANCE", -1);
    if (config.InputRecordInstance < 0 || config.InputRecordInstance >= 16)
      config.InputRecordInstance = -1;
  }
  config.ScreenHashEnabled = ReadFlag(environment, "MELONDS_NSML_SCREEN_HASH");
  config.HashLogPath = ReadCString(environment, "MELONDS_NSML_HASH_LOG", "");
  config.ScreenshotDir =
      ReadCString(environment, "MELONDS_NSML_SCREENSHOT_DIR", "");
  config.ScreenshotInterval =
      std::max(0, ReadInt(environment, "MELONDS_NSML_SCREENSHOT_INTERVAL", 0));
  config.ScreenshotStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_SCREENSHOT_START_FRAME", 0)));
  config.ScreenshotEndFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_SCREENSHOT_END_FRAME", 0)));
  if (config.ScreenshotEndFrame != 0 &&
      config.ScreenshotEndFrame < config.ScreenshotStartFrame)
    config.ScreenshotEndFrame = config.ScreenshotStartFrame;
  config.ScreenshotRegisterTrace =
      ReadFlag(environment, "MELONDS_NSML_SCREENSHOT_REG_TRACE");
  config.RamDumpDir = ReadCString(environment, "MELONDS_NSML_RAM_DUMP_DIR", "");
  config.RamDumpInterval =
      std::max(0, ReadInt(environment, "MELONDS_NSML_RAM_DUMP_INTERVAL", 0));
  config.RamDumpFrames =
      ReadCString(environment, "MELONDS_NSML_RAM_DUMP_FRAMES", "");
  config.GameStateTracePath =
      ReadCString(environment, "MELONDS_NSML_GAME_STATE_TRACE", "");
  config.DiagnosticsPath =
      ReadCString(environment, "MELONDS_NSML_DIAGNOSTICS_FILE", "");
  config.DiagnosticEventsPath =
      ReadCString(environment, "MELONDS_NSML_DIAGNOSTIC_EVENTS_FILE", "");
  config.DiagnosticEventsEnabled =
      ReadFlag(environment, "MELONDS_NSML_DIAGNOSTIC_EVENTS") ||
      !config.DiagnosticEventsPath.empty();
  if (ReadFlag(environment, "MELONDS_NSML_DIAGNOSTIC_EVENTS_DISABLE"))
    config.DiagnosticEventsEnabled = false;
  config.DiagnosticRingFrames = std::clamp(
      ReadInt(environment, "MELONDS_NSML_DIAGNOSTIC_RING_FRAMES", 360), 60,
      std::max(60, diagnosticRingCapacity));
  if (config.DiagnosticEventsEnabled && config.DiagnosticEventsPath.empty() &&
      !config.DiagnosticsPath.empty()) {
    std::filesystem::path eventsPath(config.DiagnosticsPath);
    eventsPath.replace_filename("melonds-events.jsonl");
    config.DiagnosticEventsPath = eventsPath.string();
  }
  config.GameStateTraceInterval = std::max(
      1, ReadInt(environment, "MELONDS_NSML_GAME_STATE_TRACE_INTERVAL", 60));
  config.GameStateTraceStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_GAME_STATE_TRACE_START_FRAME", 0)));
  config.GameStateTraceEndFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_GAME_STATE_TRACE_END_FRAME", 0)));
  config.GameStateTraceExtended =
      ReadFlag(environment, "MELONDS_NSML_GAME_STATE_TRACE_EXTENDED");
  config.AIPlayLogPath =
      ReadCString(environment, "MELONDS_NSML_AI_PLAY_LOG", "");
  config.AIObservationV2Path =
      ReadCString(environment, "MELONDS_NSML_AI_OBSERVATION_V2_LOG", "");
  config.AIObservationV3Path =
      ReadCString(environment, "MELONDS_NSML_AI_OBSERVATION_V3_LOG", "");
  config.AIPlayLogInterval =
      std::max(1, ReadInt(environment, "MELONDS_NSML_AI_PLAY_LOG_INTERVAL", 1));
  config.AIPlayLogFlushInterval = std::max(
      0, ReadInt(environment, "MELONDS_NSML_AI_PLAY_LOG_FLUSH_INTERVAL", 60));
  config.AIPlayLogStartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_AI_PLAY_LOG_START_FRAME", 0)));
  config.AIPlayLogEndFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_AI_PLAY_LOG_END_FRAME", 0)));
  config.AIPlayLogMaxObjects = std::clamp(
      ReadInt(environment, "MELONDS_NSML_AI_PLAY_LOG_MAX_OBJECTS", 32), 0, 256);
  config.AIObservationV2StageFilter =
      ReadHasValue(environment, "MELONDS_NSML_AI_OBSERVATION_V2_STAGE_FILTER")
          ? std::clamp(ReadInt(environment,
                               "MELONDS_NSML_AI_OBSERVATION_V2_STAGE_FILTER",
                               -1),
                       -1, 4)
          : -1;
  config.AIObservationV3StageFilter =
      ReadHasValue(environment, "MELONDS_NSML_AI_OBSERVATION_V3_STAGE_FILTER")
          ? std::clamp(ReadInt(environment,
                               "MELONDS_NSML_AI_OBSERVATION_V3_STAGE_FILTER",
                               -1),
                       -1, 4)
          : -1;
  config.AIPlayLogGameplayOnly =
      !ReadFlag(environment, "MELONDS_NSML_AI_PLAY_LOG_INCLUDE_NON_GAMEPLAY");
  return config;
}

DiagnosticsConfig LoadDiagnosticsConfig(int diagnosticRingCapacity) {
  return LoadDiagnosticsConfig(GetProcessEnvironment(), diagnosticRingCapacity);
}

AIConfig LoadAIConfig(const Environment &environment) {
  AIConfig config;
  config.Rule.Enabled = ReadFlag(environment, "MELONDS_NSML_RULE_AI");
  config.Rule.HostOnly =
      ReadFlag(environment, "MELONDS_NSML_RULE_AI_HOST_ONLY");
  config.Rule.ClientOnly =
      ReadFlag(environment, "MELONDS_NSML_RULE_AI_CLIENT_ONLY");
  config.Rule.PlayerSpec =
      ReadCString(environment, "MELONDS_NSML_RULE_AI_PLAYER", "remote");
  config.Rule.StartFrame = static_cast<std::uint32_t>(
      std::max(0, ReadInt(environment, "MELONDS_NSML_RULE_AI_START_FRAME", 0)));
  config.Rule.HorizontalDeadzone = std::clamp(
      ReadInt(environment, "MELONDS_NSML_RULE_AI_HORIZONTAL_DEADZONE", 0x4000),
      0, 0x200000);
  config.Rule.HorizontalWrapWidth = std::clamp(
      ReadInt(environment, "MELONDS_NSML_RULE_AI_WRAP_WIDTH", 0x400000), 0,
      0x800000);
  config.Rule.CloseRange = std::clamp(
      ReadInt(environment, "MELONDS_NSML_RULE_AI_CLOSE_RANGE", 0x22000), 0x1000,
      0x200000);
  config.Rule.HazardHorizontalRange = std::clamp(
      ReadInt(environment, "MELONDS_NSML_RULE_AI_HAZARD_HORIZONTAL_RANGE",
              0x40000),
      0, 0x200000);
  config.Rule.HazardVerticalRange =
      std::clamp(ReadInt(environment,
                         "MELONDS_NSML_RULE_AI_HAZARD_VERTICAL_RANGE", 0x50000),
                 0, 0x200000);
  config.Rule.JumpInterval = std::clamp(
      ReadInt(environment, "MELONDS_NSML_RULE_AI_JUMP_INTERVAL", 42), 1, 600);
  config.Rule.JumpFrames =
      std::clamp(ReadInt(environment, "MELONDS_NSML_RULE_AI_JUMP_FRAMES", 9), 0,
                 config.Rule.JumpInterval);
  config.Rule.TraceEnabled =
      ReadFlag(environment, "MELONDS_NSML_RULE_AI_TRACE");
  config.Rule.TraceInterval = std::max(
      1, ReadInt(environment, "MELONDS_NSML_RULE_AI_TRACE_INTERVAL", 60));

  config.Imitation.Enabled = ReadFlag(environment, "MELONDS_NSML_IMITATION_AI");
  config.Imitation.HostOnly =
      ReadFlag(environment, "MELONDS_NSML_IMITATION_AI_HOST_ONLY");
  config.Imitation.ClientOnly =
      ReadFlag(environment, "MELONDS_NSML_IMITATION_AI_CLIENT_ONLY");
  config.Imitation.PlayerSpec =
      ReadCString(environment, "MELONDS_NSML_IMITATION_AI_PLAYER", "remote");
  config.Imitation.StartFrame = static_cast<std::uint32_t>(std::max(
      0, ReadInt(environment, "MELONDS_NSML_IMITATION_AI_START_FRAME", 0)));
  config.Imitation.Threshold = std::clamp(
      ReadDouble(environment, "MELONDS_NSML_IMITATION_AI_THRESHOLD", 0.5), 0.0,
      1.0);
  config.Imitation.AllowedHeldMask =
      ReadU32(environment, "MELONDS_NSML_IMITATION_AI_ALLOWED_HELD_MASK",
              0x8F3) &
      0x0FFFu;
  config.Imitation.HazardGuardEnabled =
      ReadInt(environment, "MELONDS_NSML_IMITATION_AI_HAZARD_GUARD", 1) != 0 &&
      !ReadFlag(environment, "MELONDS_NSML_IMITATION_AI_DISABLE_HAZARD_GUARD");
  config.Imitation.HazardGuardHorizontalRange = std::clamp(
      ReadInt(environment,
              "MELONDS_NSML_IMITATION_AI_HAZARD_GUARD_HORIZONTAL_RANGE",
              0x40000),
      0, 0x200000);
  config.Imitation.HazardGuardVerticalRange = std::clamp(
      ReadInt(environment,
              "MELONDS_NSML_IMITATION_AI_HAZARD_GUARD_VERTICAL_RANGE", 0x50000),
      0, 0x200000);
  config.Imitation.HazardGuardCloseRange = std::clamp(
      ReadInt(environment, "MELONDS_NSML_IMITATION_AI_HAZARD_GUARD_CLOSE_RANGE",
              0x10000),
      0, config.Imitation.HazardGuardHorizontalRange);
  config.Imitation.TraceEnabled =
      ReadFlag(environment, "MELONDS_NSML_IMITATION_AI_TRACE");
  config.Imitation.TraceInterval = std::max(
      1, ReadInt(environment, "MELONDS_NSML_IMITATION_AI_TRACE_INTERVAL", 60));
  config.Imitation.InferInterval = std::clamp(
      ReadInt(environment, "MELONDS_NSML_IMITATION_AI_INFER_INTERVAL", 16), 1,
      30);
  config.Imitation.NeutralHoldFrames = std::clamp(
      ReadInt(environment, "MELONDS_NSML_IMITATION_AI_NEUTRAL_HOLD_FRAMES", 8),
      0, 120);
  config.Imitation.WarnMissingFeatures = !ReadFlag(
      environment, "MELONDS_NSML_IMITATION_AI_DISABLE_FEATURE_WARNING");
  config.Imitation.ModelPath =
      ReadCString(environment, "MELONDS_NSML_IMITATION_AI_MODEL", "");
  return config;
}

AIConfig LoadAIConfig() { return LoadAIConfig(GetProcessEnvironment()); }

StateSyncConfig LoadStateSyncConfig(const Environment &environment) {
  StateSyncConfig config;
  config.GameEnabled = ReadFlag(environment, "MELONDS_NSML_STATE_SYNC");
  config.GameExtended =
      ReadFlag(environment, "MELONDS_NSML_STATE_SYNC_EXTENDED");
  config.GameApplyEnabled = ReadFlag(environment, "MELONDS_NSML_STATE_APPLY");

  const std::string applyMode =
      ReadCString(environment, "MELONDS_NSML_STATE_APPLY_MODE", "");
  if (applyMode == "critical") {
    config.GameApplyStageObjects = false;
    config.GameApplyPlayerActors = false;
  } else if (applyMode == "globals") {
    config.GameApplyStarObjects = false;
    config.GameApplyStageObjects = false;
    config.GameApplyPlayerActors = false;
  } else if (applyMode == "objects") {
    config.GameApplyCriticalGlobals = false;
  } else if (applyMode == "remote-player") {
    config.GameApplyCriticalGlobals = false;
    config.GameApplyStarObjects = false;
    config.GameApplyStageObjects = false;
    config.GameApplyRemotePlayerOnly = true;
  }
  config.GameInterval =
      std::max(1, ReadInt(environment, "MELONDS_NSML_STATE_SYNC_INTERVAL", 60));

  config.WorldTraceMovingHazards =
      ReadFlag(environment, "MELONDS_NSML_WORLD_STATE_TRACE_MOVING_HAZARDS");
  config.WorldTraceObjectLifecycles =
      ReadFlag(environment, "MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES");
  config.WorldTraceActorInternals =
      ReadFlag(environment, "MELONDS_NSML_WORLD_STATE_TRACE_ACTOR_INTERNALS");
  config.WorldTraceEffects =
      ReadFlag(environment, "MELONDS_NSML_WORLD_STATE_TRACE_EFFECTS");
  config.WorldTraceObjectLifecyclesInterval = std::max(
      1,
      ReadInt(environment,
              "MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES_INTERVAL", 60));
  config.WorldTraceObjectLifecyclesStartFrame =
      static_cast<std::uint32_t>(std::max(
          0, ReadInt(
                 environment,
                 "MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES_START_FRAME",
                 0)));
  config.WorldTraceObjectLifecyclesEndFrame =
      static_cast<std::uint32_t>(std::max(
          0,
          ReadInt(environment,
                  "MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES_END_FRAME",
                  0)));
  return config;
}

StateSyncConfig LoadStateSyncConfig() {
  return LoadStateSyncConfig(GetProcessEnvironment());
}

bool EnvFlag(const char *name) {
  return ReadFlag(GetProcessEnvironment(), name);
}

const char *EnvCString(const char *name, const char *fallback) {
  return ReadCString(GetProcessEnvironment(), name, fallback);
}

int EnvInt(const char *name, int fallback) {
  return ReadInt(GetProcessEnvironment(), name, fallback);
}

double EnvDouble(const char *name, double fallback) {
  return ReadDouble(GetProcessEnvironment(), name, fallback);
}

std::uint32_t EnvU32(const char *name, std::uint32_t fallback) {
  return ReadU32(GetProcessEnvironment(), name, fallback);
}

bool EnvHasValue(const char *name) {
  return ReadHasValue(GetProcessEnvironment(), name);
}

} // namespace NsmbMvlNetplay::Config
