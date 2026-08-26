#include "NsmbNetplayConfig.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {

int Failures = 0;

class MapEnvironment final : public NsmbMvlNetplay::Config::Environment {
public:
  const char *Get(const char *name) const override {
    const auto it = Values.find(name);
    return it == Values.end() ? nullptr : it->second.c_str();
  }

  std::map<std::string, std::string> Values;
};

void Check(bool condition, const char *expression, int line) {
  if (condition)
    return;
  std::fprintf(stderr, "line %d: CHECK failed: %s\n", line, expression);
  Failures++;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void TestFlagsPreserveExistingSemantics() {
  using NsmbMvlNetplay::Config::ParseFlag;
  CHECK(!ParseFlag(nullptr));
  CHECK(!ParseFlag(""));
  CHECK(!ParseFlag("0"));
  CHECK(ParseFlag("1"));
  CHECK(ParseFlag("false"));
  CHECK(ParseFlag("00"));
}

void TestStringsPreserveFallbackSemantics() {
  using NsmbMvlNetplay::Config::HasValue;
  using NsmbMvlNetplay::Config::ValueOr;
  CHECK(std::strcmp(ValueOr(nullptr, "fallback"), "fallback") == 0);
  CHECK(std::strcmp(ValueOr("", "fallback"), "fallback") == 0);
  CHECK(std::strcmp(ValueOr("value", "fallback"), "value") == 0);
  CHECK(!HasValue(nullptr));
  CHECK(!HasValue(""));
  CHECK(HasValue("0"));
}

void TestIntegerParsingPreservesBaseAndFallback() {
  using NsmbMvlNetplay::Config::ParseInt;
  CHECK(ParseInt(nullptr, 17) == 17);
  CHECK(ParseInt("", 17) == 17);
  CHECK(ParseInt("invalid", 17) == 17);
  CHECK(ParseInt("42", 0) == 42);
  CHECK(ParseInt("-9", 0) == -9);
  CHECK(ParseInt("0x2A", 0) == 42);
  CHECK(ParseInt("052", 0) == 42);
  CHECK(ParseInt("12trailing", 0) == 12);
}

void TestDoubleParsingPreservesFallback() {
  using NsmbMvlNetplay::Config::ParseDouble;
  CHECK(ParseDouble(nullptr, 1.5) == 1.5);
  CHECK(ParseDouble("", 1.5) == 1.5);
  CHECK(ParseDouble("invalid", 1.5) == 1.5);
  CHECK(std::abs(ParseDouble("2.25", 0.0) - 2.25) < 0.000001);
  CHECK(std::abs(ParseDouble("3.5ms", 0.0) - 3.5) < 0.000001);
}

void TestUnsignedParsingPreservesExistingInvalidValueBehavior() {
  using NsmbMvlNetplay::Config::ParseU32;
  CHECK(ParseU32(nullptr, 99) == 99u);
  CHECK(ParseU32("", 99) == 99u);
  CHECK(ParseU32("0xFFFFFFFF", 0) == UINT32_MAX);
  CHECK(ParseU32("invalid", 99) == 0u);
}

void TestUnsignedListParsingPreservesLegacySemantics() {
  using NsmbMvlNetplay::Config::ParseU32List;
  CHECK(ParseU32List(nullptr).empty());
  CHECK(ParseU32List("").empty());
  const auto values = ParseU32List(" 1, 0x10, invalid, , 03 ");
  CHECK(values.size() == 4);
  CHECK(values[0] == 1u);
  CHECK(values[1] == 16u);
  CHECK(values[2] == 0u);
  CHECK(values[3] == 3u);
}

void TestBootstrapConfigDefaults() {
  const MapEnvironment environment;
  const auto config = NsmbMvlNetplay::Config::LoadBootstrapConfig(environment);
  CHECK(!config.Enabled);
  CHECK(!config.TestEnabled);
  CHECK(config.TestFrames == 0u);
  CHECK(config.TestInstanceCount == 1);
  CHECK(config.HashEnabled);
  CHECK(config.HashInterval == 60);
  CHECK(config.WaitTimeoutMs == 60000);
  CHECK(config.QuitGraceMs == 0);
  CHECK(!config.InputTraceEnabled);
  CHECK(config.InputTraceInterval == 60);
}

void TestBootstrapConfigReadsAndClampsEnvironment() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_NETPLAY", "1"},
      {"MELONDS_NSML_TEST", "1"},
      {"MELONDS_NSML_TEST_FRAMES", "-20"},
      {"MELONDS_NSML_TEST_INSTANCES", "99"},
      {"MELONDS_NSML_DISABLE_HASH", "1"},
      {"MELONDS_NSML_HASH_INTERVAL", "0"},
      {"MELONDS_NSML_WAIT_TIMEOUT_MS", "-1"},
      {"MELONDS_NSML_QUIT_GRACE_MS", "250"},
      {"MELONDS_NSML_INPUT_TRACE", "trace"},
      {"MELONDS_NSML_INPUT_TRACE_INTERVAL", "0"},
  };

  const auto config = NsmbMvlNetplay::Config::LoadBootstrapConfig(environment);
  CHECK(config.Enabled);
  CHECK(config.TestEnabled);
  CHECK(config.TestFrames == 0u);
  CHECK(config.TestInstanceCount == 16);
  CHECK(!config.HashEnabled);
  CHECK(config.HashInterval == 1);
  CHECK(config.WaitTimeoutMs == 0);
  CHECK(config.QuitGraceMs == 250);
  CHECK(config.InputTraceEnabled);
  CHECK(config.InputTraceInterval == 1);
}

void TestBootstrapConfigSupportsLegacyEnableAlias() {
  MapEnvironment environment;
  environment.Values = {{"MELONDS_NSML_POC", "1"}};
  CHECK(NsmbMvlNetplay::Config::LoadBootstrapConfig(environment).Enabled);

  environment.Values["MELONDS_NSML_NETPLAY"] = "0";
  CHECK(!NsmbMvlNetplay::Config::LoadBootstrapConfig(environment).Enabled);

  environment.Values["MELONDS_NSML_NETPLAY"] = "1";
  environment.Values["MELONDS_NSML_POC"] = "0";
  CHECK(NsmbMvlNetplay::Config::LoadBootstrapConfig(environment).Enabled);
}

void TestConnectionConfigDefaultsAndRoleFallback() {
  MapEnvironment environment;
  auto config =
      NsmbMvlNetplay::Config::LoadConnectionConfig(environment, false);
  CHECK(!config.Client);
  CHECK(config.Delay == 6);
  CHECK(config.WarmupFrames == 0);
  CHECK(config.Port == 8065);
  CHECK(config.LocalInstance == 0);
  CHECK(config.StartFrame == 0u);
  CHECK(config.LocalWaitsForRemote);
  CHECK(!config.RemoteInputTimeoutFatal);
  CHECK(config.PeerHost == "127.0.0.1");

  config = NsmbMvlNetplay::Config::LoadConnectionConfig(environment, true);
  CHECK(config.WarmupFrames == 12);

  environment.Values["MELONDS_NSML_LAN_ROLE"] = "client";
  config = NsmbMvlNetplay::Config::LoadConnectionConfig(environment, false);
  CHECK(config.Client);
  CHECK(config.LocalInstance == 1);

  environment.Values["MELONDS_NSML_ROLE"] = "host";
  config = NsmbMvlNetplay::Config::LoadConnectionConfig(environment, false);
  CHECK(!config.Client);
  CHECK(config.LocalInstance == 0);
}

void TestConnectionConfigReadsExistingValuesAndClamps() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_ROLE", "client"},
      {"MELONDS_NSML_DELAY", "-3"},
      {"MELONDS_NSML_NETPLAY_WARMUP_FRAMES", "-2"},
      {"MELONDS_NSML_PORT", "9000"},
      {"MELONDS_NSML_LOCAL_INSTANCE", "7"},
      {"MELONDS_NSML_NETPLAY_START_FRAME", "-5"},
      {"MELONDS_NSML_NO_LOCAL_WAIT", "1"},
      {"MELONDS_NSML_REMOTE_INPUT_TIMEOUT_FATAL", "1"},
      {"MELONDS_NSML_PEER", "192.0.2.10"},
  };

  const auto config =
      NsmbMvlNetplay::Config::LoadConnectionConfig(environment, true);
  CHECK(config.Client);
  CHECK(config.Delay == 0);
  CHECK(config.WarmupFrames == 0);
  CHECK(config.Port == 9000);
  CHECK(config.LocalInstance == 7);
  CHECK(config.StartFrame == 0u);
  CHECK(!config.LocalWaitsForRemote);
  CHECK(config.RemoteInputTimeoutFatal);
  CHECK(config.PeerHost == "192.0.2.10");
}

