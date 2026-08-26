param(
    [int]$Frames = 2600,
    [int]$WaitTimeoutMs = 300000,
    [int]$InternalWaitTimeoutMs = 5000,
    [string]$Exe = "build\release-windows-x86_64\melonDS.exe",
    [string]$HostRom = "roms\nsmb-us-direct-mvl-entry-stable-host-true-local0-wificount2-vslockskip-netaid.tmp.nds",
    [string]$ClientRom = "roms\nsmb-us-direct-mvl-entry-stable-client-true-local1-wificount2-vslockskip-netaid.tmp.nds",
    [string]$GenerateMvlSourceRom = "roms\nsmb-us.nds",
    [string]$HostInputScript = "tests\nsmb_us_direct_mvl_manual_host_mario_move.inputs",
    [string]$ClientInputScript = "tests\nsmb_us_direct_mvl_manual_client_luigi_move.inputs",
    [string]$MvlMatchSeed = "",
    [int]$MvlStage = -1,
    [string]$MvlSceneSettings = "",
    [ValidateSet(1, 2, 3)] [int]$MvlWins = 2,
    [ValidateSet(3, 5, 10)] [int]$MvlBigStars = 5,
    [ValidateSet("3", "5", "endless", "Endless")] [string]$MvlLives = "endless",
    [ValidateSet("fixed", "random", "select")] [string]$MvlCourseMode = "fixed",
    [switch]$GenerateMvlConfiguredRoms,
    [switch]$SkipRomEnsure,
    [int]$InputDelayFrames = 16,
    [string]$MvlStageSequence = "",
    [string]$MvlMatchSeedSequence = "",
    [int]$InputMaxFrameLead = 2,
    [switch]$InputNetplayTrace,
    [int]$InputSendDelayFrames = 0,
    [int]$InputSendJitterFrames = 0,
    [switch]$InputUnreliable,
    [int]$InputBundleHistory = 0,
    [switch]$AllowRemoteInputTimeoutFallback,
    [switch]$NetworkPumpThread,
    [int]$NetworkPumpSleepUs = 250,
    [switch]$LowDelayWan,
    [int]$InputDropModulo = 0,
    [int]$InputDropOffset = 0,
    [switch]$Rollback,
    [string]$RollbackBackend = "",
    [string]$RollbackTinyCoreFlags = "",
    [int]$RollbackWindow = 20,
    [int]$RollbackCheckpointInterval = 1,
    [int]$RollbackResimulateDelayFrames = 0,
    [switch]$RollbackResimulate,
    [switch]$RollbackSkipIntermediateResimCheckpoints,
    [switch]$RollbackRestoreProbe,
    [int]$RollbackPredictionProbeModulo = 0,
    [int]$RollbackPredictionProbeOffset = 0,
    [int]$RollbackPredictionProbeLimit = -1,
    [int]$RollbackPredictionProbeStartFrame = 0,
    [int]$RollbackPredictionProbeEndFrame = 0,
    [string]$RollbackPredictionProbeKeyMask = "",
    [int]$RollbackPredictionProbeConfirmDelayFrames = 0,
    [switch]$RollbackPredictionProbeConfirmAfterOneFrame,
    [ValidateSet("both", "host", "client")]
    [string]$RollbackPredictionProbeRole = "both",
    [int]$RollbackInputWaitUs = 0,
    [int]$RollbackSettleFrames = 0,
    [switch]$IgnoreSpeculativeInputFields,
    [int]$GameStateTraceInterval = 30,
    [int]$GameStateTraceStartFrame = 0,
    [int]$GameStateTraceEndFrame = 0,
    [switch]$NoGameStateTrace,
    [switch]$StateSync,
    [switch]$StateApply,
    [int]$StateSyncInterval = 60,
    [switch]$StateSyncExtended,
    [string]$StateApplyMode = "",
    [switch]$WorldStateTraceMovingHazards,
    [switch]$WorldStateTraceObjectLifecycles,
    [switch]$WorldStateTraceActorInternals,
    [switch]$WorldStateTraceEffects,
    [int]$WorldStateTraceObjectLifecyclesInterval = 60,
    [int]$WorldStateTraceObjectLifecyclesStartFrame = 0,
    [int]$WorldStateTraceObjectLifecyclesEndFrame = 0,
    [switch]$RequireNoUnexpectedWorldLifecycleDiff,
    [switch]$SkipGameStateComparison,
    [switch]$SkipMovementProbe,
    [switch]$RequireActorSnapshotMovement,
    [int]$ActorSnapshotStartFrame = 990,
    [int]$ActorSnapshotMinMovedRows = 1,
    [int]$ActorSnapshotMaxDriftX = -1,
    [int]$ActorSnapshotMaxDriftY = -1,
    [int]$ActorSnapshotMaxConsecutiveDriftRows = 0,
    [switch]$RequireWorldSnapshotSync,
    [int]$WorldSnapshotStartFrame = 900,
    [int]$WorldSnapshotMaxStarDriftX = 0,
    [int]$WorldSnapshotMaxStarDriftY = 0,
    [int]$WorldSnapshotMaxHazardDriftX = -1,
    [int]$WorldSnapshotMaxHazardDriftY = -1,
    [int]$WorldSnapshotMaxHazardConsecutiveDriftRows = 0,
    [switch]$RequireMvlManagerGlobalSync,
    [int]$MvlManagerGlobalSnapshotStartFrame = 900,
    [switch]$TracePlayerLifeChanges,
    [switch]$TracePlayerDefeated,
    [switch]$RequireStarPickup,
    [int]$RequireStarPickupPlayer = -1,
    [switch]$RequirePlayerDeath,
    [int]$RequirePlayerDeathPlayer = -1,
    [int]$RequirePlayerDeathStartFrame = 0,
    [int]$RequirePlayerDeathEndFrame = 0,
    [switch]$RequireResultScene,
    [switch]$RequireNoResultScene,
    [switch]$RequireSecondMvlGame,
    [int]$RequireMvlGameCount = 0,
    [string]$RequireMvlGameStages = "",
    [switch]$CheckMovingHazardProgressDuringDeath,
    [int]$CheckMovingHazardProgressStartFrame = 0,
    [int]$CheckMovingHazardProgressEndFrame = 0,
    [int]$CheckMovingHazardProgressMinUniqueX = 3,
    [switch]$CheckVsPipeRespawnVisibility,
    [int]$CheckVsPipeRespawnVisibilityStartFrame = 0,
    [int]$CheckVsPipeRespawnVisibilityEndFrame = 0,
    [switch]$NoFrameLimit,
    [switch]$FixedFrameTime,
    [double]$TargetFps = 0.0,
    [switch]$NoDrawScreen,
    [int]$ScreenshotInterval = 0,
    [switch]$NoAudioSync,
    [double]$MaxActiveFrameMs = 0.0,
    [int]$MaxActiveFrameOver25ms = -1,
    [int]$MaxActiveFrameOver33ms = -1,
    [double]$MaxRollbackFrameMs = 0.0,
    [int]$MinRollbackResims = -1,
    [double]$SlowFrameThresholdMs = 33.0,
    [int]$MaxConsecutiveSlowFrames = -1,
    [int]$StallTimeoutMs = 0,
    [int]$FrameHeartbeatInterval = 120,
    [int]$GameplayHeartbeatInterval = 0,
    [int]$StallStartFrame = 900,
    [switch]$UseLanMP,
    [switch]$PacketBridgePreserveLocalTouch,
    [switch]$ForcePlayerPowerups,
    [int]$ForcePlayerPowerupsStartFrame = 0,
    [int]$ForcePlayerPowerupsEndFrame = 0,
    [int]$ForcePlayerPowerup0 = 0,
    [int]$ForcePlayerPowerup1 = 0,
    [switch]$ForcePlayerInventoryPowerups,
    [int]$ForcePlayerInventoryPowerupsStartFrame = 0,
    [int]$ForcePlayerInventoryPowerupsEndFrame = 0,
    [int]$ForcePlayerInventoryPowerup0 = 0,
    [int]$ForcePlayerInventoryPowerup1 = 0,
    [int]$HostStartupDelayMs = 1200,
    [UInt64]$HostProcessAffinityMask = 0,
    [UInt64]$ClientProcessAffinityMask = 0,
    [string]$LogDir = "logs\nsmb-mvl-split-local-input-smoke",
    [string]$HostPacketReplayFile = "",
    [string]$ClientPacketReplayFile = "",
    [switch]$PacketCapture,
    [switch]$PacketCaptureAllowPreGame,
    [string]$HostAIPlayLog = "",
    [string]$ClientAIPlayLog = "",
    [int]$AIPlayLogInterval = 1,
    [int]$AIPlayLogStartFrame = 0,
    [int]$AIPlayLogEndFrame = 0,
    [int]$AIPlayLogMaxObjects = 128,
    [switch]$AIPlayLogIncludeNonGameplay,
    [switch]$FpsSpikeTrace,
    [switch]$SoftwareRenderer = $true,
    [switch]$AllowJit
)

