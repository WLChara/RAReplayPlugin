#include "PlaybackSeekProfile.h"

#include <algorithm>
#include <cstdio>

namespace rareplay::plugin
{
namespace
{
double ToMilliseconds(SeekPhaseClock::Clock::duration duration) noexcept
{
    return std::chrono::duration<double, std::milli>(duration).count();
}
}

game_api::native::SeekBacklogCleanup PlanSeekBacklogCleanup(SeekCleanupPoint point,
    const game_api::native::SeekBacklogCounts& counts) noexcept
{
    game_api::native::SeekBacklogCleanup cleanup;
    cleanup.mClearDirtyAreas = true;
    if (point == SeekCleanupPoint::FrameCompleted)
    {
        cleanup.mDeleteEBolts = counts.mEBolts > SEEK_EBOLT_LIMIT;
        cleanup.mDeleteRadBeams = counts.mRadBeams > SEEK_RADBEAM_LIMIT;
        cleanup.mDeleteDetachedLineTrails = counts.mLineTrails > SEEK_LINE_TRAIL_LIMIT;
        return cleanup;
    }
    cleanup.mDeleteEBolts = true;
    cleanup.mDeleteRadBeams = true;
    cleanup.mDeleteDetachedLineTrails = true;
    if (point == SeekCleanupPoint::SeekEnd)
    {
        cleanup.mHideAttachedLineTrails = true;
        cleanup.mClearRadarEvents = true;
        cleanup.mClearScreenShake = true;
    }
    return cleanup;
}

void SeekPhaseClock::Accumulate(Clock::time_point now) noexcept
{
    if (!mCurrent)
        return;
    if (now > mSince)
        mTotals[static_cast<std::size_t>(*mCurrent)] += now - mSince;
    mSince = now;
}

void SeekPhaseClock::Enter(SeekPhase phase, Clock::time_point now) noexcept
{
    if (phase == SeekPhase::Count)
        return;
    if (mCurrent)
        Accumulate(now);
    else
        mSince = now;
    mCurrent = phase;
}

SeekPhaseClock::Token SeekPhaseClock::Push(SeekPhase phase, Clock::time_point now) noexcept
{
    if (!mCurrent || phase == SeekPhase::Count)
        return std::nullopt;
    const Token previous = mCurrent;
    Accumulate(now);
    mCurrent = phase;
    return previous;
}

void SeekPhaseClock::Pop(Token token, Clock::time_point now) noexcept
{
    if (!token || !mCurrent)
        return;
    Accumulate(now);
    mCurrent = token;
}

void SeekPhaseClock::Stop(Clock::time_point now) noexcept
{
    Accumulate(now);
    mCurrent.reset();
}

void SeekPhaseClock::Reset() noexcept
{
    *this = SeekPhaseClock();
}

SeekPhaseClock::Clock::duration SeekPhaseClock::GetTotal(SeekPhase phase) const noexcept
{
    const auto index = static_cast<std::size_t>(phase);
    return index < mTotals.size() ? mTotals[index] : Clock::duration::zero();
}

void SeekProfile::Reset() noexcept
{
    *this = SeekProfile();
}

void SeekProfile::ObserveBacklog(const game_api::native::SeekBacklogCounts& counts) noexcept
{
    mPeaks.mDirtyAreas = std::max(mPeaks.mDirtyAreas, counts.mDirtyAreas);
    mPeaks.mEBolts = std::max(mPeaks.mEBolts, counts.mEBolts);
    mPeaks.mRadBeams = std::max(mPeaks.mRadBeams, counts.mRadBeams);
    mPeaks.mLineTrails = std::max(mPeaks.mLineTrails, counts.mLineTrails);
    mPeaks.mRadarEvents = std::max(mPeaks.mRadarEvents, counts.mRadarEvents);
}

void SeekProfile::RecordCleanup(const game_api::native::SeekBacklogCleanupResult& result) noexcept
{
    if (result.mEBoltsDeleted > 0)
        ++mEBoltClears;
    if (result.mRadBeamsDeleted > 0)
        ++mRadBeamClears;
    if (result.mLineTrailsDeleted > 0)
        ++mLineTrailClears;
}

std::string SeekProfile::FormatTimingLog(std::uint32_t frames) const
{
    char line[512]{};
    std::snprintf(line, sizeof(line),
        "Replay seek timing: frames=%u inputMs=%.1f simMs=%.1f eventsMs=%.1f tailMs=%.1f pluginMs=%.1f cleanupMs=%.1f "
        "otherMs=%.1f draws=%u dirtyPeak=%d eboltPeak=%d eboltClears=%u radBeamPeak=%d radBeamClears=%u "
        "lineTrailPeak=%d lineTrailClears=%u radarEventPeak=%d",
        frames, ToMilliseconds(mClock.GetTotal(SeekPhase::Input)), ToMilliseconds(mClock.GetTotal(SeekPhase::Simulation)),
        ToMilliseconds(mClock.GetTotal(SeekPhase::Events)), ToMilliseconds(mClock.GetTotal(SeekPhase::Tail)),
        ToMilliseconds(mClock.GetTotal(SeekPhase::Plugin)), ToMilliseconds(mClock.GetTotal(SeekPhase::Cleanup)),
        ToMilliseconds(mClock.GetTotal(SeekPhase::Other)), mFallbackDraws, mPeaks.mDirtyAreas, mPeaks.mEBolts,
        mEBoltClears, mPeaks.mRadBeams, mRadBeamClears, mPeaks.mLineTrails, mLineTrailClears, mPeaks.mRadarEvents);
    return line;
}
}