void TestInputConfigDefaultsPreserveLegacyInitializationOrder() {
  MapEnvironment environment;
  auto config = NsmbMvlNetplay::Config::LoadInputConfig(environment, false);
  CHECK(config.SendDelayFrames == 0);
  CHECK(config.SendJitterFrames == 0);
  CHECK(config.SendDelayStartFrame == 0u);
  CHECK(config.SendDelayEndFrame == 0u);
  CHECK(!config.UseHistoryBundle);
  CHECK(config.BundleHistory == 0);
  CHECK(config.DropModulo == 0);
  CHECK(config.DropOffset == 0);
  CHECK(config.DropStartFrame == 0u);
  CHECK(config.DropEndFrame == 0u);
  CHECK(config.MaxFrameLead == -1);
  CHECK(!config.NetplayOnly);
  CHECK(!config.NetplayTrace);
  CHECK(!config.HealthTrace);
  CHECK(config.HealthTraceInterval == 120);
  CHECK(config.HealthTraceWaitThresholdMs == 16);
  CHECK(config.WaitPollUs == 100);

  environment.Values["MELONDS_NSML_INPUT_NETPLAY_ONLY"] = "1";
  config = NsmbMvlNetplay::Config::LoadInputConfig(environment, false);
  CHECK(config.NetplayOnly);
  CHECK(config.MaxFrameLead == -1);
  config = NsmbMvlNetplay::Config::LoadInputConfig(environment, true);
  CHECK(config.MaxFrameLead == 2);
}

void TestInputConfigReadsClampsAndNormalizesRanges() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_INPUT_SEND_DELAY_FRAMES", "-2"},
      {"MELONDS_NSML_INPUT_SEND_JITTER_FRAMES", "4"},
      {"MELONDS_NSML_INPUT_SEND_DELAY_START_FRAME", "100"},
      {"MELONDS_NSML_INPUT_SEND_DELAY_END_FRAME", "90"},
      {"MELONDS_NSML_INPUT_UNRELIABLE", "1"},
      {"MELONDS_NSML_INPUT_BUNDLE_HISTORY", "99"},
      {"MELONDS_NSML_INPUT_DROP_MODULO", "11"},
      {"MELONDS_NSML_INPUT_DROP_OFFSET", "25"},
      {"MELONDS_NSML_INPUT_DROP_START_FRAME", "200"},
      {"MELONDS_NSML_INPUT_DROP_END_FRAME", "150"},
      {"MELONDS_NSML_INPUT_MAX_FRAME_LEAD", "7"},
      {"MELONDS_NSML_INPUT_NETPLAY_TRACE", "1"},
      {"MELONDS_NSML_INPUT_HEALTH_TRACE", "1"},
      {"MELONDS_NSML_INPUT_HEALTH_TRACE_INTERVAL", "0"},
      {"MELONDS_NSML_INPUT_HEALTH_TRACE_WAIT_THRESHOLD_MS", "9000"},
      {"MELONDS_NSML_INPUT_WAIT_POLL_US", "1"},
  };

  const auto config =
      NsmbMvlNetplay::Config::LoadInputConfig(environment, false);
  CHECK(config.SendDelayFrames == 0);
  CHECK(config.SendJitterFrames == 4);
  CHECK(config.SendDelayStartFrame == 100u);
  CHECK(config.SendDelayEndFrame == 100u);
  CHECK(config.UseHistoryBundle);
  CHECK(config.BundleHistory == 31);
  CHECK(config.DropModulo == 11);
  CHECK(config.DropOffset == 3);
  CHECK(config.DropStartFrame == 200u);
  CHECK(config.DropEndFrame == 200u);
  CHECK(config.MaxFrameLead == 7);
  CHECK(config.NetplayTrace);
  CHECK(config.HealthTrace);
  CHECK(config.HealthTraceInterval == 1);
  CHECK(config.HealthTraceWaitThresholdMs == 5000);
  CHECK(config.WaitPollUs == 50);
}

void TestRuntimePatchConfigDefaults() {
  const MapEnvironment environment;
  const auto config =
      NsmbMvlNetplay::Config::LoadRuntimePatchConfig(environment);
  CHECK(config.PlayerStickToStarStartFrame == 0u);
  CHECK(config.PlayerStickToStarEndFrame == 0u);
  CHECK(config.PlayerStickToStarSlot == 0);
  CHECK(!config.ForcePlayerDeathCountersEnabled);
  CHECK(!config.ForcePlayerLivesEnabled);
  CHECK(config.ForcePlayerLife0 == 5u);
  CHECK(config.ForcePlayerLife1 == 5u);
  CHECK(!config.ForcePlayerPowerupsEnabled);
  CHECK(!config.ForcePlayerInventoryPowerupsEnabled);
  CHECK(!config.ForcePlayerCoinsEnabled);
  CHECK(!config.ForcePlayerStarCountersEnabled);
  CHECK(!config.TracePlayerLifeChanges);
  CHECK(!config.PacketBridgeJitHelperPatchEnabled);
}

