#ifndef NSML_GAME_RAM_ROLLBACK_H
#define NSML_GAME_RAM_ROLLBACK_H

#include "types.h"

namespace melonDS::NSMLGameRAMRollback
{

constexpr u32 RequiredHistoryCount(u32 restoreFrame, u32 currentFrame)
{
    // The restored checkpoint is the state at the start of restoreFrame. The
    // ROM loop must replay through the input gate after currentFrame so the
    // corrected checkpoint ring owns the next logical frame as well.
    if (currentFrame < restoreFrame || currentFrame == 0xFFFFFFFFu)
        return 0;
    const u32 rollbackDepth = currentFrame - restoreFrame;
    if (rollbackDepth > 0xFFFFFFFDu)
        return 0;
    return rollbackDepth + 2;
}

constexpr u32 MaxRollbackDepthForHistory(u32 historyCapacity)
{
    return historyCapacity >= 2 ? historyCapacity - 2 : 0;
}

class CheckpointFrameTimeline
{
public:
    void Reset()
    {
        Active = false;
        LogicalFrame = 0;
    }

    bool SetLogicalFrame(u32 frame)
    {
        const bool invalidatesCheckpoints = !Active || frame < LogicalFrame;
        Active = true;
        LogicalFrame = frame;
        return invalidatesCheckpoints;
    }

    u32 CaptureFrame(u32 rawFrame) const
    {
        return Active ? LogicalFrame : rawFrame;
    }

private:
    bool Active = false;
    u32 LogicalFrame = 0;
};

constexpr bool CanFinalizeTransaction(
    bool restorePending,
    bool historyReachedExitGate,
    bool historyEnabled,
    u32 historyIndex,
    u32 historyCount,
    u32 gameFrame,
    u32 historyStartFrame)
{
    if (restorePending || historyCount == 0)
        return false;
    if (!historyReachedExitGate && !historyEnabled)
        return false;
    if (historyIndex < historyCount || gameFrame < historyStartFrame)
        return false;
    return gameFrame - historyStartFrame >= historyCount;
}

} // namespace melonDS::NSMLGameRAMRollback

#endif
