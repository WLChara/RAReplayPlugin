#include "PlaybackSeek.h"

namespace rareplay::plugin
{
bool PlaybackSeek::Begin(std::uint32_t lastCompletedFrame, std::uint32_t targetFrame, bool wasPaused,
    std::int64_t nowMs) noexcept
{
    if (mIsActive || targetFrame <= lastCompletedFrame)
        return false;
    mIsActive = true;
    mStartFrame = lastCompletedFrame;
    mTargetFrame = targetFrame;
    mLastCompletedFrame = lastCompletedFrame;
    mWasPaused = wasPaused;
    mBeginMs = nowMs;
    mLastDrawMs = nowMs;
    return true;
}

bool PlaybackSeek::Retarget(std::uint32_t targetFrame) noexcept
{
    if (!mIsActive || targetFrame <= mTargetFrame)
        return false;
    mTargetFrame = targetFrame;
    return true;
}

bool PlaybackSeek::OnFrameCompleted(std::uint32_t lastCompletedFrame) noexcept
{
    if (!mIsActive)
        return false;
    if (lastCompletedFrame > mLastCompletedFrame)
        mLastCompletedFrame = lastCompletedFrame;
    return mLastCompletedFrame >= mTargetFrame;
}

bool PlaybackSeek::ShouldDraw(std::int64_t nowMs) const noexcept
{
    return mIsActive && nowMs - mLastDrawMs >= SEEK_DRAW_INTERVAL_MS;
}

void PlaybackSeek::MarkDrawn(std::int64_t nowMs) noexcept
{
    if (mIsActive)
        mLastDrawMs = nowMs;
}

float PlaybackSeek::GetProgress() const noexcept
{
    if (!mIsActive)
        return 0.0f;
    return viewer::GetSeekProgress(mStartFrame, mTargetFrame, mLastCompletedFrame);
}

std::optional<SeekOutcome> PlaybackSeek::End(SeekEndReason reason, std::int64_t nowMs) noexcept
{
    if (!mIsActive)
        return std::nullopt;
    SeekOutcome outcome;
    outcome.mReason = reason;
    outcome.mStartFrame = mStartFrame;
    outcome.mTargetFrame = mTargetFrame;
    outcome.mLastCompletedFrame = mLastCompletedFrame;
    if (reason == SeekEndReason::Reached || reason == SeekEndReason::Cancelled)
        outcome.mPausedToRestore = mWasPaused;
    outcome.mElapsedMs = nowMs > mBeginMs ? nowMs - mBeginMs : 0;
    *this = PlaybackSeek();
    return outcome;
}

viewer::PlaybackSeekView PlaybackSeek::GetView() const noexcept
{
    viewer::PlaybackSeekView view;
    if (!mIsActive)
        return view;
    view.mIsSeeking = true;
    view.mStartFrame = mStartFrame;
    view.mTargetFrame = mTargetFrame;
    return view;
}
}