void TestRuntimePatchConfigReadsAndClampsEnvironment() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_PLAYER_STICK_TO_STAR_START_FRAME", "20"},
      {"MELONDS_NSML_PLAYER_STICK_TO_STAR_END_FRAME", "10"},
      {"MELONDS_NSML_PLAYER_STICK_TO_STAR_SLOT", "3"},
      {"MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_HOST_ONLY", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_CLIENT_ONLY", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_START_FRAME", "-1"},
      {"MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTERS_END_FRAME", "22"},
      {"MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTER0", "3"},
      {"MELONDS_NSML_FORCE_PLAYER_DEATH_COUNTER1", "4"},
      {"MELONDS_NSML_FORCE_PLAYER_LIVES", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_LIFE0", "-5"},
      {"MELONDS_NSML_FORCE_PLAYER_LIFE1", "7"},
      {"MELONDS_NSML_FORCE_PLAYER_POWERUPS", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_POWERUPS_START_FRAME", "30"},
      {"MELONDS_NSML_FORCE_PLAYER_POWERUPS_END_FRAME", "40"},
      {"MELONDS_NSML_FORCE_PLAYER_POWERUP0", "2"},
      {"MELONDS_NSML_FORCE_PLAYER_POWERUP1", "4"},
      {"MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUPS", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUPS_START_FRAME", "50"},
      {"MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUPS_END_FRAME", "60"},
      {"MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUP0", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_INVENTORY_POWERUP1", "5"},
      {"MELONDS_NSML_FORCE_PLAYER_COINS", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_COINS_START_FRAME", "61"},
      {"MELONDS_NSML_FORCE_PLAYER_COINS_END_FRAME", "69"},
      {"MELONDS_NSML_FORCE_PLAYER_COINS0", "8"},
      {"MELONDS_NSML_FORCE_PLAYER_COINS1", "-1"},
      {"MELONDS_NSML_FORCE_PLAYER_STAR_COUNTERS", "1"},
      {"MELONDS_NSML_FORCE_PLAYER_STAR_COUNTERS_START_FRAME", "70"},
      {"MELONDS_NSML_FORCE_PLAYER_STAR_COUNTERS_END_FRAME", "80"},
      {"MELONDS_NSML_FORCE_PLAYER_BATTLE_STARS0", "6"},
      {"MELONDS_NSML_FORCE_PLAYER_BATTLE_STARS1", "7"},
      {"MELONDS_NSML_FORCE_PLAYER_DISPLAYED_STARS0", "8"},
      {"MELONDS_NSML_FORCE_PLAYER_DISPLAYED_STARS1", "9"},
      {"MELONDS_NSML_FORCE_PLAYER_COLLECTED_STARS0", "10"},
      {"MELONDS_NSML_FORCE_PLAYER_COLLECTED_STARS1", "11"},
      {"MELONDS_NSML_TRACE_PLAYER_LIFE_CHANGES", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_JIT_HELPER_PATCH", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_JIT_HELPER_PATCH_FRAME", "-1"},
  };

  const auto config =
      NsmbMvlNetplay::Config::LoadRuntimePatchConfig(environment);
  CHECK(config.PlayerStickToStarStartFrame == 10u);
  CHECK(config.PlayerStickToStarEndFrame == 20u);
  CHECK(config.PlayerStickToStarSlot == 1);
  CHECK(config.ForcePlayerDeathCountersEnabled);
  CHECK(config.ForcePlayerDeathCountersHostOnly);
  CHECK(config.ForcePlayerDeathCountersClientOnly);
  CHECK(config.ForcePlayerDeathCountersStartFrame == 0u);
  CHECK(config.ForcePlayerDeathCountersEndFrame == 22u);
  CHECK(config.ForcePlayerDeathCounter0 == 3u);
  CHECK(config.ForcePlayerDeathCounter1 == 4u);
  CHECK(config.ForcePlayerLivesEnabled);
  CHECK(config.ForcePlayerLife0 == 0u);
  CHECK(config.ForcePlayerLife1 == 7u);
  CHECK(config.ForcePlayerPowerupsEnabled);
  CHECK(config.ForcePlayerPowerupsStartFrame == 30u);
  CHECK(config.ForcePlayerPowerupsEndFrame == 40u);
  CHECK(config.ForcePlayerPowerup0 == 2u);
  CHECK(config.ForcePlayerPowerup1 == 4u);
  CHECK(config.ForcePlayerInventoryPowerupsEnabled);
  CHECK(config.ForcePlayerInventoryPowerupsStartFrame == 50u);
  CHECK(config.ForcePlayerInventoryPowerupsEndFrame == 60u);
  CHECK(config.ForcePlayerInventoryPowerup0 == 1u);
  CHECK(config.ForcePlayerInventoryPowerup1 == 5u);
  CHECK(config.ForcePlayerCoinsEnabled);
  CHECK(config.ForcePlayerCoinsStartFrame == 61u);
  CHECK(config.ForcePlayerCoinsEndFrame == 69u);
  CHECK(config.ForcePlayerCoins0 == 7u);
  CHECK(config.ForcePlayerCoins1 == 0u);
  CHECK(config.ForcePlayerStarCountersEnabled);
  CHECK(config.ForcePlayerStarCountersStartFrame == 70u);
  CHECK(config.ForcePlayerStarCountersEndFrame == 80u);
  CHECK(config.ForcePlayerBattleStars0 == 6u);
  CHECK(config.ForcePlayerBattleStars1 == 7u);
  CHECK(config.ForcePlayerDisplayedStars0 == 8u);
  CHECK(config.ForcePlayerDisplayedStars1 == 9u);
  CHECK(config.ForcePlayerCollectedStars0 == 10u);
  CHECK(config.ForcePlayerCollectedStars1 == 11u);
  CHECK(config.TracePlayerLifeChanges);
  CHECK(config.PacketBridgeJitHelperPatchEnabled);
  CHECK(config.PacketBridgeJitHelperPatchFrame == 0u);
}

void TestHarnessConfigDefaults() {
  const MapEnvironment environment;
  const auto config = NsmbMvlNetplay::Config::LoadHarnessConfig(environment);
  CHECK(config.InputScriptPath.empty());
  CHECK(!config.FrameBarrierEnabled);
  CHECK(!config.SerialRunEnabled);
  CHECK(config.SeedWaitTimeoutMs == 10000);
  CHECK(!config.WaitForPeerBeforeStart);
  CHECK(!config.WaitForPeerAtNetplayStart);
  CHECK(!config.DeferNetworkUntilStart);
  CHECK(!config.NetplayFrameBarrierEnabled);
  CHECK(!config.NeutralizePolledInput);
  CHECK(!config.NeutralizePolledInputPreserveTouch);
  CHECK(!config.NetworkPumpThreadEnabled);
  CHECK(config.NetworkPumpSleepUs == 250);
  CHECK(config.StateSaveDir.empty());
  CHECK(config.StateSaveFrame == 0u);
  CHECK(config.StateLoadDir.empty());
  CHECK(config.StateLoadFrame == 0u);
  CHECK(!config.StateLoadFrameSet);
  CHECK(config.TestEmulationPauseFrame == 0u);
  CHECK(config.TestEmulationPauseDurationMs == 0);
  CHECK(config.TestEmulationPauseRole.empty());
}

void TestHarnessConfigReadsClampsAndPreservesPresence() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_INPUT_SCRIPT", "inputs.txt"},
      {"MELONDS_NSML_FRAME_BARRIER", "1"},
      {"MELONDS_NSML_SERIAL_RUN", "1"},
      {"MELONDS_NSML_SEED_WAIT_TIMEOUT_MS", "-1"},
      {"MELONDS_NSML_WAIT_FOR_PEER", "1"},
      {"MELONDS_NSML_WAIT_FOR_PEER_AT_NETPLAY_START", "1"},
      {"MELONDS_NSML_DEFER_NETWORK_UNTIL_START", "1"},
      {"MELONDS_NSML_NETPLAY_FRAME_BARRIER", "1"},
      {"MELONDS_NSML_NEUTRALIZE_POLLED_INPUT", "1"},
      {"MELONDS_NSML_NEUTRALIZE_POLLED_INPUT_PRESERVE_TOUCH", "1"},
      {"MELONDS_NSML_NET_PUMP_THREAD", "1"},
      {"MELONDS_NSML_NET_PUMP_SLEEP_US", "1"},
      {"MELONDS_NSML_STATE_SAVE_DIR", "save-dir"},
      {"MELONDS_NSML_STATE_SAVE_FRAME", "-3"},
      {"MELONDS_NSML_STATE_LOAD_DIR", "load-dir"},
      {"MELONDS_NSML_STATE_LOAD_FRAME", "invalid"},
      {"MELONDS_NSML_TEST_EMULATION_PAUSE_FRAME", "180"},
      {"MELONDS_NSML_TEST_EMULATION_PAUSE_MS", "99999"},
      {"MELONDS_NSML_TEST_EMULATION_PAUSE_ROLE", "client"},
  };

  auto config = NsmbMvlNetplay::Config::LoadHarnessConfig(environment);
  CHECK(config.InputScriptPath == "inputs.txt");
  CHECK(config.FrameBarrierEnabled);
  CHECK(config.SerialRunEnabled);
  CHECK(config.SeedWaitTimeoutMs == 0);
  CHECK(config.WaitForPeerBeforeStart);
  CHECK(config.WaitForPeerAtNetplayStart);
  CHECK(config.DeferNetworkUntilStart);
  CHECK(config.NetplayFrameBarrierEnabled);
  CHECK(config.NeutralizePolledInput);
  CHECK(config.NeutralizePolledInputPreserveTouch);
  CHECK(config.NetworkPumpThreadEnabled);
  CHECK(config.NetworkPumpSleepUs == 50);
  CHECK(config.StateSaveDir == "save-dir");
  CHECK(config.StateSaveFrame == 0u);
  CHECK(config.StateLoadDir == "load-dir");
  CHECK(config.StateLoadFrameSet);
  CHECK(config.StateLoadFrame == 0u);
  CHECK(config.TestEmulationPauseFrame == 180u);
  CHECK(config.TestEmulationPauseDurationMs == 10000);
  CHECK(config.TestEmulationPauseRole == "client");

  environment.Values["MELONDS_NSML_NET_PUMP_SLEEP_US"] = "99999";
  environment.Values["MELONDS_NSML_STATE_LOAD_FRAME"] = "84";
  config = NsmbMvlNetplay::Config::LoadHarnessConfig(environment);
  CHECK(config.NetworkPumpSleepUs == 5000);
  CHECK(config.StateLoadFrame == 84u);

  environment.Values["MELONDS_NSML_STATE_SAVE_FRAME"] = "0x10";
  environment.Values["MELONDS_NSML_STATE_LOAD_FRAME"] = "0x10";
  config = NsmbMvlNetplay::Config::LoadHarnessConfig(environment);
  CHECK(config.StateSaveFrame == 16u);
  CHECK(config.StateLoadFrame == 0u);
}

void TestPacketBridgeConfigDefaults() {
  const MapEnvironment environment;
  const auto config =
      NsmbMvlNetplay::Config::LoadPacketBridgeConfig(environment);
  CHECK(!config.Enabled);
  CHECK(!config.Only);
  CHECK(!config.AllowPreGame);
  CHECK(!config.TraceEnabled);
  CHECK(config.LocalPlayerOverride == -1);
  CHECK(!config.DirectCaptureEnabled);
  CHECK(!config.ForceTickEnabled);
  CHECK(config.ForceTickStartFrame == 0u);
  CHECK(config.ForceTickBase == -1);
  CHECK(config.ForceGameLocalPlayerID == -1);
  CHECK(config.ForceGameLocalPlayerIDStartFrame == 0u);
  CHECK(!config.ForceGameLocalPlayerIDEarly);
  CHECK(config.MaxFrameLead == -1);
  CHECK(config.ThrottleTimeoutMs == 5000);
  CHECK(config.ThrottleStartFrame == 0u);
  CHECK(config.LocalInputDelay == 0);
  CHECK(!config.NeutralizeLocalInput);
  CHECK(!config.PreserveLocalTouch);
  CHECK(config.SendDelayFrames == 0);
  CHECK(config.SendJitterFrames == 0);
}

void TestPacketBridgeConfigReadsAndClampsEnvironment() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_PACKET_BRIDGE", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_ONLY", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_ALLOW_PRE_GAME", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_TRACE", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_LOCAL_PLAYER", "9"},
      {"MELONDS_NSML_PACKET_BRIDGE_DIRECT_CAPTURE", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_FORCE_TICK", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_FORCE_TICK_START_FRAME", "-3"},
      {"MELONDS_NSML_PACKET_BRIDGE_FORCE_TICK_BASE", "42"},
      {"MELONDS_NSML_PACKET_BRIDGE_FORCE_GAME_LOCAL_PLAYER_ID", "7"},
      {"MELONDS_NSML_PACKET_BRIDGE_FORCE_GAME_LOCAL_PLAYER_ID_START_FRAME",
       "-8"},
      {"MELONDS_NSML_PACKET_BRIDGE_FORCE_GAME_LOCAL_PLAYER_ID_EARLY", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_MAX_FRAME_LEAD", "10"},
      {"MELONDS_NSML_PACKET_BRIDGE_THROTTLE_TIMEOUT_MS", "-11"},
      {"MELONDS_NSML_PACKET_BRIDGE_THROTTLE_START_FRAME", "-12"},
      {"MELONDS_NSML_PACKET_BRIDGE_LOCAL_INPUT_DELAY", "-13"},
      {"MELONDS_NSML_PACKET_BRIDGE_NEUTRALIZE_LOCAL_INPUT", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_PRESERVE_LOCAL_TOUCH", "1"},
      {"MELONDS_NSML_PACKET_BRIDGE_SEND_DELAY_FRAMES", "-14"},
      {"MELONDS_NSML_PACKET_BRIDGE_SEND_JITTER_FRAMES", "15"},
  };

  auto config = NsmbMvlNetplay::Config::LoadPacketBridgeConfig(environment);
  CHECK(config.Enabled);
  CHECK(config.Only);
  CHECK(config.AllowPreGame);
  CHECK(config.TraceEnabled);
  CHECK(config.LocalPlayerOverride == 1);
  CHECK(config.DirectCaptureEnabled);
  CHECK(config.ForceTickEnabled);
  CHECK(config.ForceTickStartFrame == 0u);
  CHECK(config.ForceTickBase == 42);
  CHECK(config.ForceGameLocalPlayerID == 7);
  CHECK(config.ForceGameLocalPlayerIDStartFrame == 0u);
  CHECK(config.ForceGameLocalPlayerIDEarly);
  CHECK(config.MaxFrameLead == 10);
  CHECK(config.ThrottleTimeoutMs == 0);
  CHECK(config.ThrottleStartFrame == 0u);
  CHECK(config.LocalInputDelay == 0);
  CHECK(config.NeutralizeLocalInput);
  CHECK(config.PreserveLocalTouch);
  CHECK(config.SendDelayFrames == 0);
  CHECK(config.SendJitterFrames == 15);

  environment.Values["MELONDS_NSML_PACKET_BRIDGE_LOCAL_PLAYER"] = "";
  config = NsmbMvlNetplay::Config::LoadPacketBridgeConfig(environment);
  CHECK(config.LocalPlayerOverride == 0);
  environment.Values["MELONDS_NSML_PACKET_BRIDGE_LOCAL_PLAYER"] = "invalid";
  config = NsmbMvlNetplay::Config::LoadPacketBridgeConfig(environment);
  CHECK(config.LocalPlayerOverride == 0);
}

void TestRollbackConfigDefaultsAndBackendAliases() {
  using NsmbMvlNetplay::Config::RollbackBackend;
  MapEnvironment environment;
  auto config = NsmbMvlNetplay::Config::LoadRollbackConfig(environment);
  CHECK(!config.Enabled);
  CHECK(!config.Resimulate);
  CHECK(!config.SkipJitReset);
  CHECK(config.InputWaitUs == 0);
  CHECK(config.PredictionProbeModulo == 0);
  CHECK(config.PredictionProbeOffset == 0);
  CHECK(config.PredictionProbeLimit == -1);
  CHECK(config.PredictionProbeKeyMask == 1u);
  CHECK(config.PredictionProbeConfirmDelayFrames == 0);
  CHECK(config.Backend == RollbackBackend::Savestate);
  CHECK(config.Window == 20);
  CHECK(config.CheckpointInterval == 1);
  CHECK(config.DeltaKeyframeInterval == 10);
  CHECK(config.MainRAMPageSize == 4096);
  CHECK(config.PredictionHorizonFrames == 0);
  CHECK(config.PredictionHorizonTimeoutMs == 7000);
  CHECK(!config.PhaseRecoveryEnabled);

  const std::pair<const char *, RollbackBackend> aliases[] = {
      {"core-lite", RollbackBackend::CoreLite},
      {"coresparse", RollbackBackend::CoreSparse},
      {"core-delta", RollbackBackend::CoreDelta},
      {"coreframedelta", RollbackBackend::CoreFrameDelta},
      {"core-preimage", RollbackBackend::CorePreimage},
      {"tiny-core-preimage", RollbackBackend::TinyCorePreimage},
      {"rom-loop", RollbackBackend::RomLoop},
      {"slippi", RollbackBackend::RomLoop},
      {"nsmb-ranges", RollbackBackend::Savestate},
      {"ram", RollbackBackend::Savestate},
      {"unknown", RollbackBackend::Savestate},
  };
  for (const auto &alias : aliases) {
    environment.Values["MELONDS_NSML_ROLLBACK_BACKEND"] = alias.first;
    config = NsmbMvlNetplay::Config::LoadRollbackConfig(environment);
    CHECK(config.Backend == alias.second);
  }

  MapEnvironment legacyProbeEnvironment;
  legacyProbeEnvironment.Values[
      "MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_AFTER_ONE_FRAME"] = "1";
  config = NsmbMvlNetplay::Config::LoadRollbackConfig(legacyProbeEnvironment);
  CHECK(config.PredictionProbeConfirmDelayFrames == 1);
}

void TestRollbackConfigReadsClampsAndDependencies() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_ROLLBACK", "1"},
      {"MELONDS_NSML_ROLLBACK_RESIMULATE", "1"},
      {"MELONDS_NSML_ROLLBACK_RESIM_SKIP_RENDER", "1"},
      {"MELONDS_NSML_ROLLBACK_RESIM_SKIP_INTERMEDIATE_CHECKPOINTS", "1"},
      {"MELONDS_NSML_ROLLBACK_SKIP_JIT_RESET", "0"},
      {"MELONDS_NSML_ROLLBACK_INPUT_WAIT_US", "99999"},
      {"MELONDS_NSML_ROLLBACK_RESTORE_PROBE", "1"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_MODULO", "7"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_OFFSET", "20"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_LIMIT", "20000"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_START_FRAME", "100"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_END_FRAME", "90"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_KEY_MASK", "0"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_DELAY_FRAMES", "999"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_AFTER_ONE_FRAME", "1"},
      {"MELONDS_NSML_ROLLBACK_WINDOW", "999"},
      {"MELONDS_NSML_ROLLBACK_CHECKPOINT_INTERVAL", "0"},
      {"MELONDS_NSML_ROLLBACK_DELTA_KEYFRAME_INTERVAL", "99"},
      {"MELONDS_NSML_ROLLBACK_MAIN_RAM_PAGE_SIZE", "300"},
      {"MELONDS_NSML_ROLLBACK_CORE_SKIP_MASK", "99"},
      {"MELONDS_NSML_ROLLBACK_TINY_CORE_FLAGS", "9999"},
      {"MELONDS_NSML_ROLLBACK_RESIMULATE_DELAY_FRAMES", "99"},
      {"MELONDS_NSML_ROLLBACK_MAX_RESIM_FRAMES", "-1"},
      {"MELONDS_NSML_ROLLBACK_PREDICTION_HORIZON_FRAMES", "99"},
      {"MELONDS_NSML_ROLLBACK_HORIZON_TIMEOUT_MS", "1"},
      {"MELONDS_NSML_ROLLBACK_PHASE_RECOVERY", "1"},
  };

  const auto config = NsmbMvlNetplay::Config::LoadRollbackConfig(environment);
  CHECK(config.Enabled);
  CHECK(config.Resimulate);
  CHECK(config.SkipRenderDuringResim);
  CHECK(config.SkipIntermediateResimCheckpoints);
  CHECK(config.SkipJitReset);
  CHECK(config.InputWaitUs == 20000);
  CHECK(config.RestoreProbe);
  CHECK(config.PredictionProbeModulo == 7);
  CHECK(config.PredictionProbeOffset == 6);
  CHECK(config.PredictionProbeLimit == 10000);
  CHECK(config.PredictionProbeStartFrame == 100u);
  CHECK(config.PredictionProbeEndFrame == 100u);
  CHECK(config.PredictionProbeKeyMask == 1u);
  CHECK(config.PredictionProbeConfirmDelayFrames == 180);
  CHECK(config.Window == 180);
  CHECK(config.CheckpointInterval == 1);
  CHECK(config.DeltaKeyframeInterval == 60);
  CHECK(config.MainRAMPageSize == 4096);
  CHECK(config.CoreSkipMask == 31);
  CHECK(config.TinyCoreFlags == 2047);
  CHECK(config.ResimulateDelayFrames == 30);
  CHECK(config.MaxResimFrames == 0);
  CHECK(config.PredictionHorizonFrames == 10);
  CHECK(config.PredictionHorizonTimeoutMs == 100);
  CHECK(config.PhaseRecoveryEnabled);
}

