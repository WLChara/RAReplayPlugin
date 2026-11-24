#include "ReplayRadarSession.h"

#include "Diagnostics.h"

#include "../game_api/radar/RadarObservation.h"
#include "../viewer/RadarDisplayRules.h"
#include "../viewer/ReplayViewExchange.h"

#include <cstdio>
#include <utility>

namespace rareplay::plugin
{
namespace
{
// 单帧内待处理观察的上限：一帧里同时发射或渗透几十次已是异常，超出的丢弃，防止异常数据撑大内存。
constexpr std::size_t MAX_PENDING_EVENTS = 64;

bool IsSameMarkFrame(const viewer::SuperWeaponMarkFrame& left, const viewer::SuperWeaponMarkFrame& right) noexcept
{
    if (left.mSessionId != right.mSessionId || left.mGameTimeMs != right.mGameTimeMs ||
        left.mCanvasWidth != right.mCanvasWidth || left.mCanvasHeight != right.mCanvasHeight ||
        left.mView.mX != right.mView.mX || left.mView.mY != right.mView.mY ||
        left.mView.mWidth != right.mView.mWidth || left.mView.mHeight != right.mView.mHeight ||
        left.mMarks.size() != right.mMarks.size())
        return false;
    for (std::size_t index = 0; index < left.mMarks.size(); ++index)
    {
        if (left.mMarks[index].mInfo != right.mMarks[index].mInfo ||
            left.mMarks[index].mPosition.mX != right.mMarks[index].mPosition.mX ||
            left.mMarks[index].mPosition.mY != right.mMarks[index].mPosition.mY)
            return false;
    }
    return true;
}
}

void ReplayRadarSession::Begin()
{
    mTracker = {};
    mPending.clear();
    mResolver.Reset();
    mLog.Reset();
    mMarks.Reset();
    mPublishedMarks.reset();
    mLaunchCount = 0;
    mInfiltrationCount = 0;
    mFilteredCount = 0;
    mDroppedPendingCount = 0;
    mIsActive = true;
}

void ReplayRadarSession::End() noexcept
{
    if (!mIsActive)
        return;
    mIsActive = false;
    WithdrawMarks();
    char message[256]{};
    const auto& icons = mResolver.GetIconStats();
    std::snprintf(message, sizeof(message),
        "Replay radar events: launches=%u infiltrations=%u hiddenSuperWeapons=%u unmatchedObservations=%u "
        "droppedPending=%u icons(PCX=%u SHP=%u missing=%u)",
        mLaunchCount, mInfiltrationCount, mFilteredCount, mTracker.GetDiscardedCount(), mDroppedPendingCount,
        icons.mPcx, icons.mShp, icons.mMissing);
    Log(message);
    mPending.clear();
    mResolver.Reset();
}

void ReplayRadarSession::Disable(const char* reason) noexcept
{
    if (!mIsActive)
        return;
    mIsActive = false;
    WithdrawMarks();
    mPending.clear();
    Log(reason);
}

void ReplayRadarSession::WithdrawMarks() noexcept
{
    if (mPublishedMarks)
    {
        viewer::GetReplayViewExchange().PublishMarks(nullptr);
        mPublishedMarks.reset();
    }
}

void ReplayRadarSession::OnSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
    const void* targetCell) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        stats::SuperWeaponFireBegin begin;
        if (game_api::ReadSuperWeaponFireBegin(house, super, prerequisiteSuper, targetCell, begin))
            mTracker.Begin(begin);
    }
    catch (...)
    {
        Disable("Replay radar events disabled after a super weapon observation exception; playback continues");
    }
}

void ReplayRadarSession::OnSuperWeaponFireEnd(const void* super) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        stats::SuperWeaponFireEnd end;
        if (!game_api::ReadSuperWeaponFireEnd(super, end))
            return;
        if (const auto launch = mTracker.End(end))
        {
            if (mPending.size() < MAX_PENDING_EVENTS)
            {
                PendingEvent pending;
                pending.mIsLaunch = true;
                pending.mLaunch = *launch;
                mPending.push_back(pending);
            }
            else
                ++mDroppedPendingCount;
        }
    }
    catch (...)
    {
        Disable("Replay radar events disabled after a super weapon observation exception; playback continues");
    }
}