$ErrorActionPreference = "Stop"

function Set-MelonTomlValue {
    param(
        [string]$Text,
        [string]$KeyPath,
        [string]$Value
    )

    $idx = $KeyPath.LastIndexOf('.')
    if ($idx -lt 0) {
        if ($Text -match "(?m)^$([regex]::Escape($KeyPath))\s*=") {
            return ($Text -replace "(?m)^$([regex]::Escape($KeyPath))\s*=.*$", "$KeyPath = $Value")
        }
        return "$Text`n$KeyPath = $Value"
    }

    $section = $KeyPath.Substring(0, $idx)
    $key = $KeyPath.Substring($idx + 1)
    $sectionPattern = "(?ms)^\[$([regex]::Escape($section))\]\r?\n.*?(?=^\[|\z)"
    $sectionMatch = [regex]::Match($Text, $sectionPattern)
    if (-not $sectionMatch.Success) {
        return "$Text`n[$section]`n$key = $Value`n"
    }

    $sectionText = $sectionMatch.Value
    if ($sectionText -match "(?m)^$([regex]::Escape($key))\s*=") {
        $newSection = $sectionText -replace "(?m)^$([regex]::Escape($key))\s*=.*$", "$key = $Value"
    } else {
        $newSection = $sectionText.TrimEnd() + "`n$key = $Value`n"
    }
    return $Text.Substring(0, $sectionMatch.Index) + $newSection + $Text.Substring($sectionMatch.Index + $sectionMatch.Length)
}

if ($FpsSpikeTrace -and ($MaxConsecutiveSlowFrames -ge 0 -or $MaxRollbackFrameMs -gt 0.0)) {
    $env:MELONDS_NSML_FPS_SPIKE_TRACE = "1"
    $env:MELONDS_NSML_PERF_SPIKE_PHASE_TRACE = "1"
    $currentSpikeThreshold = 0.0
    $targetSpikeThreshold = if ($MaxRollbackFrameMs -gt 0.0) {
        [Math]::Min($SlowFrameThresholdMs, $MaxRollbackFrameMs)
    } else {
        $SlowFrameThresholdMs
    }
    $hasSpikeThreshold = [double]::TryParse(
        $env:MELONDS_NSML_FPS_SPIKE_THRESHOLD_MS,
        [System.Globalization.NumberStyles]::Float,
        [System.Globalization.CultureInfo]::InvariantCulture,
        [ref]$currentSpikeThreshold)
    if (-not $hasSpikeThreshold -or $currentSpikeThreshold -le 0.0 -or $currentSpikeThreshold -gt $targetSpikeThreshold) {
        $env:MELONDS_NSML_FPS_SPIKE_THRESHOLD_MS = $targetSpikeThreshold.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    }
} elseif (-not $FpsSpikeTrace) {
    Remove-Item Env:\MELONDS_NSML_FPS_SPIKE_TRACE -ErrorAction SilentlyContinue
    Remove-Item Env:\MELONDS_NSML_PERF_SPIKE_PHASE_TRACE -ErrorAction SilentlyContinue
    Remove-Item Env:\MELONDS_NSML_FPS_SPIKE_THRESHOLD_MS -ErrorAction SilentlyContinue
}

function Set-RollbackPredictionProbeEnvForChild {
    param([string]$Role)

    $roleEnabled = $RollbackPredictionProbeRole -eq "both" -or $RollbackPredictionProbeRole -eq $Role
    if ($RollbackPredictionProbeModulo -gt 0 -and $roleEnabled) {
        $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_MODULO = "$RollbackPredictionProbeModulo"
        $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_OFFSET = "$RollbackPredictionProbeOffset"
        $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_LIMIT = "$RollbackPredictionProbeLimit"
        $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_START_FRAME = "$RollbackPredictionProbeStartFrame"
        $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_END_FRAME = "$RollbackPredictionProbeEndFrame"
        if ($RollbackPredictionProbeKeyMask -ne "") {
            $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_KEY_MASK = "$RollbackPredictionProbeKeyMask"
        } else {
            Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_KEY_MASK -ErrorAction SilentlyContinue
        }
        if ($RollbackPredictionProbeConfirmDelayFrames -gt 0) {
            $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_DELAY_FRAMES = "$RollbackPredictionProbeConfirmDelayFrames"
        } else {
            Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_DELAY_FRAMES -ErrorAction SilentlyContinue
        }
        if ($RollbackPredictionProbeConfirmAfterOneFrame) {
            $env:MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_AFTER_ONE_FRAME = "1"
        } else {
            Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_AFTER_ONE_FRAME -ErrorAction SilentlyContinue
        }
    } else {
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_MODULO -ErrorAction SilentlyContinue
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_OFFSET -ErrorAction SilentlyContinue
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_LIMIT -ErrorAction SilentlyContinue
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_START_FRAME -ErrorAction SilentlyContinue
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_END_FRAME -ErrorAction SilentlyContinue
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_KEY_MASK -ErrorAction SilentlyContinue
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_DELAY_FRAMES -ErrorAction SilentlyContinue
        Remove-Item Env:\MELONDS_NSML_ROLLBACK_PREDICTION_PROBE_CONFIRM_AFTER_ONE_FRAME -ErrorAction SilentlyContinue
    }
}

Set-RollbackPredictionProbeEnvForChild -Role "none"

