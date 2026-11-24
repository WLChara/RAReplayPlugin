#include "MatchStatistics.h"

#include "PlayerStatistics.h"

#include <algorithm>

namespace rareplay::stats
{
void MatchStatistics::Reset()
{
    mClock.Reset();
    mOperations.Reset();
    mTracks.clear();
    mLatestHouses.clear();
    mNextEconomySampleMs.reset();
    mLastProductionSampleMs.reset();
    mSkippedHouses = 0;
}

MatchStatistics::PlayerTrack& MatchStatistics::GetTrack(int houseIndex)
{
    return mTracks[houseIndex];
}

void MatchStatistics::UpgradeTracker::Update(bool isFlagSet, std::int64_t nowMs) noexcept
{
    // 渗透标志除构造函数外没有写 0 的地方，所以正常情况下只会 0→1；仍以标志本身为准，
    // 生效时间只在首次观测到 0→1 时记录，之后保持不变。
    if (isFlagSet && !mHasActivated)
    {
        mFirstActivatedAtMs = nowMs;
        mHasActivated = true;
    }
    mIsActive = isFlagSet;
}

void MatchStatistics::OnFrameCompleted(std::uint32_t frameNumber, const FrameObservation& frame)
{
    if (!mClock.OnFrameCompleted(frameNumber, frame.mTiming))
        return;
    const std::int64_t nowMs = mClock.GetMilliseconds();
    const bool isEconomyDue = !mNextEconomySampleMs || nowMs >= *mNextEconomySampleMs;
    for (const HouseObservation& house : frame.mHouses)
    {
        PlayerTrack& track = GetTrack(house.mHouseIndex);
        track.mBarracksUpgrade.Update(house.mBarracksInfiltrated, nowMs);
        track.mWarFactoryUpgrade.Update(house.mWarFactoryInfiltrated, nowMs);
        track.mHasPowerOutage = HasPowerOutage(house.mPowerOutput, house.mPowerDrain);
        if (isEconomyDue)
            track.mEconomy.Add(nowMs, house.mCredits);
    }
    if (isEconomyDue)
    {
        // 对齐到整秒边界，使采样间隔平均为 1 秒而不随帧时长累积漂移。
        mNextEconomySampleMs = (nowMs / ECONOMY_SAMPLE_INTERVAL_MS + 1) * ECONOMY_SAMPLE_INTERVAL_MS;
    }
    mLatestHouses = frame.mHouses;
}

bool MatchStatistics::IsProductionSampleDue() const noexcept
{
    if (!mClock.HasStarted())
        return false;
    return !mLastProductionSampleMs ||
        mClock.GetMilliseconds() - *mLastProductionSampleMs >= PRODUCTION_SAMPLE_INTERVAL_MS;
}

void MatchStatistics::OnProductionSampled(const ProductionObservation& observation)
{
    if (!mClock.HasStarted())
        return;
    const std::int64_t nowMs = mClock.GetMilliseconds();
    for (const HouseProductionObservation& house : observation.mHouses)
    {
        PlayerTrack& track = GetTrack(house.mHouseIndex);
        track.mBarracks.Update(nowMs, house.mBarracks, track.mHasPowerOutage);
        track.mWarFactory.Update(nowMs, house.mWarFactory, track.mHasPowerOutage);
    }
    mLastProductionSampleMs = nowMs;
}

void MatchStatistics::OnOperationEvent(std::uint32_t frameNumber, std::span<const std::uint8_t> event)
{
    mOperations.Count(frameNumber, event);
}

void MatchStatistics::ApplyTo(viewer::ReplayViewSnapshot& snapshot)
{
    if (!mClock.HasStarted())
        return;
    const std::int64_t nowMs = mClock.GetMilliseconds();
    snapshot.mGameTime = viewer::GameTime{nowMs, mClock.IsNominal()};
    const std::vector<viewer::PlayerStatistics> results = ComputePlayerStatistics(mLatestHouses, mSkippedHouses);
    for (viewer::PlayerSummary& player : snapshot.mPlayers)
    {
        const auto observed = std::find_if(mLatestHouses.begin(), mLatestHouses.end(),
            [&player](const HouseObservation& house) { return house.mHouseIndex == player.mHouseIndex; });
        const auto track = mTracks.find(player.mHouseIndex);
        if (observed != mLatestHouses.end() && track != mTracks.end())
        {
            viewer::PlayerStatistics statistics = results[static_cast<std::size_t>(observed - mLatestHouses.begin())];
            if (observed->mIsHuman)
            {
                const std::uint32_t count = mOperations.GetOperationCount(player.mHouseIndex);
                statistics.mOperationCount = count;
                statistics.mOperationEventCount = mOperations.GetRawEventCount(player.mHouseIndex);
                statistics.mApm = ComputeApm(count, nowMs);
            }
            statistics.mBarracksUpgrade = track->second.mBarracksUpgrade.ToStatus();
            statistics.mWarFactoryUpgrade = track->second.mWarFactoryUpgrade.ToStatus();
            player.mStatistics = statistics;
        }
        if (track != mTracks.end())
        {
            player.mBarracksLine = track->second.mBarracks.GetStatus(nowMs);
            player.mWarFactoryLine = track->second.mWarFactory.GetStatus(nowMs);
            player.mEconomy = track->second.mEconomy.BuildCurve();
        }
    }
}
}