void ReplayRadarSession::OnBuildingInfiltrated(const void* spy, const void* building) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        PendingEvent pending;
        pending.mIsLaunch = false;
        if (!game_api::ReadBuildingInfiltration(spy, building, pending.mInfiltration))
            return;
        if (mPending.size() < MAX_PENDING_EVENTS)
            mPending.push_back(pending);
        else
            ++mDroppedPendingCount;
    }
    catch (...)
    {
        Disable("Replay radar events disabled after an infiltration observation exception; playback continues");
    }
}

void ReplayRadarSession::OnFrameCompleted(std::optional<std::int64_t> gameTimeMs) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        // Fire_SW 是同步调用，帧完成时栈必须为空；残留项视为丢失。
        mTracker.Clear();
        if (mPending.empty())
            return;
        if (!gameTimeMs)
        {
            mPending.clear();
            return;
        }
        mResolver.BeginSample();
        for (const PendingEvent& pending : mPending)
        {
            auto draft = pending.mIsLaunch ? mResolver.ResolveLaunch(pending.mLaunch)
                                           : mResolver.ResolveInfiltration(pending.mInfiltration);
            if (!draft)
            {
                if (pending.mIsLaunch)
                    ++mFilteredCount;
                continue;
            }
            const bool hasLocation = draft->mLocation.has_value();
            const std::uint64_t id = mLog.Add(*draft, *gameTimeMs);
            if (!pending.mIsLaunch)
            {
                ++mInfiltrationCount;
                continue;
            }
            ++mLaunchCount;
            if (hasLocation)
            {
                auto info = std::make_shared<viewer::SuperWeaponMarkInfo>();
                info->mId = id;
                info->mReleasedAtMs = *gameTimeMs;
                info->mOwnerColorRgb = draft->mActor.mColorRgb;
                info->mLabel = draft->mSubjectName;
                info->mIcon = draft->mIcon;
                mMarks.Add(std::move(info), *draft->mLocation);
            }
        }
        mPending.clear();
    }
    catch (...)
    {
        Disable("Replay radar events disabled after a sampling exception; playback continues");
    }
}

void ReplayRadarSession::PublishMarks(std::uint64_t sessionId, std::optional<std::int64_t> gameTimeMs) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        if (gameTimeMs)
            mMarks.Prune(*gameTimeMs);
        const auto geometry = game_api::ReadTacticalViewGeometry();
        if (!gameTimeMs || mMarks.GetActive().empty() || !geometry)
        {
            WithdrawMarks();
            return;
        }
        auto frame = std::make_shared<viewer::SuperWeaponMarkFrame>();
        frame->mSessionId = sessionId;
        frame->mGameTimeMs = *gameTimeMs;
        frame->mCanvasWidth = geometry->mCanvasWidth;
        frame->mCanvasHeight = geometry->mCanvasHeight;
        frame->mView = geometry->mView;
        frame->mMarks.reserve(mMarks.GetActive().size());
        for (const stats::ActiveSuperWeaponMark& mark : mMarks.GetActive())
        {
            if (const auto position = game_api::ProjectWorldToCanvas(mark.mWorld, *geometry))
                frame->mMarks.push_back({mark.mInfo, *position});
        }
        if (mPublishedMarks && IsSameMarkFrame(*mPublishedMarks, *frame))
            return;
        mPublishedMarks = frame;
        viewer::GetReplayViewExchange().PublishMarks(std::move(frame));
    }
    catch (...)
    {
        Disable("Replay radar events disabled after a mark projection exception; playback continues");
    }
}

void ReplayRadarSession::ApplyTo(viewer::ReplayViewSnapshot& snapshot) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        snapshot.mRadarEvents = mLog.GetSnapshot();
    }
    catch (...)
    {
        Disable("Replay radar events disabled after a publish exception; playback continues");
    }
}

bool ReplayRadarSession::CenterOnEvent(std::uint64_t eventId) noexcept
{
    if (!mIsActive)
        return false;
    try
    {
        const viewer::RadarEvent* event = mLog.Find(eventId);
        return event && event->mLocation && game_api::CenterTacticalViewOn(*event->mLocation);
    }
    catch (...)
    {
        return false;
    }
}
}