if ($RollbackInputWaitUs -gt 0) {
    $env:MELONDS_NSML_ROLLBACK_INPUT_WAIT_US = "$RollbackInputWaitUs"
} else {
    Remove-Item Env:\MELONDS_NSML_ROLLBACK_INPUT_WAIT_US -ErrorAction SilentlyContinue
}
$isTinyCorePreimageRollback = $RollbackBackend -eq "tinycorepreimage" -or $RollbackBackend -eq "tiny-core-preimage"
if ($Rollback -and -not $PSBoundParameters.ContainsKey('RollbackResimulate')) {
    $RollbackResimulate = $true
}
if ($RollbackSkipIntermediateResimCheckpoints) {
    $env:MELONDS_NSML_ROLLBACK_RESIM_SKIP_INTERMEDIATE_CHECKPOINTS = "1"
} else {
    Remove-Item Env:\MELONDS_NSML_ROLLBACK_RESIM_SKIP_INTERMEDIATE_CHECKPOINTS -ErrorAction SilentlyContinue
}
if ($Rollback -and $isTinyCorePreimageRollback) {
    $env:MELONDS_NSML_SUPPRESS_PU_DEBUG = "1"
    $env:MELONDS_NSML_ROLLBACK_SKIP_JIT_RESET = "1"
    $env:MELONDS_NSML_ROLLBACK_RESIM_SKIP_RENDER = "1"
    if ($RollbackTinyCoreFlags -eq "") { $RollbackTinyCoreFlags = "0x245" }
    $env:MELONDS_NSML_ROLLBACK_TINY_CORE_FLAGS = "$RollbackTinyCoreFlags"
} else {
    Remove-Item Env:\MELONDS_NSML_SUPPRESS_PU_DEBUG -ErrorAction SilentlyContinue
    Remove-Item Env:\MELONDS_NSML_ROLLBACK_SKIP_JIT_RESET -ErrorAction SilentlyContinue
    Remove-Item Env:\MELONDS_NSML_ROLLBACK_RESIM_SKIP_RENDER -ErrorAction SilentlyContinue
    Remove-Item Env:\MELONDS_NSML_ROLLBACK_TINY_CORE_FLAGS -ErrorAction SilentlyContinue
}
if ($NetworkPumpThread) {
    $env:MELONDS_NSML_NET_PUMP_THREAD = "1"
    $env:MELONDS_NSML_NET_PUMP_SLEEP_US = "$NetworkPumpSleepUs"
} else {
    Remove-Item Env:\MELONDS_NSML_NET_PUMP_THREAD -ErrorAction SilentlyContinue
    Remove-Item Env:\MELONDS_NSML_NET_PUMP_SLEEP_US -ErrorAction SilentlyContinue
}

