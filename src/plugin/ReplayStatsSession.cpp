#include "ReplayStatsSession.h"

#include "Diagnostics.h"
#include "StatsDigest.h"

#include "../game_api/stats/StatsReader.h"
#include "../stats/OperationEvents.h"

#include <string>

namespace rareplay::plugin
{
void ReplayStatsSession::Begin()
{
    mStats.Reset();
    mFrame.mHouses.clear();
    mProduction.mHouses.clear();
    mHasLoggedSkippedHouses = false;
    mIsActive = true;
}

void ReplayStatsSession::End() noexcept
{
    mIsActive = false;
}

void ReplayStatsSession::Disable(const char* reason) noexcept
{
    if (!mIsActive)
        return;
    mIsActive = false;
    Log(reason);
}

void ReplayStatsSession::OnFrameCompleted(std::uint32_t frameNumber) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        game_api::ReadFrameObservation(mFrame);
        const std::int64_t previousMs = mStats.GetClock().GetMilliseconds();
        mStats.OnFrameCompleted(frameNumber, mFrame);
        // 诊断摘要只读统计层刚登记的观测，不改变统计行为与采样频率；写摘要出错只丢这一行，不停用统计。
        const std::int64_t currentMs = mStats.GetClock().GetMilliseconds();
        if (HasCrossedStatsDigestBoundary(previousMs, currentMs))
        {
            try { Log(FormatStatsDigest(frameNumber, currentMs, mStats.GetLatestHouses()).c_str()); }
            catch (...) { Log("Replay stats digest skipped after an exception"); }
        }
        // 产线读取需要遍历各房屋的建筑，只在累计足够游戏时间后才做。
        if (mStats.IsProductionSampleDue())
        {
            game_api::ReadProductionObservation(mProduction);
            mStats.OnProductionSampled(mProduction);
        }
    }
    catch (...)
    {
        Disable("Replay statistics disabled after a sampling exception; playback continues");
    }
}

void ReplayStatsSession::OnPlaybackFrameExecuted(const replay::ReplayFrame& frame) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        // 同一帧的事件连续登记，统计层据此把同帧的群体命令合并为一次操作。
        for (const replay::RecordedEvent& event : frame.mEvents)
            mStats.OnOperationEvent(frame.mFrameNumber, event);
    }
    catch (...)
    {
        Disable("Replay statistics disabled after an operation counting exception; playback continues");
    }
}

std::optional<std::int64_t> ReplayStatsSession::GetGameTimeMs() const noexcept
{
    if (!mIsActive || !mStats.GetClock().HasStarted())
        return std::nullopt;
    return mStats.GetClock().GetMilliseconds();
}

void ReplayStatsSession::ApplyTo(viewer::ReplayViewSnapshot& snapshot) noexcept
{
    if (!mIsActive)
        return;
    try
    {
        mStats.ApplyTo(snapshot);
        if (!mHasLoggedSkippedHouses && mStats.GetSkippedHouseCount() > 0)
        {
            // 击杀矩阵只有 20 项，下标越界的房屋没有战斗统计；只记录一次，供实机核对。
            mHasLoggedSkippedHouses = true;
            const auto message = "Replay statistics: " + std::to_string(mStats.GetSkippedHouseCount()) +
                " house(s) outside the 20-entry kill matrix are skipped";
            Log(message.c_str());
        }
    }
    catch (...)
    {
        Disable("Replay statistics disabled after a publish exception; playback continues");
    }
}
}
