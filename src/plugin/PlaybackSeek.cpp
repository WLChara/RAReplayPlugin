#include "PlaybackSeek.h"

namespace rareplay::plugin
{
namespace
{
// 心跳为空，或距今已超过 SEEK_HUD_STALE_MS；心跳晚于 now（另一线程刚写入）时算新鲜。
bool IsHudStale(std::int64_t nowMs, std::optional<std::int64_t> hudPresentedAtMs) noexcept
{
    return !hudPresentedAtMs || nowMs - *hudPresentedAtMs > SEEK_HUD_STALE_MS;
}
}

bool PlaybackSeek::Begin(std::uint32_t lastCompletedFrame, std::uint32_t targetFrame, bool wasPaused,
    std::int64_t nowMs, std::optional<std::int64_t> hudPresentedAtMs) noexcept
{
    if (mIsActive || targetFrame <= lastCompletedFrame)
        return false;
    mIsActive = true;
    mStartFrame = lastCompletedFrame;
    mTargetFrame = targetFrame;
    mLastCompletedFrame = lastCompletedFrame;
    mWasPaused = wasPaused;
    mBeginMs = nowMs;
    mLastFallbackDrawMs.reset();
    if (!IsHudStale(nowMs, hudPresentedAtMs))
        mLastFallbackDrawMs = nowMs;
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

bool PlaybackSeek::ShouldDrawFallback(std::int64_t nowMs, std::optional<std::int64_t> hudPresentedAtMs) const noexcept
{
    if (!mIsActive || !IsHudStale(nowMs, hudPresentedAtMs))
        return false;
    return !mLastFallbackDrawMs || nowMs - *mLastFallbackDrawMs >= SEEK_FALLBACK_DRAW_INTERVAL_MS;
}

void PlaybackSeek::MarkFallbackDrawn(std::int64_t nowMs) noexcept
{
    if (mIsActive)
        mLastFallbackDrawMs = nowMs;
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

bool SeekInputThrottle::ShouldSkip(bool isSeeking, std::int64_t nowMs) noexcept
{
    if (!isSeeking)
    {
        mLastFullRunMs.reset();
        return false;
    }
    if (mLastFullRunMs && nowMs >= *mLastFullRunMs && nowMs - *mLastFullRunMs < SEEK_INPUT_TICK_MS)
        return true;
    mLastFullRunMs = nowMs;
    return false;
}
}