void TestMvlConfigDefaults() {
  const MapEnvironment environment;
  const auto config = NsmbMvlNetplay::Config::LoadMvlConfig(environment);
  CHECK(config.Stage == 0);
  CHECK(config.StageSequence.empty());
  CHECK(config.StageSceneSettings == 0x00B4FF00u);
  CHECK(config.CourseMode == "fixed");
  CHECK(config.InvalidCourseMode.empty());
  CHECK(config.TargetWins == 2);
  CHECK(config.BigStarTarget == 5);
  CHECK(!config.RuntimeConfigEnabled);
  CHECK(config.InitialLives == 3u);
  CHECK(config.LifeModeSelector == 2u);
  CHECK(config.BigStarSelector == 1u);
  CHECK(!config.AutoRestartAfterResult);
  CHECK(config.AutoRestartDelayFrames == 120u);
  CHECK(config.AutoRestartBootstrapFrame == 120u);
  CHECK(!config.CameraInitHold.Enabled);
  CHECK(!config.CameraInitHold.HostOnly);
  CHECK(!config.CameraInitHold.ClientOnly);
  CHECK(config.CameraInitHold.StartFrame == 840u);
  CHECK(config.CameraInitHold.EndFrame == 0u);
  CHECK(!config.NetRandom.Enabled);
  CHECK(!config.NetRandom.Auto);
  CHECK(config.NetRandom.Frame == 0u);
  CHECK(config.NetRandom.Value == 0u);
  CHECK(!config.MatchSeedConfigured);
  CHECK(config.MatchSeed == 0u);
  CHECK(config.MatchSeedSequence.empty());
}