if ($LowDelayWan) {
    $InputDelayFrames = 4
    $InputMaxFrameLead = 4
    $InputSendDelayFrames = 0
    $InputSendJitterFrames = 0
    $InputUnreliable = $true
    $InputBundleHistory = 8
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$smokeScript = Join-Path $PSScriptRoot "run-nsmb-mvl-lan-route-smoke.ps1"
$logRoot = if ([System.IO.Path]::IsPathRooted($LogDir)) {
    $LogDir
} else {
    Join-Path $repoRoot $LogDir
}

$cfgPath = Join-Path $repoRoot "build\release-windows-x86_64\melonDS.toml"
if (Test-Path $cfgPath) {
    $cfg = Get-Content $cfgPath -Raw
    $useGL = if ($SoftwareRenderer) { 'false' } else { 'true' }
    $renderer = if ($SoftwareRenderer) { '0' } else { '2' }
    $replacements = [ordered]@{
        'LimitFPS' = 'true'
        'AudioSync' = 'false'
        'Screen.UseGL' = $useGL
        'Screen.VSync' = 'false'
        'Screen.VSyncInterval' = '1'
        '3D.Renderer' = $renderer
        '3D.GL.ScaleFactor' = '1'
        '3D.GL.HiresCoordinates' = 'false'
        '3D.Soft.Threaded' = 'true'
        'Instance0.Window0.ScreenSizing' = '0'
        'Instance0.Window0.ShowOSD' = 'false'
    }
    foreach ($key in $replacements.Keys) {
        $cfg = Set-MelonTomlValue -Text $cfg -KeyPath $key -Value $replacements[$key]
    }
    Set-Content -Path $cfgPath -Value $cfg -Encoding UTF8
}
$hostLog = Join-Path $logRoot "host"
$clientLog = Join-Path $logRoot "client"
$wrapperLog = Join-Path $logRoot "wrapper"
New-Item -ItemType Directory -Force $wrapperLog | Out-Null
Remove-Item -Recurse -Force $hostLog, $clientLog -ErrorAction SilentlyContinue

$common = @(
    "-WaitTimeoutMs", "$WaitTimeoutMs",
    "-InternalWaitTimeoutMs", "$InternalWaitTimeoutMs",
    "-StallTimeoutMs", "$StallTimeoutMs",
    "-FrameHeartbeatInterval", "$FrameHeartbeatInterval",
    "-GameplayHeartbeatInterval", "$GameplayHeartbeatInterval",
    "-StallStartFrame", "$StallStartFrame",
    "-Frames", "$Frames",
    "-Exe", $Exe,
    "-MvlWins", "$MvlWins",
    "-MvlBigStars", "$MvlBigStars",
    "-MvlLives", "$MvlLives",
    "-MvlCourseMode", "$MvlCourseMode",
    "-ScreenshotInterval", "$ScreenshotInterval",
    "-NoHashLog",
    "-SkipDisconnectScreenshotCheck",
    "-SkipBlankScreenshotCheck",
    "-SkipMvlStateCheck",
    "-SkipGameplayActorCheck",
    "-InputNetplay",
    "-InputDelayFrames", "$InputDelayFrames",
    "-InputMaxFrameLead", "$InputMaxFrameLead",
    "-InputSendDelayFrames", "$InputSendDelayFrames",
    "-InputSendJitterFrames", "$InputSendJitterFrames",
    "-PacketBridgeJitHelperPatch",
    "-PacketBridgeJitHelperPatchFrame", "870",
    "-PacketBridgeStartFrame", "870",
    "-RequireNetLocalAidStartFrame", "870"
)
if ($MvlStageSequence -ne "") {
    $common += @("-MvlStageSequence", "$MvlStageSequence")
}
if ($MvlMatchSeedSequence -ne "") {
    $common += @("-MvlMatchSeedSequence", "$MvlMatchSeedSequence")
}
if (-not $UseLanMP) {
    $common += "-NoLanMP"
}
if ($PacketBridgePreserveLocalTouch) {
    $common += "-PacketBridgePreserveLocalTouch"
}
if ($PacketCapture) {
    $common += "-PacketCapture"
    if ($PacketCaptureAllowPreGame) {
        $common += "-PacketCaptureAllowPreGame"
    }
}
if ($ForcePlayerPowerups) {
    $common += @(
        "-ForcePlayerPowerups",
        "-ForcePlayerPowerupsStartFrame", "$ForcePlayerPowerupsStartFrame",
        "-ForcePlayerPowerupsEndFrame", "$ForcePlayerPowerupsEndFrame",
        "-ForcePlayerPowerup0", "$ForcePlayerPowerup0",
        "-ForcePlayerPowerup1", "$ForcePlayerPowerup1"
    )
}
if ($ForcePlayerInventoryPowerups) {
    $common += @(
        "-ForcePlayerInventoryPowerups",
        "-ForcePlayerInventoryPowerupsStartFrame", "$ForcePlayerInventoryPowerupsStartFrame",
        "-ForcePlayerInventoryPowerupsEndFrame", "$ForcePlayerInventoryPowerupsEndFrame",
        "-ForcePlayerInventoryPowerup0", "$ForcePlayerInventoryPowerup0",
        "-ForcePlayerInventoryPowerup1", "$ForcePlayerInventoryPowerup1"
    )
}
if (-not $NoGameStateTrace) {
    $common += @(
        "-GameStateTrace",
        "-GameStateTraceExtended",
        "-GameStateTraceInterval", "$GameStateTraceInterval",
        "-GameStateTraceStartFrame", "$GameStateTraceStartFrame",
        "-GameStateTraceEndFrame", "$GameStateTraceEndFrame"
    )
}
if ($StateSync) {
    $common += @(
        "-StateSync",
        "-StateSyncInterval", "$StateSyncInterval"
    )
    if ($StateApply) {
        $common += "-StateApply"
    }
    if ($StateSyncExtended) {
        $common += "-StateSyncExtended"
    }
    if ($StateApplyMode -ne "") {
        $common += @("-StateApplyMode", "$StateApplyMode")
    }
}
if ($WorldStateTraceMovingHazards) {
    $common += "-WorldStateTraceMovingHazards"
}
if ($WorldStateTraceObjectLifecycles) {
    $common += @(
        "-WorldStateTraceObjectLifecycles",
        "-WorldStateTraceObjectLifecyclesInterval", "$WorldStateTraceObjectLifecyclesInterval",
        "-WorldStateTraceObjectLifecyclesStartFrame", "$WorldStateTraceObjectLifecyclesStartFrame",
        "-WorldStateTraceObjectLifecyclesEndFrame", "$WorldStateTraceObjectLifecyclesEndFrame"
    )
}
if ($WorldStateTraceActorInternals) {
    $common += "-WorldStateTraceActorInternals"
}
if ($WorldStateTraceEffects) {
    $common += "-WorldStateTraceEffects"
}
if ($AllowJit) {
    $common += "-AllowJit"
}
if ($NoFrameLimit) {
    $common += "-NoFrameLimit"
}
if ($FixedFrameTime) {
    $common += "-FixedFrameTime"
}
if ($TargetFps -gt 0.0) {
    $common += @("-TargetFps", "$TargetFps")
}
if ($NoDrawScreen) {
    $common += "-NoDrawScreen"
}
if ($NoAudioSync) {
    $common += "-NoAudioSync"
}
if ($InputNetplayTrace) {
    $common += "-InputNetplayTrace"
}
if ($TracePlayerLifeChanges) {
    $common += "-TracePlayerLifeChanges"
}
if ($TracePlayerDefeated) {
    $common += "-TracePlayerDefeated"
}
if ($RequireStarPickup) {
    $common += @("-RequireStarPickup", "-RequireStarPickupPlayer", "$RequireStarPickupPlayer")
}
if ($RequirePlayerDeath) {
    $common += @(
        "-RequirePlayerDeath",
        "-RequirePlayerDeathPlayer", "$RequirePlayerDeathPlayer",
        "-RequirePlayerDeathStartFrame", "$RequirePlayerDeathStartFrame",
        "-RequirePlayerDeathEndFrame", "$RequirePlayerDeathEndFrame"
    )
}
if ($RequireResultScene) {
    $common += "-RequireResultScene"
}
if ($RequireNoResultScene) {
    $common += "-RequireNoResultScene"
}
if ($RequireSecondMvlGame) {
    $common += "-RequireSecondMvlGame"
}
if ($RequireMvlGameCount -gt 0) {
    $common += @("-RequireMvlGameCount", "$RequireMvlGameCount")
}
if ($RequireMvlGameStages -ne "") {
    $common += @("-RequireMvlGameStages", "$RequireMvlGameStages")
}
if ($CheckMovingHazardProgressDuringDeath) {
    $common += @(
        "-CheckMovingHazardProgressDuringDeath",
        "-CheckMovingHazardProgressStartFrame", "$CheckMovingHazardProgressStartFrame",
        "-CheckMovingHazardProgressEndFrame", "$CheckMovingHazardProgressEndFrame",
        "-CheckMovingHazardProgressMinUniqueX", "$CheckMovingHazardProgressMinUniqueX"
    )
}
if ($CheckVsPipeRespawnVisibility) {
    $common += @(
        "-CheckVsPipeRespawnVisibility",
        "-CheckVsPipeRespawnVisibilityStartFrame", "$CheckVsPipeRespawnVisibilityStartFrame",
        "-CheckVsPipeRespawnVisibilityEndFrame", "$CheckVsPipeRespawnVisibilityEndFrame"
    )
}
if ($MvlMatchSeed -ne "") {
    $common += @("-MvlMatchSeed", $MvlMatchSeed)
}
if ($MvlStage -ge 0) {
    $common += @("-MvlStage", "$MvlStage")
}
if ($MvlSceneSettings -ne "") {
    $common += @("-MvlSceneSettings", "$MvlSceneSettings")
}
if ($GenerateMvlConfiguredRoms) {
    $common += @("-GenerateMvlConfiguredRoms", "-Rom", "$GenerateMvlSourceRom")
}
if (!$SkipRomEnsure -and !$GenerateMvlConfiguredRoms) {
    $generatorCourseMode = if ($MvlCourseMode -eq "fixed") { "random" } else { $MvlCourseMode }
    $ensureParams = @{
        SourceRom = $GenerateMvlSourceRom
        HostRom = $HostRom
        ClientRom = $ClientRom
        MvlWins = $MvlWins
        MvlBigStars = $MvlBigStars
        MvlLives = $MvlLives
        MvlCourseMode = $generatorCourseMode
    }
    if ($MvlStage -ge 0) {
        $ensureParams.MvlStage = $MvlStage
    }
    if ($MvlSceneSettings -ne "") {
        $ensureParams.MvlSceneSettings = $MvlSceneSettings
    }
    & (Join-Path $PSScriptRoot "generate-nsmb-mvl-stable-roms.ps1") @ensureParams
}
if ($InputUnreliable) {
    $common += "-InputUnreliable"
}
if ($InputBundleHistory -gt 0) {
    $common += @("-InputBundleHistory", "$InputBundleHistory")
}
if ($AllowRemoteInputTimeoutFallback) {
    $common += "-AllowRemoteInputTimeoutFallback"
}
if ($InputDropModulo -gt 0) {
    $common += @("-InputDropModulo", "$InputDropModulo", "-InputDropOffset", "$InputDropOffset")
}
if ($Rollback) {
    $common += @(
        "-Rollback",
        "-RollbackWindow", "$RollbackWindow",
        "-RollbackCheckpointInterval", "$RollbackCheckpointInterval",
        "-RollbackResimulateDelayFrames", "$RollbackResimulateDelayFrames"
    )
    if ($RollbackBackend -ne "") {
        $common += @("-RollbackBackend", "$RollbackBackend")
    }
    if ($RollbackResimulate) {
        $common += "-RollbackResimulate"
    }
    if ($RollbackRestoreProbe) {
        $common += "-RollbackRestoreProbe"
    }
}

$hostArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $smokeScript
) + $common + @(
    "-RunRole", "host",
    "-HostRom", $HostRom,
    "-InputScript", $HostInputScript,
    "-LogDir", $hostLog
)
if ($HostProcessAffinityMask -ne 0) {
    $hostArgs += @("-ProcessAffinityMask", "$HostProcessAffinityMask")
}
if ($HostPacketReplayFile -ne "") {
    $hostArgs += @("-HostPacketReplayFile", $HostPacketReplayFile)
}
if (-not $NoGameStateTrace) {
    $hostArgs += @("-RequireHostLocalPlayerID", "0", "-RequireHostNetLocalAid", "0")
}

$clientArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $smokeScript
) + $common + @(
    "-RunRole", "client",
    "-Peer", "127.0.0.1",
    "-ClientRom", $ClientRom,
    "-InputScript", $ClientInputScript,
    "-LogDir", $clientLog
)
if ($ClientProcessAffinityMask -ne 0) {
    $clientArgs += @("-ProcessAffinityMask", "$ClientProcessAffinityMask")
}
if ($ClientPacketReplayFile -ne "") {
    $clientArgs += @("-ClientPacketReplayFile", $ClientPacketReplayFile)
}
if (-not $NoGameStateTrace) {
    $clientArgs += @("-RequireClientLocalPlayerID", "1", "-RequireClientNetLocalAid", "1")
}

$hostOut = Join-Path $wrapperLog "host-wrapper.out.txt"
$hostErr = Join-Path $wrapperLog "host-wrapper.err.txt"
$clientOut = Join-Path $wrapperLog "client-wrapper.out.txt"
$clientErr = Join-Path $wrapperLog "client-wrapper.err.txt"

