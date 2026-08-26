#include "NsmbRollbackStore.h"
#include "NSMLGameRAMRollback.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using NsmbMvlNetplay::RollbackStorage::CheckpointBytes;
using NsmbMvlNetplay::RollbackStorage::ClampResimulationMismatch;
using NsmbMvlNetplay::RollbackStorage::DeltaMode;
using NsmbMvlNetplay::RollbackStorage::IsResimulationDelayElapsed;
using NsmbMvlNetplay::RollbackStorage::ShouldSaveCheckpoint;
using NsmbMvlNetplay::RollbackStorage::ShouldSaveResimulationCheckpoint;
using NsmbMvlNetplay::RollbackStorage::Store;
using NsmbMvlNetplay::RollbackStorage::StoredState;
using NsmbMvlNetplay::RollbackStorage::Statistics;
using melonDS::NSMLGameRAMRollback::CanFinalizeTransaction;
using melonDS::NSMLGameRAMRollback::CheckpointFrameTimeline;
using melonDS::NSMLGameRAMRollback::MaxRollbackDepthForHistory;
using melonDS::NSMLGameRAMRollback::RequiredHistoryCount;

void Require(bool condition, const std::string &message) {
  if (condition)
    return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

StoredState Keyframe(std::initializer_list<melonDS::u8> mainRAM = {}) {
  StoredState state;
  state.Buffer = {'k'};
  state.MainRAMCopy.assign(mainRAM);
  return state;
}

StoredState Delta(melonDS::u32 baseFrame) {
  StoredState state;
  state.Buffer = {'d'};
  state.MainRAMDelta = true;
  state.BaseFrame = baseFrame;
  return state;
}

StoredState Preimage(melonDS::u32 baseFrame) {
  StoredState state;
  state.Buffer = {'p'};
  state.MainRAMFramePreimage = true;
  state.BaseFrame = baseFrame;
  return state;
}

void TestCheckpointBytes() {
  StoredState state;
  state.Buffer.resize(5);
  state.MainRAMPreimagePages.resize(3);
  state.MainRAMPreimage.resize(7);
  state.MainRAMCopy.resize(100);
  state.MainRAMShadowCopy.resize(200);
  Require(CheckpointBytes(state) == 5 + 3 * sizeof(melonDS::u32) + 7,
          "checkpoint byte accounting must preserve the production metric");
}

void TestRollbackPolicies() {
  Require(ShouldSaveCheckpoint(7, 0, 100) &&
              ShouldSaveCheckpoint(7, 1, 100),
          "checkpoint intervals zero and one save every frame");
  Require(ShouldSaveCheckpoint(101, 10, 101),
          "netplay start frame forces a checkpoint");
  Require(ShouldSaveCheckpoint(120, 10, 101) &&
              !ShouldSaveCheckpoint(121, 10, 101),
          "checkpoint interval follows frame modulo");

  Require(ClampResimulationMismatch(90, 100, 0) == 90 &&
              ClampResimulationMismatch(90, 100, -1) == 90,
          "disabled resimulation cap preserves mismatch");
  Require(ClampResimulationMismatch(95, 100, 10) == 95,
          "mismatch inside cap is preserved");
  Require(ClampResimulationMismatch(80, 100, 10) == 90,
          "mismatch outside cap is clamped");
  Require(ClampResimulationMismatch(100, 100, 10) == 100,
          "current-frame mismatch is not rewritten");

  Require(IsResimulationDelayElapsed(100, 90, 0) &&
              IsResimulationDelayElapsed(100, std::nullopt, 5),
          "disabled or unobserved delay is immediately elapsed");
  Require(!IsResimulationDelayElapsed(104, 100, 5) &&
              IsResimulationDelayElapsed(105, 100, 5),
          "resimulation waits through the configured observed-frame delay");

  Require(ShouldSaveResimulationCheckpoint(99, 100, false) &&
              !ShouldSaveResimulationCheckpoint(99, 100, true),
          "only requested intermediate resimulation checkpoints should save");
  Require(!ShouldSaveResimulationCheckpoint(100, 100, false) &&
              !ShouldSaveResimulationCheckpoint(101, 100, false),
          "the ordinary before-frame path should own the final checkpoint");
}

void TestRomLoopTransactionCompletionPolicy() {
  Require(!CanFinalizeTransaction(true, false, true, 8, 8, 108, 100),
          "pending RAM restore cannot finalize");
  Require(!CanFinalizeTransaction(false, false, false, 8, 8, 108, 100),
          "inactive history cannot finalize from stale counters");
  Require(!CanFinalizeTransaction(false, false, true, 7, 8, 108, 100),
          "unfinished history index cannot finalize");
  Require(!CanFinalizeTransaction(false, false, true, 8, 8, 107, 100),
          "unfinished game frame cannot finalize");
  Require(CanFinalizeTransaction(false, false, true, 8, 8, 108, 100),
          "completed replay finalizes at the outer-frame boundary");
  Require(CanFinalizeTransaction(false, true, false, 8, 8, 108, 100),
          "legacy exit-gate completion remains valid");
  Require(!CanFinalizeTransaction(false, false, true, 1, 0, 100, 100),
          "empty history cannot finalize");
  Require(!CanFinalizeTransaction(false, false, true, 8, 8, 2, 0xFFFFFFFE),
          "wrapped game-frame interval cannot finalize");
}

void TestRomLoopHistoryBoundaryPolicy() {
  Require(RequiredHistoryCount(100, 100) == 2,
          "same-frame history includes the following logical input gate");
  Require(RequiredHistoryCount(100, 105) == 7,
          "history replays from the checkpoint through current plus one");
  Require(RequiredHistoryCount(105, 100) == 0,
          "backwards history range is rejected");
  Require(RequiredHistoryCount(0xFFFFFFFE, 0xFFFFFFFF) == 0,
          "history range that cannot represent the following frame is rejected");
  Require(RequiredHistoryCount(0, 0xFFFFFFFE) == 0,
          "overflowing history count is rejected");
  Require(MaxRollbackDepthForHistory(12) == 10,
          "twelve history entries leave room for both inclusive boundaries");
  Require(MaxRollbackDepthForHistory(1) == 0,
          "undersized history cannot accept rollback depth");
}

void TestRomLoopCheckpointFrameTimeline() {
  CheckpointFrameTimeline timeline;
  Require(timeline.CaptureFrame(866) == 866,
          "pre-handshake checkpoint labels fall back to raw frames");
  Require(timeline.SetLogicalFrame(840),
          "the first logical frame invalidates raw-frame checkpoints");
  Require(timeline.CaptureFrame(866) == 840,
          "checkpoint labels use the generation-local logical frame");
  Require(!timeline.SetLogicalFrame(841) && timeline.CaptureFrame(867) == 841,
          "monotonic logical frames preserve the current checkpoint ring");
  Require(!timeline.SetLogicalFrame(841),
          "repeated before-frame hooks do not invalidate checkpoints");
  Require(timeline.SetLogicalFrame(840),
          "a backwards logical frame invalidates the previous generation");
  timeline.Reset();
  Require(timeline.CaptureFrame(12) == 12,
          "reset restores raw-frame fallback until the next handshake");
}

void TestRestoreChain() {
  Store store;
  store.Put(10, Keyframe());
  store.Put(11, Delta(10));
  store.Put(12, Delta(11));

  std::vector<StoredState> chain;
  Require(store.BuildRestoreChain(12, chain), "complete delta chain");
  Require(chain.size() == 3, "delta chain length");
  Require(!chain[0].MainRAMDelta && chain[1].BaseFrame == 10 &&
              chain[2].BaseFrame == 11,
          "delta chain must be ordered keyframe to target");

  store.Put(13, Delta(99));
  Require(!store.BuildRestoreChain(13, chain), "missing delta base rejected");
  store.Put(14, Delta(14));
  Require(!store.BuildRestoreChain(14, chain), "cyclic delta base rejected");
}

void TestPrepareSaveModes() {
  Store store;
  StoredState checkpoint;
  std::vector<melonDS::u8> base;

  store.PrepareSave(10, DeltaMode::KeyframeDelta, 10, 0, checkpoint, base);
  Require(!checkpoint.MainRAMDelta && base.empty(),
          "empty store forces keyframe");

  store.Put(10, Keyframe({1, 2, 3, 4}));
  store.PrepareSave(11, DeltaMode::KeyframeDelta, 10, 0, checkpoint, base);
  Require(checkpoint.MainRAMDelta && checkpoint.BaseFrame == 10 &&
              base == std::vector<melonDS::u8>({1, 2, 3, 4}),
          "keyframe delta uses latest complete RAM copy");

  store.UpdateFrameShadow(10, base.data(), base.size());
  store.PrepareSave(11, DeltaMode::FrameDelta, 10, 0, checkpoint, base);
  Require(checkpoint.MainRAMDelta && checkpoint.BaseFrame == 10,
          "frame delta uses valid shadow");

  store.PrepareSave(10, DeltaMode::FrameDelta, 10, 10, checkpoint, base);
  Require(!checkpoint.MainRAMDelta && base.empty(),
          "netplay start frame forces keyframe");

  const melonDS::u8 shadow[] = {9, 8, 7};
  store.UpdateFrameShadow(10, shadow, sizeof(shadow));
  store.PrepareSave(11, DeltaMode::Preimage, 10, 0, checkpoint, base);
  Require(checkpoint.MainRAMFramePreimage && checkpoint.BaseFrame == 10 &&
              base == std::vector<melonDS::u8>({9, 8, 7}),
          "preimage mode uses valid frame shadow");
}

void TestPreimageRestoreAndPrune() {
  Store store;
  store.Put(1, Keyframe());
  store.Put(2, Preimage(1));
  store.Put(3, Preimage(2));
  store.Put(20, Keyframe());
  const melonDS::u8 shadow[] = {4, 3, 2, 1};
  store.UpdateFrameShadow(3, shadow, sizeof(shadow));

  std::vector<StoredState> reverse;
  std::vector<melonDS::u8> latest;
  Require(store.BuildPreimageRestore(1, reverse, latest),
          "complete reverse preimage chain");
  Require(reverse.size() == 2 && reverse[0].BaseFrame == 2 &&
              reverse[1].BaseFrame == 1,
          "preimage chain must run newest to target");
  Require(latest == std::vector<melonDS::u8>({4, 3, 2, 1}),
          "latest RAM shadow copied for restoration");

  store.Prune(20, 5);
  Require(store.Size() == 2 && store.States().count(3) == 1 &&
              store.States().count(20) == 1,
          "preimage prune preserves the current shadow frame contract");

  store.UpdateFrameShadow(20, shadow, sizeof(shadow));
  store.Prune(20, 5);
  Require(store.Size() == 1 && store.States().count(20) == 1,
          "unreferenced history outside rollback window is pruned");
}

void TestPrunePreservesDeltaDependencies() {
  Store store;
  store.Put(1, Keyframe());
  store.Put(2, Delta(1));
  store.Put(3, Delta(2));
  store.Put(20, Keyframe());
  const melonDS::u8 shadow[] = {1};
  store.UpdateFrameShadow(3, shadow, sizeof(shadow));

  store.Prune(20, 5);
  Require(store.Size() == 4,
          "prune preserves the shadow's complete delta dependency chain");
}

void TestLatestAndEraseAfter() {
  Store store;
  store.Put(5, Keyframe());
  store.Put(10, Keyframe());
  store.Put(15, Keyframe());

  melonDS::u32 frame = 0;
  StoredState checkpoint;
  Require(store.LatestAtOrBefore(12, frame, checkpoint) && frame == 10,
          "latest checkpoint at or before mismatch");
  Require(!store.LatestAtOrBefore(4, frame, checkpoint),
          "no checkpoint before history start");

  store.EraseAfter(10);
  Require(store.Size() == 2 && store.States().count(15) == 0,
          "future checkpoints erased before resimulation");
}

void TestStatistics() {
  Statistics statistics;
  auto snapshot = statistics.Snapshot();
  Require(snapshot.AverageCheckpointBytes() == 0 &&
              snapshot.AverageCheckpointSaveUs() == 0 &&
              snapshot.AverageCheckpointRestoreUs() == 0 &&
              snapshot.AverageResimRunFrameUs() == 0 &&
              snapshot.AverageResimCheckpointSaveUs() == 0 &&
              snapshot.AverageResimCorrectionUs() == 0,
          "empty statistics averages are zero");

  statistics.RecordCheckpointSave(100, 10);
  statistics.RecordCheckpointSave(50, 30);
  statistics.RecordCheckpointRestore(7);
  statistics.RecordCheckpointRestore(13);
  statistics.RecordProbeRestore();
  statistics.RecordProbeRestore();
  statistics.RecordResimulation(3, 90, 40, 30, 20, 200);
  statistics.RecordResimulation(2, 50, 30, 10, 6, 100);

  snapshot = statistics.Snapshot();
  Require(snapshot.CheckpointSaveCount == 2 &&
              snapshot.CheckpointLastBytes == 50 &&
              snapshot.CheckpointMinBytes == 50 &&
              snapshot.CheckpointMaxBytes == 100 &&
              snapshot.CheckpointTotalBytes == 150 &&
              snapshot.AverageCheckpointBytes() == 75,
          "checkpoint byte statistics");
  Require(snapshot.CheckpointSaveTotalUs == 40 &&
              snapshot.CheckpointSaveMaxUs == 30 &&
              snapshot.AverageCheckpointSaveUs() == 20,
          "checkpoint save timing statistics");
  Require(snapshot.CheckpointRestoreOpCount == 2 &&
              snapshot.CheckpointRestoreTotalUs == 20 &&
              snapshot.CheckpointRestoreMaxUs == 13 &&
              snapshot.AverageCheckpointRestoreUs() == 10 &&
              snapshot.RestoreCount == 2,
          "checkpoint restore statistics");
  Require(snapshot.ResimulateCount == 2 &&
              snapshot.MeasuredResimOpCount == 2 &&
              snapshot.MeasuredResimFrameCount == 5 &&
              snapshot.ResimRunFrameTotalUs == 140 &&
              snapshot.ResimRunFrameMaxUs == 40 &&
              snapshot.AverageResimRunFrameUs() == 28,
          "resimulation run statistics");
  Require(snapshot.ResimCheckpointSaveTotalUs == 40 &&
              snapshot.ResimCheckpointSaveMaxUs == 20 &&
              snapshot.AverageResimCheckpointSaveUs() == 8 &&
              snapshot.ResimCorrectionTotalUs == 300 &&
              snapshot.ResimCorrectionMaxUs == 200 &&
              snapshot.AverageResimCorrectionUs() == 150,
          "resimulation checkpoint and total statistics");

  Require(!statistics.ShouldTrace(120, 0), "zero trace interval disabled");
  Require(!statistics.ShouldTrace(119, 120), "off-interval trace rejected");
  Require(statistics.ShouldTrace(120, 120), "first interval frame traced");
  Require(!statistics.ShouldTrace(120, 120), "duplicate frame suppressed");
  Require(statistics.ShouldTrace(240, 120), "next interval frame traced");
}

} // namespace

int main() {
  TestCheckpointBytes();
  TestRollbackPolicies();
  TestRomLoopTransactionCompletionPolicy();
  TestRomLoopHistoryBoundaryPolicy();
  TestRomLoopCheckpointFrameTimeline();
  TestRestoreChain();
  TestPrepareSaveModes();
  TestPreimageRestoreAndPrune();
  TestPrunePreservesDeltaDependencies();
  TestLatestAndEraseAfter();
  TestStatistics();
  std::cout << "NsmbRollbackStore tests passed\n";
  return 0;
}