void TestMvlConfigReadsClampsAndPreservesPriority() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_MVL_STAGE", "-3"},
      {"MELONDS_NSML_MVL_STAGE_SEQUENCE", " 4, 9, invalid, , 2 "},
      {"MELONDS_NSML_MVL_COURSE_MODE", "unsupported"},
      {"MELONDS_NSML_MVL_WINS", "0"},
      {"MELONDS_NSML_MVL_BIG_STARS", "99"},
      {"MELONDS_NSML_MVL_LIVES", "5"},
      {"MELONDS_NSML_MVL_AUTO_RESTART_AFTER_RESULT", "1"},
      {"MELONDS_NSML_MVL_AUTO_RESTART_DELAY_FRAMES", "-1"},
      {"MELONDS_NSML_MVL_AUTO_RESTART_BOOTSTRAP_FRAME", "2000000"},
      {"MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD", "1"},
      {"MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_HOST_ONLY", "1"},
      {"MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_CLIENT_ONLY", "1"},
      {"MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_START_FRAME", "-4"},
      {"MELONDS_NSML_CLEAR_MVL_CAMERA_INIT_HOLD_END_FRAME", "-1"},
      {"MELONDS_NSML_NET_RANDOM_VALUE", "0x111"},
      {"MELONDS_NSML_NET_RANDOM_AUTO", "1"},
      {"MELONDS_NSML_NET_RANDOM_FRAME", "-5"},
      {"MELONDS_NSML_MATCH_SEED", "0x222"},
      {"MELONDS_NSML_MATCH_SEED_SEQUENCE", "0x333, 0x444"},
  };

  auto config = NsmbMvlNetplay::Config::LoadMvlConfig(environment);
  CHECK(config.StageSequence == std::vector<int>({4, 4, 0, 2}));
  CHECK(config.Stage == 4);
  CHECK(config.StageSceneSettings == 0x00B4FF00u);
  CHECK(config.CourseMode == "fixed");
  CHECK(config.InvalidCourseMode == "unsupported");
  CHECK(config.TargetWins == 1);
  CHECK(config.BigStarTarget == 10);
  CHECK(config.RuntimeConfigEnabled);
  CHECK(config.InitialLives == 5u);
  CHECK(config.LifeModeSelector == 0u);
  CHECK(config.BigStarSelector == 2u);
  CHECK(config.AutoRestartAfterResult);
  CHECK(config.AutoRestartDelayFrames == 1u);
  CHECK(config.AutoRestartBootstrapFrame == 1000000u);
  CHECK(config.CameraInitHold.Enabled);
  CHECK(config.CameraInitHold.HostOnly);
  CHECK(config.CameraInitHold.ClientOnly);
  CHECK(config.CameraInitHold.StartFrame == 0u);
  CHECK(config.CameraInitHold.EndFrame == 0u);
  CHECK(config.NetRandom.Enabled);
  CHECK(config.NetRandom.Auto);
  CHECK(config.NetRandom.Frame == 0u);
  CHECK(config.NetRandom.Value == 0x333u);
  CHECK(config.MatchSeedConfigured);
  CHECK(config.MatchSeed == 0x333u);
  CHECK(config.MatchSeedSequence ==
        std::vector<std::uint32_t>({0x333u, 0x444u}));

  environment.Values["MELONDS_NSML_MVL_SCENE_SETTINGS"] = "invalid";
  config = NsmbMvlNetplay::Config::LoadMvlConfig(environment);
  CHECK(config.StageSceneSettings == 0u);

  environment.Values.erase("MELONDS_NSML_MATCH_SEED_SEQUENCE");
  config = NsmbMvlNetplay::Config::LoadMvlConfig(environment);
  CHECK(config.MatchSeed == 0x222u);
  CHECK(config.NetRandom.Value == 0x111u);

  environment.Values["MELONDS_NSML_NET_RANDOM_VALUE"] = "";
  environment.Values.erase("MELONDS_NSML_MATCH_SEED");
  config = NsmbMvlNetplay::Config::LoadMvlConfig(environment);
  CHECK(!config.NetRandom.Enabled);
  CHECK(!config.NetRandom.Auto);
  CHECK(config.NetRandom.Frame == 0u);
  CHECK(!config.MatchSeedConfigured);
}