$aiPlayLogEnvNames = @(
    "MELONDS_NSML_AI_PLAY_LOG",
    "MELONDS_NSML_AI_PLAY_LOG_INTERVAL",
    "MELONDS_NSML_AI_PLAY_LOG_START_FRAME",
    "MELONDS_NSML_AI_PLAY_LOG_END_FRAME",
    "MELONDS_NSML_AI_PLAY_LOG_MAX_OBJECTS",
    "MELONDS_NSML_AI_PLAY_LOG_INCLUDE_NON_GAMEPLAY"
)
$savedAIPlayLogEnv = @{}
foreach ($name in $aiPlayLogEnvNames) {
    $savedAIPlayLogEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

function Set-AIPlayLogEnvForChild {
    param([string]$Path)

    if ($Path -eq "") {
        Remove-Item Env:\MELONDS_NSML_AI_PLAY_LOG -ErrorAction SilentlyContinue
        return
    }
    $resolvedPath = $Path
    if (-not [System.IO.Path]::IsPathRooted($resolvedPath)) {
        $resolvedPath = Join-Path $repoRoot $resolvedPath
    }
    $parent = Split-Path -Parent $resolvedPath
    if ($parent -ne "") {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $env:MELONDS_NSML_AI_PLAY_LOG = $resolvedPath
    $env:MELONDS_NSML_AI_PLAY_LOG_INTERVAL = "$AIPlayLogInterval"
    $env:MELONDS_NSML_AI_PLAY_LOG_START_FRAME = "$AIPlayLogStartFrame"
    $env:MELONDS_NSML_AI_PLAY_LOG_END_FRAME = "$AIPlayLogEndFrame"
    $env:MELONDS_NSML_AI_PLAY_LOG_MAX_OBJECTS = "$AIPlayLogMaxObjects"
    if ($AIPlayLogIncludeNonGameplay) {
        $env:MELONDS_NSML_AI_PLAY_LOG_INCLUDE_NON_GAMEPLAY = "1"
    } else {
        Remove-Item Env:\MELONDS_NSML_AI_PLAY_LOG_INCLUDE_NON_GAMEPLAY -ErrorAction SilentlyContinue
    }
}

function Restore-AIPlayLogEnv {
    foreach ($name in $aiPlayLogEnvNames) {
        $value = $savedAIPlayLogEnv[$name]
        if ($null -eq $value) {
            [Environment]::SetEnvironmentVariable($name, $null, "Process")
        } else {
            [Environment]::SetEnvironmentVariable($name, $value, "Process")
        }
    }
}

$hostProc = $null
$clientProc = $null
try {
    Set-RollbackPredictionProbeEnvForChild -Role "host"
    Set-AIPlayLogEnvForChild -Path $HostAIPlayLog
    $hostProc = Start-Process -FilePath "powershell.exe" `
        -ArgumentList $hostArgs `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $hostOut `
        -RedirectStandardError $hostErr `
        -PassThru `
        -WindowStyle Hidden

    Start-Sleep -Milliseconds $HostStartupDelayMs

    Set-RollbackPredictionProbeEnvForChild -Role "client"
    Set-AIPlayLogEnvForChild -Path $ClientAIPlayLog
    $clientProc = Start-Process -FilePath "powershell.exe" `
        -ArgumentList $clientArgs `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $clientOut `
        -RedirectStandardError $clientErr `
        -PassThru `
        -WindowStyle Hidden
} finally {
    Restore-AIPlayLogEnv
}

$clientProc.WaitForExit()
$hostProc.WaitForExit()

$hostText = if (Test-Path $hostOut) { Get-Content $hostOut -Raw } else { "" }
$clientText = if (Test-Path $clientOut) { Get-Content $clientOut -Raw } else { "" }
$hostText = [string]$hostText
$clientText = [string]$clientText
$hostMelonOut = Join-Path $hostLog "host.stdout.txt"
$clientMelonOut = Join-Path $clientLog "client.stdout.txt"
$hostMelonText = if (Test-Path $hostMelonOut) { [string](Get-Content $hostMelonOut -Raw) } else { "" }
$clientMelonText = if (Test-Path $clientMelonOut) { [string](Get-Content $clientMelonOut -Raw) } else { "" }
$hostExitFailed = $null -ne $hostProc.ExitCode -and $hostProc.ExitCode -ne 0
$clientExitFailed = $null -ne $clientProc.ExitCode -and $clientProc.ExitCode -ne 0
if ($hostExitFailed -or
    $clientExitFailed -or
    $hostText -notmatch "NSMB Mario vs Luigi LAN route smoke passed" -or
    $clientText -notmatch "NSMB Mario vs Luigi LAN route smoke passed") {
    $details = @()
    foreach ($path in @($hostOut, $hostErr, $clientOut, $clientErr)) {
        if (Test-Path $path) { $details += Get-Content $path -Raw }
    }
    throw "split local-input child smoke failed: hostExit=$($hostProc.ExitCode) clientExit=$($clientProc.ExitCode) $($details -join "`n")"
}

function Assert-ActiveFrameTiming {
    param(
        [string]$Role,
        [string]$Text,
        [double]$RollbackFrameLimitMs
    )

    $line = ($Text -split "`r?`n") |
        Where-Object { $_ -match "NSMB Test: active frame timing" } |
        Select-Object -Last 1
    if ($null -eq $line) {
        throw "$Role missing active frame timing line"
    }

    if ($line -notmatch "maxFrameMs=([0-9.]+).*over25ms=([0-9]+).*over33ms=([0-9]+)") {
        throw "$Role malformed active frame timing line: $line"
    }

    $maxFrameMs = [double]$Matches[1]
    $over25ms = [int]$Matches[2]
    $over33ms = [int]$Matches[3]
    if ($MaxActiveFrameMs -gt 0.0 -and $maxFrameMs -gt $MaxActiveFrameMs) {
        throw "$Role active frame spike too high: maxFrameMs=$maxFrameMs limit=$MaxActiveFrameMs"
    }
    if ($MaxActiveFrameOver25ms -ge 0 -and $over25ms -gt $MaxActiveFrameOver25ms) {
        throw "$Role active frame over25ms too high: over25ms=$over25ms limit=$MaxActiveFrameOver25ms"
    }
    if ($MaxActiveFrameOver33ms -ge 0 -and $over33ms -gt $MaxActiveFrameOver33ms) {
        throw "$Role active frame over33ms too high: over33ms=$over33ms limit=$MaxActiveFrameOver33ms"
    }

    if ($MaxConsecutiveSlowFrames -ge 0) {
        $maxRun = 0
        $run = 0
        $lastFrame = -1
        foreach ($perfLine in ($Text -split "`r?`n")) {
            if ($perfLine -notmatch "NSMB PerfSpike: .*frame=([0-9]+) frameTimeUs=([0-9]+)") {
                continue
            }

            $frame = [int]$Matches[1]
            $frameMs = [double]$Matches[2] / 1000.0
            if ($frameMs -lt $SlowFrameThresholdMs) {
                continue
            }

            if ($lastFrame -ge 0 -and $frame -eq ($lastFrame + 1)) {
                $run++
            } else {
                $run = 1
            }
            $lastFrame = $frame
            if ($run -gt $maxRun) {
                $maxRun = $run
            }
        }

        if ($maxRun -gt $MaxConsecutiveSlowFrames) {
            throw "$Role consecutive slow frames too high: thresholdMs=$SlowFrameThresholdMs maxRun=$maxRun limit=$MaxConsecutiveSlowFrames"
        }
    }

    if ($RollbackFrameLimitMs -gt 0.0) {
        $maxRollbackFrameMs = 0.0
        $rollbackSpikeCount = 0
        $lastRestores = 0
        $lastResims = 0
        foreach ($perfLine in ($Text -split "`r?`n")) {
            if ($perfLine -notmatch "NSMB PerfSpike: .*frame=([0-9]+) frameTimeUs=([0-9]+)") {
                continue
            }

            $frameMs = [double]$Matches[2] / 1000.0
            $restoreDelta = 0
            $resimDelta = 0
            if ($perfLine -match "rollbackRestoreDelta=([0-9]+).*rollbackResimDelta=([0-9]+)") {
                $restoreDelta = [int]$Matches[1]
                $resimDelta = [int]$Matches[2]
            } elseif ($perfLine -match "rollbackRestores=([0-9]+).*rollbackResims=([0-9]+)") {
                $restores = [int]$Matches[1]
                $resims = [int]$Matches[2]
                $restoreDelta = $restores - $lastRestores
                $resimDelta = $resims - $lastResims
                $lastRestores = $restores
                $lastResims = $resims
            }

            if ($restoreDelta -le 0 -and $resimDelta -le 0) {
                continue
            }

            $rollbackSpikeCount++
            if ($frameMs -gt $maxRollbackFrameMs) {
                $maxRollbackFrameMs = $frameMs
            }
        }

        if ($maxRollbackFrameMs -gt $RollbackFrameLimitMs) {
            throw "$Role rollback frame spike too high: maxRollbackFrameMs=$maxRollbackFrameMs limit=$RollbackFrameLimitMs rollbackSpikeCount=$rollbackSpikeCount"
        }
    }
}

function Assert-RollbackResimCount {
    param(
        [string]$Role,
        [string]$Text
    )

    if ($MinRollbackResims -lt 0) {
        return
    }

    $maxResims = 0
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match "NSMB Rollback: frame=.*resims=([0-9]+)") {
            $value = [int]$Matches[1]
            if ($value -gt $maxResims) {
                $maxResims = $value
            }
        } elseif ($line -match "rollbackResims=([0-9]+)") {
            $value = [int]$Matches[1]
            if ($value -gt $maxResims) {
                $maxResims = $value
            }
        }
    }

    if ($maxResims -lt $MinRollbackResims) {
        throw "$Role rollback resim count too low: resims=$maxResims min=$MinRollbackResims"
    }
}

if ($MaxActiveFrameMs -gt 0.0 -or $MaxActiveFrameOver25ms -ge 0 -or $MaxActiveFrameOver33ms -ge 0 -or $MaxConsecutiveSlowFrames -ge 0 -or $MaxRollbackFrameMs -gt 0.0) {
    Assert-ActiveFrameTiming -Role "host" -Text $hostMelonText -RollbackFrameLimitMs $MaxRollbackFrameMs
    Assert-ActiveFrameTiming -Role "client" -Text $clientMelonText -RollbackFrameLimitMs $MaxRollbackFrameMs
}
if ($MinRollbackResims -ge 0) {
    Assert-RollbackResimCount -Role "host" -Text $hostMelonText
    Assert-RollbackResimCount -Role "client" -Text $clientMelonText
}

function Convert-TraceHexToInt64 {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return 0
    }
    if ($Value.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        return [Convert]::ToInt64($Value.Substring(2), 16)
    }
    return [Convert]::ToInt64($Value, 10)
}

function Convert-TraceHexToSigned32 {
    param([string]$Value)

    $raw = Convert-TraceHexToInt64 $Value
    if ($raw -ge [int64]2147483648) {
        return $raw - [int64]4294967296
    }
    return $raw
}

$hostCsv = Join-Path $hostLog "host.game-state.csv"
$clientCsv = Join-Path $clientLog "client.game-state.csv"
$hostRows = $null
$clientRows = $null
if (-not $NoGameStateTrace -and ($RequireActorSnapshotMovement -or $RequireWorldSnapshotSync -or $RequireMvlManagerGlobalSync -or -not $SkipGameStateComparison)) {
    if (-not (Test-Path $hostCsv)) {
        throw "missing host game-state trace: $hostCsv"
    }
    if (-not (Test-Path $clientCsv)) {
        throw "missing client game-state trace: $clientCsv"
    }
    $hostRows = @(Import-Csv $hostCsv)
    $clientRows = @(Import-Csv $clientCsv)
}

function Assert-ActorSnapshotMovement {
    param(
        [object[]]$HostRows,
        [object[]]$ClientRows
    )

    function Test-RemoteMovement {
        param(
            [string]$Label,
            [object[]]$Rows,
            [string]$FoundField,
            [string]$XField,
            [string]$InputField
        )

        $candidateRows = @($Rows | Where-Object {
            [int]$_.frame -ge $ActorSnapshotStartFrame -and $_.$FoundField -eq "0x1"
        })
        if ($candidateRows.Count -lt 2) {
            throw "$Label actor snapshot movement check failed: rows=$($candidateRows.Count) startFrame=$ActorSnapshotStartFrame"
        }

        $first = $candidateRows | Select-Object -First 1
        $last = $candidateRows | Select-Object -Last 1
        $firstX = Convert-TraceHexToInt64 $first.$XField
        $inputRows = @($candidateRows | Where-Object { (Convert-TraceHexToInt64 $_.$InputField) -ne 0 })
        $movedRows = @($candidateRows | Where-Object { (Convert-TraceHexToInt64 $_.$XField) -ne $firstX })
        if ($movedRows.Count -lt $ActorSnapshotMinMovedRows) {
            throw "$Label actor snapshot movement check failed: rows=$($candidateRows.Count) inputRows=$($inputRows.Count) movedRows=$($movedRows.Count) minMoved=$ActorSnapshotMinMovedRows firstX=$($first.$XField) lastX=$($last.$XField) lastInput=$($last.$InputField)"
        }
        Write-Host "$Label actor snapshot movement check passed: rows=$($candidateRows.Count) inputRows=$($inputRows.Count) movedRows=$($movedRows.Count) firstX=$($first.$XField) lastX=$($last.$XField)"
    }

    Test-RemoteMovement `
        -Label "client remote player0" `
        -Rows $ClientRows `
        -FoundField "playerActor0Found" `
        -XField "playerActor0X" `
        -InputField "inputPlayer0Held"
    Test-RemoteMovement `
        -Label "host remote player1" `
        -Rows $HostRows `
        -FoundField "playerActor1Found" `
        -XField "playerActor1X" `
        -InputField "inputPlayer1Held"

    if ($ActorSnapshotMaxDriftX -lt 0 -and $ActorSnapshotMaxDriftY -lt 0) {
        return
    }

    $hostByFrame = @{}
    foreach ($row in $HostRows) {
        $hostByFrame[[int]$row.frame] = $row
    }
    $clientByFrame = @{}
    foreach ($row in $ClientRows) {
        $clientByFrame[[int]$row.frame] = $row
    }

    $maxDriftX = 0
    $maxDriftY = 0
    $checked = 0
    $skippedTransitionRows = 0
    $driftRuns = @{}
    $maxConsecutiveDriftRows = 0
    foreach ($frame in $hostByFrame.Keys) {
        if ($frame -lt $ActorSnapshotStartFrame -or -not $clientByFrame.ContainsKey($frame)) {
            continue
        }

        $hostRow = $hostByFrame[$frame]
        $clientRow = $clientByFrame[$frame]
        foreach ($pair in @(
            @{ Label = "player0"; Local = $hostRow; Remote = $clientRow; X = "playerActor0X"; Y = "playerActor0Y"; Found = "playerActor0Found"; Dead = "player0Dead"; Step = "playerActor0TransitionStep" },
            @{ Label = "player1"; Local = $clientRow; Remote = $hostRow; X = "playerActor1X"; Y = "playerActor1Y"; Found = "playerActor1Found"; Dead = "player1Dead"; Step = "playerActor1TransitionStep" }
        )) {
            if ($pair.Local.($pair.Found) -ne "0x1" -or $pair.Remote.($pair.Found) -ne "0x1") {
                continue
            }
            if ((Convert-TraceHexToInt64 $pair.Local.($pair.Dead)) -ne 0 -or
                (Convert-TraceHexToInt64 $pair.Remote.($pair.Dead)) -ne 0 -or
                (Convert-TraceHexToInt64 $pair.Local.($pair.Step)) -ne 1 -or
                (Convert-TraceHexToInt64 $pair.Remote.($pair.Step)) -ne 1) {
                $skippedTransitionRows++
                $driftRuns[$pair.Label] = 0
                continue
            }

            $dx = [Math]::Abs((Convert-TraceHexToSigned32 $pair.Local.($pair.X)) - (Convert-TraceHexToSigned32 $pair.Remote.($pair.X)))
            $dy = [Math]::Abs((Convert-TraceHexToSigned32 $pair.Local.($pair.Y)) - (Convert-TraceHexToSigned32 $pair.Remote.($pair.Y)))
            if ($dx -gt $maxDriftX) { $maxDriftX = $dx }
            if ($dy -gt $maxDriftY) { $maxDriftY = $dy }
            $checked++
            $overDriftLimit =
                ($ActorSnapshotMaxDriftX -ge 0 -and $dx -gt $ActorSnapshotMaxDriftX) -or
                ($ActorSnapshotMaxDriftY -ge 0 -and $dy -gt $ActorSnapshotMaxDriftY)
            if ($overDriftLimit) {
                $driftRuns[$pair.Label] = 1 + [int]$driftRuns[$pair.Label]
                if ($driftRuns[$pair.Label] -gt $maxConsecutiveDriftRows) {
                    $maxConsecutiveDriftRows = $driftRuns[$pair.Label]
                }
                if ($driftRuns[$pair.Label] -gt $ActorSnapshotMaxConsecutiveDriftRows) {
                    throw "$($pair.Label) actor snapshot drift too high: frame=$frame dx=$dx limitX=$ActorSnapshotMaxDriftX dy=$dy limitY=$ActorSnapshotMaxDriftY consecutiveRows=$($driftRuns[$pair.Label]) limitRows=$ActorSnapshotMaxConsecutiveDriftRows local=$($pair.Local.($pair.X))/$($pair.Local.($pair.Y)) remote=$($pair.Remote.($pair.X))/$($pair.Remote.($pair.Y))"
                }
            } else {
                $driftRuns[$pair.Label] = 0
            }
        }
    }

    if ($checked -eq 0) {
        throw "actor snapshot drift check failed: no comparable actor rows after frame $ActorSnapshotStartFrame"
    }
    Write-Host "actor snapshot movement/drift check passed: checked=$checked skippedTransitionRows=$skippedTransitionRows maxDriftX=$maxDriftX maxDriftY=$maxDriftY maxConsecutiveDriftRows=$maxConsecutiveDriftRows"
}

if ($RequireActorSnapshotMovement) {
    Assert-ActorSnapshotMovement -HostRows $hostRows -ClientRows $clientRows
}

function Assert-WorldSnapshotSync {
    param(
        [object[]]$HostRows,
        [object[]]$ClientRows
    )

    $clientByFrame = @{}
    foreach ($row in $ClientRows) {
        $clientByFrame[[int]$row.frame] = $row
    }

    $starChecked = 0
    $starMaxDriftX = 0
    $starMaxDriftY = 0
    $hazardChecked = 0
    $hazardMaxDriftX = 0
    $hazardMaxDriftY = 0
    $hazardDriftRun = 0
    $hazardMaxConsecutiveDriftRows = 0
    foreach ($hostRow in $HostRows) {
        $frame = [int]$hostRow.frame
        if ($frame -lt $WorldSnapshotStartFrame -or -not $clientByFrame.ContainsKey($frame)) {
            continue
        }

        $clientRow = $clientByFrame[$frame]
        if ($hostRow.vsStarActorFound -eq "0x1" -and $clientRow.vsStarActorFound -eq "0x1") {
            $starDx = [Math]::Abs((Convert-TraceHexToSigned32 $hostRow.vsStarActorX) - (Convert-TraceHexToSigned32 $clientRow.vsStarActorX))
            $starDy = [Math]::Abs((Convert-TraceHexToSigned32 $hostRow.vsStarActorY) - (Convert-TraceHexToSigned32 $clientRow.vsStarActorY))
            if ($starDx -gt $starMaxDriftX) { $starMaxDriftX = $starDx }
            if ($starDy -gt $starMaxDriftY) { $starMaxDriftY = $starDy }
            $starChecked++
            if ($starDx -gt $WorldSnapshotMaxStarDriftX -or $starDy -gt $WorldSnapshotMaxStarDriftY) {
                throw "world snapshot Big Star drift too high: frame=$frame dx=$starDx limitX=$WorldSnapshotMaxStarDriftX dy=$starDy limitY=$WorldSnapshotMaxStarDriftY host=$($hostRow.vsStarActorX)/$($hostRow.vsStarActorY) client=$($clientRow.vsStarActorX)/$($clientRow.vsStarActorY)"
            }
        }

        if ($hostRow.movingHazardFound -eq "0x1" -and $clientRow.movingHazardFound -eq "0x1") {
            $hazardDx = [Math]::Abs((Convert-TraceHexToSigned32 $hostRow.movingHazardX) - (Convert-TraceHexToSigned32 $clientRow.movingHazardX))
            $hazardDy = [Math]::Abs((Convert-TraceHexToSigned32 $hostRow.movingHazardY) - (Convert-TraceHexToSigned32 $clientRow.movingHazardY))
            if ($hazardDx -gt $hazardMaxDriftX) { $hazardMaxDriftX = $hazardDx }
            if ($hazardDy -gt $hazardMaxDriftY) { $hazardMaxDriftY = $hazardDy }
            $hazardChecked++
            $hazardOverLimit =
                ($WorldSnapshotMaxHazardDriftX -ge 0 -and $hazardDx -gt $WorldSnapshotMaxHazardDriftX) -or
                ($WorldSnapshotMaxHazardDriftY -ge 0 -and $hazardDy -gt $WorldSnapshotMaxHazardDriftY)
            if ($hazardOverLimit) {
                $hazardDriftRun++
                if ($hazardDriftRun -gt $hazardMaxConsecutiveDriftRows) {
                    $hazardMaxConsecutiveDriftRows = $hazardDriftRun
                }
                if ($hazardDriftRun -gt $WorldSnapshotMaxHazardConsecutiveDriftRows) {
                    throw "world snapshot moving hazard drift too high: frame=$frame dx=$hazardDx limitX=$WorldSnapshotMaxHazardDriftX dy=$hazardDy limitY=$WorldSnapshotMaxHazardDriftY consecutiveRows=$hazardDriftRun limitRows=$WorldSnapshotMaxHazardConsecutiveDriftRows"
                }
            } else {
                $hazardDriftRun = 0
            }
        } else {
            $hazardDriftRun = 0
        }
    }

    if ($starChecked -eq 0) {
        throw "world snapshot Big Star check failed: no comparable rows after frame $WorldSnapshotStartFrame"
    }
    Write-Host "world snapshot check passed: starChecked=$starChecked starMaxDriftX=$starMaxDriftX starMaxDriftY=$starMaxDriftY hazardChecked=$hazardChecked hazardMaxDriftX=$hazardMaxDriftX hazardMaxDriftY=$hazardMaxDriftY hazardMaxConsecutiveDriftRows=$hazardMaxConsecutiveDriftRows"
}

if ($RequireWorldSnapshotSync) {
    Assert-WorldSnapshotSync -HostRows $hostRows -ClientRows $clientRows
}

function Assert-MvlManagerGlobalSync {
    param(
        [object[]]$HostRows,
        [object[]]$ClientRows
    )

    $fields = @(
        "player0Coins", "player1Coins", "vsCoinCount",
        "mvlGlobal965C", "mvlGlobal9670", "mvlGlobal9674", "mvlGlobal9694_0", "mvlGlobal9694_1",
        "mvlManagerStateType", "mvlManagerFlags", "mvlManagerUnk54",
        "mvlManagerWordA8CC", "mvlManagerWordA8D0", "mvlManagerWordA8D4", "mvlManagerWordA8D8",
        "mvlManagerWordA8DC", "mvlManagerWordA8E0", "mvlManagerWordA8E4",
        "mvlManagerHalfA8E8", "mvlManagerHalfA8EA", "mvlManagerByteA8EC",
        "mvlManagerHalf494", "mvlManagerHalf4A0",
        "stageSceneWord154", "stageSceneWord160", "stageSceneWord5618", "stageSceneWord561C",
        "stageSceneWord563C", "stageSceneByte5643", "stageSceneByte5644", "stageSceneByte5645",
        "stageSceneByte5646", "stageSceneByte5648", "stageSceneByte5649"
    )

    $clientByFrame = @{}
    foreach ($row in $ClientRows) {
        $clientByFrame[[int]$row.frame] = $row
    }

    $checkedRows = 0
    $managerObservedRows = 0
    foreach ($hostRow in $HostRows) {
        $frame = [int]$hostRow.frame
        if ($frame -lt $MvlManagerGlobalSnapshotStartFrame -or -not $clientByFrame.ContainsKey($frame)) {
            continue
        }

        $clientRow = $clientByFrame[$frame]
        $checkedRows++
        if ($hostRow.mvlManagerBase -ne "0x0" -or $clientRow.mvlManagerBase -ne "0x0") {
            $managerObservedRows++
        }
        foreach ($field in $fields) {
            if ($hostRow.$field -ne $clientRow.$field) {
                throw "MvL manager/global snapshot mismatch: frame=$frame field=$field host=$($hostRow.$field) client=$($clientRow.$field)"
            }
        }
    }

    if ($checkedRows -eq 0) {
        throw "MvL manager/global snapshot check failed: no comparable rows after frame $MvlManagerGlobalSnapshotStartFrame"
    }
    Write-Host "MvL manager/global snapshot check passed: checkedRows=$checkedRows managerObservedRows=$managerObservedRows fields=$($fields.Count)"
}

if ($RequireMvlManagerGlobalSync) {
    Assert-MvlManagerGlobalSync -HostRows $hostRows -ClientRows $clientRows
}

if ($RequireNoUnexpectedWorldLifecycleDiff) {
    if (-not $WorldStateTraceObjectLifecycles) {
        throw "-RequireNoUnexpectedWorldLifecycleDiff requires -WorldStateTraceObjectLifecycles"
    }
    & (Join-Path $PSScriptRoot "analyze-nsmb-mvl-object-lifecycle-diff.ps1") `
        -LogDir $logRoot `
        -IgnoreActors "012/*,0F0/01080002" `
        -FailOnDifference
}

if ($NoGameStateTrace -or $SkipGameStateComparison) {
    Get-Content $hostOut
    Get-Content $clientOut
    Write-Host "NSMB Mario vs Luigi split local-input smoke passed without game-state comparison: frames=$Frames log=$logRoot"
    return
}

$clientByFrame = @{}
foreach ($row in $clientRows) {
    $clientByFrame[[int]$row.frame] = $row
}

$stableFields = @(
    "stageID", "stageGroup", "vsMode", "sceneCurrentSceneID",
    "vsStarActorFound", "vsStarActorX", "vsStarActorY",
    "playerActor0Found", "playerActor0X", "playerActor0Y", "playerActor0Z",
    "playerActor1Found", "playerActor1X", "playerActor1Y", "playerActor1Z",
    "movingHazardFound", "movingHazardX", "movingHazardY",
    "objectActiveCount", "objectDeadCount",
    "player0Lives", "player1Lives", "player0BattleStars", "player1BattleStars",
    "player0Dead", "player1Dead", "player0InventoryPowerup", "player1InventoryPowerup",
    "playerGlobalHash", "wifiCandidateHash",
    "playerActor0UpdateLocked", "playerActor1UpdateLocked",
    "playerActor0VisibleFlag", "playerActor1VisibleFlag"
)
$inputFields = @("inputPlayer0Held", "inputPlayer1Held", "inputPlayer0Pressed", "inputPlayer1Pressed")
$fields = if ($IgnoreSpeculativeInputFields) {
    $stableFields
} else {
    @($stableFields + $inputFields)
}
$defaultSettleFields = @(
    "movingHazardX", "movingHazardY",
    "playerActor0VisibleFlag", "playerActor1VisibleFlag"
)

function RowAtFrame {
    param([object[]]$Rows, [int]$Frame)
    return $Rows | Where-Object { [int]$_.frame -eq $Frame } | Select-Object -First 1
}

function RowsMatchFields {
    param([object]$HostRow, [object]$ClientRow, [string[]]$Fields)
    foreach ($field in $Fields) {
        if ($HostRow.$field -ne $ClientRow.$field) {
            return $false
        }
    }
    return $true
}

foreach ($hostRow in $hostRows) {
    $frame = [int]$hostRow.frame
    if ($frame -lt 900) { continue }
    if (-not $clientByFrame.ContainsKey($frame)) {
        throw "missing client frame $frame"
    }
    $clientRow = $clientByFrame[$frame]
    foreach ($field in $fields) {
        if ($hostRow.$field -ne $clientRow.$field) {
            $settleFrames = $RollbackSettleFrames
            if ($settleFrames -le 0 -and $defaultSettleFields -contains $field) {
                $settleFrames = [Math]::Max(1, $GameStateTraceInterval)
            }
            if ($settleFrames -gt 0) {
                for ($settleFrame = $frame + 1; $settleFrame -le $frame + $settleFrames; $settleFrame++) {
                    $hostSettle = RowAtFrame -Rows $hostRows -Frame $settleFrame
                    $clientSettle = if ($clientByFrame.ContainsKey($settleFrame)) { $clientByFrame[$settleFrame] } else { $null }
                    if ($null -ne $hostSettle -and $null -ne $clientSettle -and
                        (RowsMatchFields -HostRow $hostSettle -ClientRow $clientSettle -Fields $fields)) {
                        Write-Host "rollback transient mismatch settled frame=$frame settleFrame=$settleFrame field=$field"
                        break
                    }
                }
                if ($settleFrame -le $frame + $settleFrames) {
                    break
                }
            }
            throw "gameplay mismatch frame=$frame field=$field host=$($hostRow.$field) client=$($clientRow.$field)"
        }
    }
}

if (-not $SkipMovementProbe) {
    $before = RowAtFrame -Rows $hostRows -Frame 1770
    $after = RowAtFrame -Rows $hostRows -Frame 2220
    if ($null -eq $before -or $null -eq $after) {
        throw "missing movement probe rows"
    }
    if ($before.playerActor0X -eq $after.playerActor0X) {
        throw "Mario did not move in host local-input probe"
    }
    if ($before.playerActor1X -eq $after.playerActor1X) {
        throw "Luigi did not move in client local-input probe"
    }
}

Get-Content $hostOut
Get-Content $clientOut
Write-Host "NSMB Mario vs Luigi split local-input smoke passed: frames=$Frames log=$logRoot"
