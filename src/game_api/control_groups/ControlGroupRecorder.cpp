#include "ControlGroupRecorder.h"

#include <utility>

namespace rareplay::game_api
{
bool ControlGroupRecorder::Capture(std::uint32_t frameNumber, std::string& error)
{
    replay::RecordedControlGroupSnapshot snapshot;
    if (!mReader || !mReader(snapshot, error))
    {
        if (error.empty()) error = "无法采集原生编队";
        return false;
    }
    std::optional<replay::RecordedControlGroupSnapshot> candidate(std::move(snapshot));
    if (!replay::ValidateRecordedControlGroups(candidate, frameNumber, 3, error))
        return false;
    mCaptured = std::move(candidate);
    error.clear();
    return true;
}

bool ControlGroupRecorder::CaptureAfterNativeUpdate(std::string& error)
{
    if (!Capture(0, error))
        return false;
    ++mStats.mNativeUpdateCount;
    return true;
}

bool ControlGroupRecorder::CopyFrameUpdate(std::uint32_t frameNumber,
    std::optional<replay::RecordedControlGroupSnapshot>& outUpdate, std::string& error)
{
    if (!Capture(frameNumber, error))
        return false;
    if (!mLastOutput || *mCaptured != *mLastOutput || frameNumber == 0)
    {
        outUpdate = mCaptured;
        mLastOutput = mCaptured;
        ++mStats.mSnapshotUpdateCount;
    }
    else
        outUpdate.reset();
    error.clear();
    return true;
}

void ControlGroupRecorder::Reset() noexcept
{
    mCaptured.reset();
    mLastOutput.reset();
    mStats = {};
}
}