void TestDiagnosticsConfigDefaults() {
  const MapEnvironment environment;
  const auto config =
      NsmbMvlNetplay::Config::LoadDiagnosticsConfig(environment, 720);
  CHECK(!config.HangDiagnosticsEnabled);
  CHECK(config.HangWatchdogIntervalMs == 1000);
  CHECK(config.HangThresholdMs == 8000);
  CHECK(config.HangWatchdogPath.empty());
  CHECK(config.HangPhaseEventsPath.empty());
  CHECK(config.HangDumpPath.empty());
  CHECK(config.ActiveFpsStartFrame == 0u);
  CHECK(config.ActiveFrameSpikeThresholdUs == 25000);
  CHECK(!config.ActiveFrameSpikeTrace);
  CHECK(config.FrameHeartbeatInterval == 0);
  CHECK(config.GameplayHeartbeatInterval == 0);
  CHECK(config.FrameHeartbeatPath.empty());
  CHECK(config.InputRecordPath.empty());
  CHECK(config.InputRecordStartFrame == 0u);
  CHECK(config.InputRecordEndFrame == 0u);
  CHECK(config.InputRecordInstance == -1);
  CHECK(!config.ScreenHashEnabled);
  CHECK(config.HashLogPath.empty());
  CHECK(config.ScreenshotDir.empty());
  CHECK(config.ScreenshotInterval == 0);
  CHECK(config.ScreenshotStartFrame == 0u);
  CHECK(config.ScreenshotEndFrame == 0u);
  CHECK(!config.ScreenshotRegisterTrace);
  CHECK(config.RamDumpDir.empty());
  CHECK(config.RamDumpInterval == 0);
  CHECK(config.RamDumpFrames.empty());
  CHECK(config.GameStateTracePath.empty());
  CHECK(config.DiagnosticsPath.empty());
  CHECK(config.DiagnosticEventsPath.empty());
  CHECK(!config.DiagnosticEventsEnabled);
  CHECK(config.DiagnosticRingFrames == 360);
  CHECK(config.GameStateTraceInterval == 60);
  CHECK(config.GameStateTraceStartFrame == 0u);
  CHECK(config.GameStateTraceEndFrame == 0u);
  CHECK(!config.GameStateTraceExtended);
  CHECK(config.AIPlayLogPath.empty());
  CHECK(config.AIObservationV2Path.empty());
  CHECK(config.AIObservationV3Path.empty());
  CHECK(config.AIPlayLogInterval == 1);
  CHECK(config.AIPlayLogFlushInterval == 60);
  CHECK(config.AIPlayLogStartFrame == 0u);
  CHECK(config.AIPlayLogEndFrame == 0u);
  CHECK(config.AIPlayLogMaxObjects == 32);
  CHECK(config.AIObservationV2StageFilter == -1);
  CHECK(config.AIObservationV3StageFilter == -1);
  CHECK(config.AIPlayLogGameplayOnly);
}

void TestDiagnosticsConfigReadsClampsAndPreservesPriority() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_HANG_DIAGNOSTICS", "1"},
      {"MELONDS_NSML_WATCHDOG_INTERVAL_MS", "1"},
      {"MELONDS_NSML_HANG_THRESHOLD_MS", "999999"},
      {"MELONDS_NSML_WATCHDOG_FILE", "watchdog.txt"},
      {"MELONDS_NSML_PHASE_EVENTS_FILE", "phases.txt"},
      {"MELONDS_NSML_HANG_DUMP_FILE", "hang.dmp"},
      {"MELONDS_NSML_ACTIVE_FPS_START_FRAME", "-1"},
      {"MELONDS_NSML_FPS_SPIKE_THRESHOLD_MS", "1001"},
      {"MELONDS_NSML_FPS_SPIKE_TRACE", "1"},
      {"MELONDS_NSML_FRAME_HEARTBEAT_INTERVAL", "-1"},
      {"MELONDS_NSML_GAMEPLAY_HEARTBEAT_INTERVAL", "9999"},
      {"MELONDS_NSML_FRAME_HEARTBEAT_FILE", "heartbeat.txt"},
      {"MELONDS_NSML_INPUT_RECORD_FILE", "input.txt"},
      {"MELONDS_NSML_INPUT_RECORD_START_FRAME", "10"},
      {"MELONDS_NSML_INPUT_RECORD_END_FRAME", "5"},
      {"MELONDS_NSML_INPUT_RECORD_INSTANCE", "16"},
      {"MELONDS_NSML_SCREEN_HASH", "1"},
      {"MELONDS_NSML_HASH_LOG", "hash.log"},
      {"MELONDS_NSML_SCREENSHOT_DIR", "screens"},
      {"MELONDS_NSML_SCREENSHOT_INTERVAL", "-1"},
      {"MELONDS_NSML_SCREENSHOT_START_FRAME", "10"},
      {"MELONDS_NSML_SCREENSHOT_END_FRAME", "5"},
      {"MELONDS_NSML_SCREENSHOT_REG_TRACE", "1"},
      {"MELONDS_NSML_RAM_DUMP_DIR", "ram"},
      {"MELONDS_NSML_RAM_DUMP_INTERVAL", "-2"},
      {"MELONDS_NSML_RAM_DUMP_FRAMES", "10,20-30"},
      {"MELONDS_NSML_GAME_STATE_TRACE", "state.csv"},
      {"MELONDS_NSML_DIAGNOSTICS_FILE", "melonds-diagnostics.json"},
      {"MELONDS_NSML_DIAGNOSTIC_EVENTS", "1"},
      {"MELONDS_NSML_DIAGNOSTIC_RING_FRAMES", "999"},
      {"MELONDS_NSML_GAME_STATE_TRACE_INTERVAL", "0"},
      {"MELONDS_NSML_GAME_STATE_TRACE_START_FRAME", "-3"},
      {"MELONDS_NSML_GAME_STATE_TRACE_END_FRAME", "4"},
      {"MELONDS_NSML_GAME_STATE_TRACE_EXTENDED", "1"},
      {"MELONDS_NSML_AI_PLAY_LOG", "ai.jsonl"},
      {"MELONDS_NSML_AI_OBSERVATION_V2_LOG", "v2.jsonl"},
      {"MELONDS_NSML_AI_OBSERVATION_V3_LOG", "v3.jsonl"},
      {"MELONDS_NSML_AI_PLAY_LOG_INTERVAL", "0"},
      {"MELONDS_NSML_AI_PLAY_LOG_FLUSH_INTERVAL", "-5"},
      {"MELONDS_NSML_AI_PLAY_LOG_START_FRAME", "-6"},
      {"MELONDS_NSML_AI_PLAY_LOG_END_FRAME", "7"},
      {"MELONDS_NSML_AI_PLAY_LOG_MAX_OBJECTS", "999"},
      {"MELONDS_NSML_AI_OBSERVATION_V2_STAGE_FILTER", "99"},
      {"MELONDS_NSML_AI_OBSERVATION_V3_STAGE_FILTER", "-99"},
      {"MELONDS_NSML_AI_PLAY_LOG_INCLUDE_NON_GAMEPLAY", "1"},
  };

  auto config = NsmbMvlNetplay::Config::LoadDiagnosticsConfig(environment, 720);
  CHECK(config.HangDiagnosticsEnabled);
  CHECK(config.HangWatchdogIntervalMs == 100);
  CHECK(config.HangThresholdMs == 300000);
  CHECK(config.HangWatchdogPath == "watchdog.txt");
  CHECK(config.HangPhaseEventsPath == "phases.txt");
  CHECK(config.HangDumpPath == "hang.dmp");
  CHECK(config.ActiveFpsStartFrame == 0u);
  CHECK(config.ActiveFrameSpikeThresholdUs == 1000000);
  CHECK(config.ActiveFrameSpikeTrace);
  CHECK(config.FrameHeartbeatInterval == 0);
  CHECK(config.GameplayHeartbeatInterval == 3600);
  CHECK(config.FrameHeartbeatPath == "heartbeat.txt");
  CHECK(config.InputRecordPath == "input.txt");
  CHECK(config.InputRecordStartFrame == 10u);
  CHECK(config.InputRecordEndFrame == 10u);
  CHECK(config.InputRecordInstance == -1);
  CHECK(config.ScreenHashEnabled);
  CHECK(config.HashLogPath == "hash.log");
  CHECK(config.ScreenshotDir == "screens");
  CHECK(config.ScreenshotInterval == 0);
  CHECK(config.ScreenshotStartFrame == 10u);
  CHECK(config.ScreenshotEndFrame == 10u);
  CHECK(config.ScreenshotRegisterTrace);
  CHECK(config.RamDumpDir == "ram");
  CHECK(config.RamDumpInterval == 0);
  CHECK(config.RamDumpFrames == "10,20-30");
  CHECK(config.GameStateTracePath == "state.csv");
  CHECK(config.DiagnosticsPath == "melonds-diagnostics.json");
  CHECK(config.DiagnosticEventsPath == "melonds-events.jsonl");
  CHECK(config.DiagnosticEventsEnabled);
  CHECK(config.DiagnosticRingFrames == 720);
  CHECK(config.GameStateTraceInterval == 1);
  CHECK(config.GameStateTraceStartFrame == 0u);
  CHECK(config.GameStateTraceEndFrame == 4u);
  CHECK(config.GameStateTraceExtended);
  CHECK(config.AIPlayLogPath == "ai.jsonl");
  CHECK(config.AIObservationV2Path == "v2.jsonl");
  CHECK(config.AIObservationV3Path == "v3.jsonl");
  CHECK(config.AIPlayLogInterval == 1);
  CHECK(config.AIPlayLogFlushInterval == 0);
  CHECK(config.AIPlayLogStartFrame == 0u);
  CHECK(config.AIPlayLogEndFrame == 7u);
  CHECK(config.AIPlayLogMaxObjects == 256);
  CHECK(config.AIObservationV2StageFilter == 4);
  CHECK(config.AIObservationV3StageFilter == -1);
  CHECK(!config.AIPlayLogGameplayOnly);

  environment.Values["MELONDS_NSML_DIAGNOSTIC_EVENTS_FILE"] = "custom.jsonl";
  environment.Values["MELONDS_NSML_DIAGNOSTIC_EVENTS_DISABLE"] = "1";
  environment.Values["MELONDS_NSML_INPUT_RECORD_INSTANCE"] = "15";
  config = NsmbMvlNetplay::Config::LoadDiagnosticsConfig(environment, 720);
  CHECK(config.DiagnosticEventsPath == "custom.jsonl");
  CHECK(!config.DiagnosticEventsEnabled);
  CHECK(config.InputRecordInstance == 15);
}

void TestAIConfigDefaults() {
  const MapEnvironment environment;
  const auto config = NsmbMvlNetplay::Config::LoadAIConfig(environment);
  CHECK(!config.Rule.Enabled);
  CHECK(!config.Rule.HostOnly);
  CHECK(!config.Rule.ClientOnly);
  CHECK(config.Rule.PlayerSpec == "remote");
  CHECK(config.Rule.StartFrame == 0u);
  CHECK(config.Rule.HorizontalDeadzone == 0x4000);
  CHECK(config.Rule.HorizontalWrapWidth == 0x400000);
  CHECK(config.Rule.CloseRange == 0x22000);
  CHECK(config.Rule.HazardHorizontalRange == 0x40000);
  CHECK(config.Rule.HazardVerticalRange == 0x50000);
  CHECK(config.Rule.JumpInterval == 42);
  CHECK(config.Rule.JumpFrames == 9);
  CHECK(!config.Rule.TraceEnabled);
  CHECK(config.Rule.TraceInterval == 60);

  CHECK(!config.Imitation.Enabled);
  CHECK(!config.Imitation.HostOnly);
  CHECK(!config.Imitation.ClientOnly);
  CHECK(config.Imitation.PlayerSpec == "remote");
  CHECK(config.Imitation.StartFrame == 0u);
  CHECK(config.Imitation.Threshold == 0.5);
  CHECK(config.Imitation.AllowedHeldMask == 0x8F3u);
  CHECK(config.Imitation.HazardGuardEnabled);
  CHECK(config.Imitation.HazardGuardHorizontalRange == 0x40000);
  CHECK(config.Imitation.HazardGuardVerticalRange == 0x50000);
  CHECK(config.Imitation.HazardGuardCloseRange == 0x10000);
  CHECK(!config.Imitation.TraceEnabled);
  CHECK(config.Imitation.TraceInterval == 60);
  CHECK(config.Imitation.InferInterval == 16);
  CHECK(config.Imitation.NeutralHoldFrames == 8);
  CHECK(config.Imitation.WarnMissingFeatures);
  CHECK(config.Imitation.ModelPath.empty());
}

void TestAIConfigReadsClampsAndPreservesDependencies() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_RULE_AI", "1"},
      {"MELONDS_NSML_RULE_AI_HOST_ONLY", "1"},
      {"MELONDS_NSML_RULE_AI_CLIENT_ONLY", "1"},
      {"MELONDS_NSML_RULE_AI_PLAYER", "self"},
      {"MELONDS_NSML_RULE_AI_START_FRAME", "-1"},
      {"MELONDS_NSML_RULE_AI_HORIZONTAL_DEADZONE", "-1"},
      {"MELONDS_NSML_RULE_AI_WRAP_WIDTH", "99999999"},
      {"MELONDS_NSML_RULE_AI_CLOSE_RANGE", "0"},
      {"MELONDS_NSML_RULE_AI_HAZARD_HORIZONTAL_RANGE", "99999999"},
      {"MELONDS_NSML_RULE_AI_HAZARD_VERTICAL_RANGE", "-1"},
      {"MELONDS_NSML_RULE_AI_JUMP_INTERVAL", "3"},
      {"MELONDS_NSML_RULE_AI_JUMP_FRAMES", "9"},
      {"MELONDS_NSML_RULE_AI_TRACE", "1"},
      {"MELONDS_NSML_RULE_AI_TRACE_INTERVAL", "0"},
      {"MELONDS_NSML_IMITATION_AI", "1"},
      {"MELONDS_NSML_IMITATION_AI_HOST_ONLY", "1"},
      {"MELONDS_NSML_IMITATION_AI_CLIENT_ONLY", "1"},
      {"MELONDS_NSML_IMITATION_AI_PLAYER", "local"},
      {"MELONDS_NSML_IMITATION_AI_START_FRAME", "-1"},
      {"MELONDS_NSML_IMITATION_AI_THRESHOLD", "2"},
      {"MELONDS_NSML_IMITATION_AI_ALLOWED_HELD_MASK", "0xFFFF"},
      {"MELONDS_NSML_IMITATION_AI_HAZARD_GUARD", "1"},
      {"MELONDS_NSML_IMITATION_AI_DISABLE_HAZARD_GUARD", "1"},
      {"MELONDS_NSML_IMITATION_AI_HAZARD_GUARD_HORIZONTAL_RANGE", "100"},
      {"MELONDS_NSML_IMITATION_AI_HAZARD_GUARD_VERTICAL_RANGE", "99999999"},
      {"MELONDS_NSML_IMITATION_AI_HAZARD_GUARD_CLOSE_RANGE", "200"},
      {"MELONDS_NSML_IMITATION_AI_TRACE", "1"},
      {"MELONDS_NSML_IMITATION_AI_TRACE_INTERVAL", "0"},
      {"MELONDS_NSML_IMITATION_AI_INFER_INTERVAL", "0"},
      {"MELONDS_NSML_IMITATION_AI_NEUTRAL_HOLD_FRAMES", "999"},
      {"MELONDS_NSML_IMITATION_AI_DISABLE_FEATURE_WARNING", "1"},
      {"MELONDS_NSML_IMITATION_AI_MODEL", "model.json"},
  };
  const auto config = NsmbMvlNetplay::Config::LoadAIConfig(environment);
  CHECK(config.Rule.Enabled);
  CHECK(config.Rule.HostOnly);
  CHECK(config.Rule.ClientOnly);
  CHECK(config.Rule.PlayerSpec == "self");
  CHECK(config.Rule.StartFrame == 0u);
  CHECK(config.Rule.HorizontalDeadzone == 0);
  CHECK(config.Rule.HorizontalWrapWidth == 0x800000);
  CHECK(config.Rule.CloseRange == 0x1000);
  CHECK(config.Rule.HazardHorizontalRange == 0x200000);
  CHECK(config.Rule.HazardVerticalRange == 0);
  CHECK(config.Rule.JumpInterval == 3);
  CHECK(config.Rule.JumpFrames == 3);
  CHECK(config.Rule.TraceEnabled);
  CHECK(config.Rule.TraceInterval == 1);

  CHECK(config.Imitation.Enabled);
  CHECK(config.Imitation.HostOnly);
  CHECK(config.Imitation.ClientOnly);
  CHECK(config.Imitation.PlayerSpec == "local");
  CHECK(config.Imitation.StartFrame == 0u);
  CHECK(config.Imitation.Threshold == 1.0);
  CHECK(config.Imitation.AllowedHeldMask == 0x0FFFu);
  CHECK(!config.Imitation.HazardGuardEnabled);
  CHECK(config.Imitation.HazardGuardHorizontalRange == 100);
  CHECK(config.Imitation.HazardGuardVerticalRange == 0x200000);
  CHECK(config.Imitation.HazardGuardCloseRange == 100);
  CHECK(config.Imitation.TraceEnabled);
  CHECK(config.Imitation.TraceInterval == 1);
  CHECK(config.Imitation.InferInterval == 1);
  CHECK(config.Imitation.NeutralHoldFrames == 120);
  CHECK(!config.Imitation.WarnMissingFeatures);
  CHECK(config.Imitation.ModelPath == "model.json");
}

void TestStateSyncConfigDefaultsAndApplyModes() {
  MapEnvironment environment;
  auto config = NsmbMvlNetplay::Config::LoadStateSyncConfig(environment);
  CHECK(!config.GameEnabled);
  CHECK(!config.GameExtended);
  CHECK(!config.GameApplyEnabled);
  CHECK(config.GameApplyCriticalGlobals);
  CHECK(config.GameApplyStarObjects);
  CHECK(config.GameApplyStageObjects);
  CHECK(config.GameApplyPlayerActors);
  CHECK(!config.GameApplyRemotePlayerOnly);
  CHECK(config.GameInterval == 60);
  CHECK(config.WorldTraceObjectLifecyclesInterval == 60);

  environment.Values["MELONDS_NSML_STATE_APPLY_MODE"] = "critical";
  config = NsmbMvlNetplay::Config::LoadStateSyncConfig(environment);
  CHECK(config.GameApplyCriticalGlobals);
  CHECK(config.GameApplyStarObjects);
  CHECK(!config.GameApplyStageObjects);
  CHECK(!config.GameApplyPlayerActors);

  environment.Values["MELONDS_NSML_STATE_APPLY_MODE"] = "globals";
  config = NsmbMvlNetplay::Config::LoadStateSyncConfig(environment);
  CHECK(config.GameApplyCriticalGlobals);
  CHECK(!config.GameApplyStarObjects);
  CHECK(!config.GameApplyStageObjects);
  CHECK(!config.GameApplyPlayerActors);

  environment.Values["MELONDS_NSML_STATE_APPLY_MODE"] = "objects";
  config = NsmbMvlNetplay::Config::LoadStateSyncConfig(environment);
  CHECK(!config.GameApplyCriticalGlobals);
  CHECK(config.GameApplyStarObjects);
  CHECK(config.GameApplyStageObjects);
  CHECK(config.GameApplyPlayerActors);

  environment.Values["MELONDS_NSML_STATE_APPLY_MODE"] = "remote-player";
  config = NsmbMvlNetplay::Config::LoadStateSyncConfig(environment);
  CHECK(!config.GameApplyCriticalGlobals);
  CHECK(!config.GameApplyStarObjects);
  CHECK(!config.GameApplyStageObjects);
  CHECK(config.GameApplyPlayerActors);
  CHECK(config.GameApplyRemotePlayerOnly);

  environment.Values["MELONDS_NSML_STATE_APPLY_MODE"] = "unknown";
  config = NsmbMvlNetplay::Config::LoadStateSyncConfig(environment);
  CHECK(config.GameApplyCriticalGlobals);
  CHECK(config.GameApplyStarObjects);
  CHECK(config.GameApplyStageObjects);
  CHECK(config.GameApplyPlayerActors);
  CHECK(!config.GameApplyRemotePlayerOnly);
}

void TestStateSyncConfigReadsClampsAndSkipPriorities() {
  MapEnvironment environment;
  environment.Values = {
      {"MELONDS_NSML_STATE_SYNC", "1"},
      {"MELONDS_NSML_STATE_SYNC_EXTENDED", "1"},
      {"MELONDS_NSML_STATE_APPLY", "1"},
      {"MELONDS_NSML_STATE_SYNC_INTERVAL", "0"},
      {"MELONDS_NSML_WORLD_STATE_TRACE_MOVING_HAZARDS", "1"},
      {"MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES", "1"},
      {"MELONDS_NSML_WORLD_STATE_TRACE_ACTOR_INTERNALS", "1"},
      {"MELONDS_NSML_WORLD_STATE_TRACE_EFFECTS", "1"},
      {"MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES_INTERVAL", "0"},
      {"MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES_START_FRAME", "-1"},
      {"MELONDS_NSML_WORLD_STATE_TRACE_OBJECT_LIFECYCLES_END_FRAME", "-2"},
  };

  const auto config = NsmbMvlNetplay::Config::LoadStateSyncConfig(environment);
  CHECK(config.GameEnabled);
  CHECK(config.GameExtended);
  CHECK(config.GameApplyEnabled);
  CHECK(config.GameInterval == 1);
  CHECK(config.WorldTraceMovingHazards);
  CHECK(config.WorldTraceObjectLifecycles);
  CHECK(config.WorldTraceActorInternals);
  CHECK(config.WorldTraceEffects);
  CHECK(config.WorldTraceObjectLifecyclesInterval == 1);
  CHECK(config.WorldTraceObjectLifecyclesStartFrame == 0u);
  CHECK(config.WorldTraceObjectLifecyclesEndFrame == 0u);
}

} // namespace

int main() {
  TestFlagsPreserveExistingSemantics();
  TestStringsPreserveFallbackSemantics();
  TestIntegerParsingPreservesBaseAndFallback();
  TestDoubleParsingPreservesFallback();
  TestUnsignedParsingPreservesExistingInvalidValueBehavior();
  TestUnsignedListParsingPreservesLegacySemantics();
  TestBootstrapConfigDefaults();
  TestBootstrapConfigReadsAndClampsEnvironment();
  TestBootstrapConfigSupportsLegacyEnableAlias();
  TestConnectionConfigDefaultsAndRoleFallback();
  TestConnectionConfigReadsExistingValuesAndClamps();
  TestInputConfigDefaultsPreserveLegacyInitializationOrder();
  TestInputConfigReadsClampsAndNormalizesRanges();
  TestRuntimePatchConfigDefaults();
  TestRuntimePatchConfigReadsAndClampsEnvironment();
  TestHarnessConfigDefaults();
  TestHarnessConfigReadsClampsAndPreservesPresence();
  TestPacketBridgeConfigDefaults();
  TestPacketBridgeConfigReadsAndClampsEnvironment();
  TestRollbackConfigDefaultsAndBackendAliases();
  TestRollbackConfigReadsClampsAndDependencies();
  TestMvlConfigDefaults();
  TestMvlConfigReadsClampsAndPreservesPriority();
  TestDiagnosticsConfigDefaults();
  TestDiagnosticsConfigReadsClampsAndPreservesPriority();
  TestAIConfigDefaults();
  TestAIConfigReadsClampsAndPreservesDependencies();
  TestStateSyncConfigDefaultsAndApplyModes();
  TestStateSyncConfigReadsClampsAndSkipPriorities();

  if (Failures != 0) {
    std::fprintf(stderr, "nsmb netplay config tests failed: %d\n", Failures);
    return 1;
  }

  std::printf("nsmb netplay config tests passed\n");
  return 0;
}
