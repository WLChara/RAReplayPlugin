#include "OperationEventBuilder.h"

#include "../src/stats/MatchStatistics.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::stats;
using namespace rareplay::tests;
using rareplay::viewer::ProductionLineState;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

HouseObservation MakeHouse(int index, bool isHuman, std::int64_t credits)
{
    HouseObservation house;
    house.mHouseIndex = index;
    house.mIsHuman = isHuman;
    house.mCredits = credits;
    house.mAllyMask = 1u << index;
    return house;
}

FrameObservation MakeFrame(int speed, std::initializer_list<HouseObservation> houses)
{
    FrameObservation frame;
    frame.mTiming.mGameSpeed = speed;
    frame.mHouses = houses;
    return frame;
}

rareplay::viewer::ReplayViewSnapshot MakeSnapshot(std::initializer_list<int> houseIndices)
{
    rareplay::viewer::ReplayViewSnapshot snapshot;
    for (const int index : houseIndices)
    {
        rareplay::viewer::PlayerSummary player;
        player.mHouseIndex = index;
        snapshot.mPlayers.push_back(player);
    }
    return snapshot;
}

const rareplay::viewer::PlayerSummary& Player(const rareplay::viewer::ReplayViewSnapshot& snapshot, int houseIndex)
{
    for (const auto& player : snapshot.mPlayers)
    {
        if (player.mHouseIndex == houseIndex)
            return player;
    }
    throw std::runtime_error("missing player");
}

// 速度 2 每帧 32 ms：100 帧后 3200 ms，整秒边界采样发生在 32、1024、2016、3008 ms。
void CheckClockAndEconomySampling()
{
    MatchStatistics stats;
    auto snapshot = MakeSnapshot({0, 1});
    stats.ApplyTo(snapshot);
    Require(!snapshot.mGameTime && !snapshot.mPlayers[0].mStatistics && !snapshot.mPlayers[0].mEconomy,
            "nothing is published before the first frame");
    Require(!stats.IsProductionSampleDue(), "no production sample before the first frame");

    for (std::uint32_t frame = 0; frame < 100; ++frame)
    {
        const auto observation = MakeFrame(2, {MakeHouse(0, true, 1000 + frame), MakeHouse(1, false, 5000)});
        stats.OnFrameCompleted(frame, observation);
        stats.OnFrameCompleted(frame, observation);   // 暂停时反复进入同一帧
    }
    stats.ApplyTo(snapshot);
    Require(snapshot.mGameTime && snapshot.mGameTime->mMilliseconds == 3200 && !snapshot.mGameTime->mIsNominal,
            "game time ignores repeated frames");
    const auto& curve = Player(snapshot, 0).mEconomy;
    Require(curve && curve->mPoints.size() == 4 && curve->mPoints[0].mTimeMs == 32 && curve->mPoints[1].mTimeMs == 1024 &&
                curve->mPoints[2].mTimeMs == 2016 && curve->mPoints[3].mTimeMs == 3008 &&
                curve->mPoints[3].mCredits == 1000 + 93, "economy is sampled once per game second");
    Require(snapshot.mPlayers[0].mEconomy == Player(snapshot, 0).mEconomy, "same player");
    auto again = MakeSnapshot({0, 1});
    stats.ApplyTo(again);
    Require(again.mPlayers[0].mEconomy == snapshot.mPlayers[0].mEconomy,
            "an unchanged curve is shared between snapshots");
}

void CheckNominalTime()
{
    MatchStatistics stats;
    stats.OnFrameCompleted(0, MakeFrame(0, {MakeHouse(0, true, 0)}));
    auto snapshot = MakeSnapshot({0});
    stats.ApplyTo(snapshot);
    Require(snapshot.mGameTime && snapshot.mGameTime->mMilliseconds == 16 && snapshot.mGameTime->mIsNominal,
            "speed 0 is nominal 16 ms");
}

// 兵营与重工升级：首次由 0 变 1 的游戏时间；之后保持，标志本身仍如实反映。
void CheckUpgradeTiming()
{
    MatchStatistics stats;
    for (std::uint32_t frame = 0; frame < 20; ++frame)
    {
        HouseObservation house = MakeHouse(0, true, 0);
        house.mBarracksInfiltrated = frame >= 5;
        house.mWarFactoryInfiltrated = frame >= 12;
        stats.OnFrameCompleted(frame, MakeFrame(1, {house}));
    }
    auto snapshot = MakeSnapshot({0});
    stats.ApplyTo(snapshot);
    const auto& statistics = *snapshot.mPlayers[0].mStatistics;
    Require(statistics.mBarracksUpgrade.mIsActive && statistics.mBarracksUpgrade.mActivatedAtMs == 6 * 16,
            "barracks upgrade time is the first frame the flag is seen");
    Require(statistics.mWarFactoryUpgrade.mIsActive && statistics.mWarFactoryUpgrade.mActivatedAtMs == 13 * 16,
            "war factory upgrade time");

    // 标志被清掉（理论上不会发生）时如实显示未生效；再次置位不改写首次时间。
    HouseObservation cleared = MakeHouse(0, true, 0);
    stats.OnFrameCompleted(20, MakeFrame(1, {cleared}));
    cleared.mBarracksInfiltrated = true;
    stats.OnFrameCompleted(21, MakeFrame(1, {cleared}));
    auto later = MakeSnapshot({0});
    stats.ApplyTo(later);
    Require(later.mPlayers[0].mStatistics->mBarracksUpgrade.mIsActive &&
                later.mPlayers[0].mStatistics->mBarracksUpgrade.mActivatedAtMs == 6 * 16 &&
                !later.mPlayers[0].mStatistics->mWarFactoryUpgrade.mIsActive,
            "first activation time is kept");

    MatchStatistics fresh;
    HouseObservation preset = MakeHouse(0, true, 0);
    preset.mBarracksInfiltrated = true;
    fresh.OnFrameCompleted(0, MakeFrame(2, {preset}));
    auto presetSnapshot = MakeSnapshot({0});
    fresh.ApplyTo(presetSnapshot);
    Require(presetSnapshot.mPlayers[0].mStatistics->mBarracksUpgrade.mActivatedAtMs == 32,
            "a flag already set at the first observation takes that time");
}

void CheckOperationCountingAndApm()
{
    MatchStatistics stats;
    const auto mega = [](int house) { return MakeMegaMission(house, 1, 2, 7); };
    for (std::uint32_t frame = 0; frame < 500; ++frame)   // 速度 1：每帧 16 ms，共 8000 ms
    {
        stats.OnFrameCompleted(frame, MakeFrame(1, {MakeHouse(0, true, 0), MakeHouse(1, false, 0)}));
        stats.OnOperationEvent(frame, mega(0));
        stats.OnOperationEvent(frame, MakeSimpleEvent(NativeEventType::Timing, 0));   // 系统事件不计
        stats.OnOperationEvent(frame, mega(1));     // AI 的命令事件不会出现在显示里
    }
    auto early = MakeSnapshot({0, 1});
    stats.ApplyTo(early);
    Require(early.mPlayers[0].mStatistics->mOperationCount == 500u && !early.mPlayers[0].mStatistics->mApm,
            "APM is withheld before ten seconds of game time");
    for (std::uint32_t frame = 500; frame < 3750; ++frame)   // 共 60 000 ms
    {
        stats.OnFrameCompleted(frame, MakeFrame(1, {MakeHouse(0, true, 0), MakeHouse(1, false, 0)}));
        if (frame % 5 == 0)
            stats.OnOperationEvent(frame, mega(0));
    }
    auto snapshot = MakeSnapshot({0, 1});
    stats.ApplyTo(snapshot);
    const auto& human = *snapshot.mPlayers[0].mStatistics;
    Require(human.mOperationCount == 500u + 650u && human.mOperationEventCount == 500u + 650u && human.mApm &&
                std::abs(*human.mApm - 1150.0f) < 0.01f, "operations divided by game minutes");
    const auto& ai = *snapshot.mPlayers[1].mStatistics;
    Require(!ai.mOperationCount && !ai.mApm, "AI players show no operations");
}

// 群体命令按“操作”计：同帧同玩家同参数的多单位命令合并为一次，原始事件条数另行给出；APM 用合并后的操作数。
void CheckGroupCommandsMergeIntoOperations()
{
    MatchStatistics stats;
    for (std::uint32_t frame = 0; frame < 1000; ++frame)   // 速度 1：16 ms 一帧，共 16 秒
    {
        stats.OnFrameCompleted(frame, MakeFrame(1, {MakeHouse(0, true, 0), MakeHouse(1, true, 0)}));
        if (frame % 100 == 0)
        {
            // 玩家 0 每 100 帧对 10 个单位下一次同目标命令，并在同一帧再对 4 个单位下另一个目标的命令；
            // 玩家 1 在同一帧对 3 个单位下与玩家 0 相同的命令。
            for (std::uint32_t whom = 1; whom <= 10; ++whom)
                stats.OnOperationEvent(frame, MakeMegaMission(0, whom, 2, 700));
            for (std::uint32_t whom = 11; whom <= 14; ++whom)
                stats.OnOperationEvent(frame, MakeMegaMission(0, whom, 2, 701));
            for (std::uint32_t whom = 1; whom <= 3; ++whom)
                stats.OnOperationEvent(frame, MakeMegaMission(1, whom, 2, 700));
        }
    }
    auto snapshot = MakeSnapshot({0, 1});
    stats.ApplyTo(snapshot);
    const auto& first = *snapshot.mPlayers[0].mStatistics;
    const auto& second = *snapshot.mPlayers[1].mStatistics;
    Require(first.mOperationCount == 20u && first.mOperationEventCount == 140u,
            "10 group commands of two targets are 20 operations from 140 raw events");
    Require(second.mOperationCount == 10u && second.mOperationEventCount == 30u, "the other player is counted separately");
    Require(first.mApm && std::abs(*first.mApm - 20.0f * 60.0f / 16.0f) < 0.01f, "APM uses merged operations");
}

// 产线采样每累计 250 ms 游戏时间一次；停产起止写入历史，断电期间的停产带标注。
void CheckProductionSampling()
{
    MatchStatistics stats;
    int sampleCount = 0;
    const auto runFrames = [&](std::uint32_t from, std::uint32_t to, const LineObservation& barracks,
                               int powerOutput) {
        for (std::uint32_t frame = from; frame < to; ++frame)
        {
            HouseObservation house = MakeHouse(0, true, 0);
            house.mPowerOutput = powerOutput;
            house.mPowerDrain = 100;
            stats.OnFrameCompleted(frame, MakeFrame(2, {house}));
            if (stats.IsProductionSampleDue())
            {
                ProductionObservation observation;
                HouseProductionObservation houseObservation;
                houseObservation.mHouseIndex = 0;
                houseObservation.mBarracks = barracks;
                observation.mHouses.push_back(houseObservation);
                stats.OnProductionSampled(observation);
                ++sampleCount;
                Require(!stats.IsProductionSampleDue(), "a sample resets the due flag");
            }
        }
    };
    LineObservation producing;
    producing.mFactoryBuildings = 1;
    producing.mFactories.push_back({10, false, false, false});
    LineObservation stalled;
    stalled.mFactoryBuildings = 1;
    runFrames(0, 63, producing, 200);          // 约 2 秒：每 8 帧（256 ms）采样一次
    Require(sampleCount >= 7 && sampleCount <= 9, "about four samples per game second");
    runFrames(63, 250, stalled, 50);           // 持续约 6 秒的停产，期间电力不足
    runFrames(250, 300, producing, 200);
    auto snapshot = MakeSnapshot({0});
    stats.ApplyTo(snapshot);
    const auto& line = snapshot.mPlayers[0].mBarracksLine;
    Require(line && line->mState == ProductionLineState::Producing && line->mHistory && line->mHistory->size() == 1,
            "stall interval is recorded");
    const auto& record = (*line->mHistory)[0];
    Require(record.mHadPowerOutage && record.mEndMs - record.mStartMs >= 5500 && record.mEndMs - record.mStartMs <= 6500,
            "interval length and outage annotation");
    Require(snapshot.mPlayers[0].mWarFactoryLine && snapshot.mPlayers[0].mWarFactoryLine->mState ==
                ProductionLineState::NoFactory, "an untouched line has no factory");

    // 停产汇总经快照传出：总停产时间等于历史区间之和，停产率按快照里的游戏时间计算；无工厂的产线为 0。
    Require(snapshot.mGameTime && line->mTotalStalledMs == record.mEndMs - record.mStartMs &&
                line->mStallRatePercent &&
                std::abs(*line->mStallRatePercent - static_cast<float>(line->mTotalStalledMs) * 100.0f /
                                                        static_cast<float>(snapshot.mGameTime->mMilliseconds)) < 0.01f,
            "stall total and rate are published with the snapshot");
    Require(snapshot.mPlayers[0].mWarFactoryLine->mTotalStalledMs == 0 &&
                snapshot.mPlayers[0].mWarFactoryLine->mStallRatePercent &&
                *snapshot.mPlayers[0].mWarFactoryLine->mStallRatePercent == 0.0f,
            "a line without a factory has no stall time");
}

// 进行中的停产在发布时按发布时刻的游戏时间计入（满 2 秒后）。
void CheckOngoingStallSummary()
{
    MatchStatistics stats;
    LineObservation stalled;
    stalled.mFactoryBuildings = 1;
    const auto publish = [&]() {
        auto snapshot = MakeSnapshot({0});
        stats.ApplyTo(snapshot);
        return snapshot;
    };
    // 速度 2：每帧 32 ms。先正常生产几帧，再停产。
    LineObservation producing;
    producing.mFactoryBuildings = 1;
    producing.mFactories.push_back({10, false, false, false});
    std::uint32_t frame = 0;
    const auto runUntil = [&](std::int64_t timeMs, const LineObservation& barracks) {
        while (stats.GetClock().GetMilliseconds() < timeMs)
        {
            stats.OnFrameCompleted(frame, MakeFrame(2, {MakeHouse(0, true, 0)}));
            if (stats.IsProductionSampleDue())
            {
                ProductionObservation observation;
                HouseProductionObservation house;
                house.mHouseIndex = 0;
                house.mBarracks = barracks;
                observation.mHouses.push_back(house);
                stats.OnProductionSampled(observation);
            }
            ++frame;
        }
    };
    runUntil(1000, producing);
    runUntil(2000, stalled);
    auto snapshot = publish();
    Require(snapshot.mPlayers[0].mBarracksLine->mTotalStalledMs == 0, "an unconfirmed stall is not totalled");
    runUntil(5000, stalled);
    snapshot = publish();
    const auto& line = *snapshot.mPlayers[0].mBarracksLine;
    Require(line.mState == ProductionLineState::Stalled && line.mTotalStalledMs ==
                snapshot.mGameTime->mMilliseconds - line.mStalledSinceMs && line.mTotalStalledMs >= 2000,
            "an ongoing confirmed stall is totalled up to the publishing time");
    Require(line.mStallRatePercent && *line.mStallRatePercent > 50.0f && *line.mStallRatePercent < 100.0f,
            "the rate of an ongoing stall");
}

void CheckApplyToMatchesPlayersAndHandlesMissing()
{
    MatchStatistics stats;
    HouseObservation a = MakeHouse(0, true, 100);
    HouseObservation b = MakeHouse(1, false, 200);
    a.mAllyMask = 0b01;
    b.mAllyMask = 0b10;
    a.mCounters.mKilledUnitsOfHouses[1] = 4;
    b.mCounters.mKilledBuildingsOfHouses[0] = 2;
    a.mCounters.mPointTotal = -5;
    b.mCounters.mPointTotal = 99;
    stats.OnFrameCompleted(0, MakeFrame(1, {a, b}));
    auto snapshot = MakeSnapshot({1, 0, 7});   // 顺序不同，且 7 号玩家没有观测
    stats.ApplyTo(snapshot);
    Require(Player(snapshot, 0).mStatistics->mScore == 0 && Player(snapshot, 1).mStatistics->mScore == 99,
            "players are matched by house index");
    Require(Player(snapshot, 0).mStatistics->mCombat->mDestroyedUnits == 4 &&
                Player(snapshot, 0).mStatistics->mCombat->mLostBuildings == 2, "combat statistics are applied");
    Require(!Player(snapshot, 7).mStatistics && !Player(snapshot, 7).mEconomy && !Player(snapshot, 7).mBarracksLine,
            "a player without observations stays missing instead of zero");
    Require(stats.GetSkippedHouseCount() == 0, "no skipped houses");

    HouseObservation outOfRange = MakeHouse(30, false, 0);
    MatchStatistics skipping;
    skipping.OnFrameCompleted(0, MakeFrame(1, {a, outOfRange}));
    auto second = MakeSnapshot({0, 30});
    skipping.ApplyTo(second);
    Require(skipping.GetSkippedHouseCount() == 1 && !Player(second, 30).mStatistics->mCombat &&
                Player(second, 0).mStatistics->mCombat, "out-of-range houses are skipped and counted");
}

void CheckReset()
{
    MatchStatistics stats;
    stats.OnFrameCompleted(0, MakeFrame(1, {MakeHouse(0, true, 5)}));
    stats.OnOperationEvent(0, MakeProduce(NativeEventType::Produce, 0, 40, 1, 0));
    stats.Reset();
    auto snapshot = MakeSnapshot({0});
    stats.ApplyTo(snapshot);
    Require(!snapshot.mGameTime && !snapshot.mPlayers[0].mStatistics && !stats.GetClock().HasStarted(),
            "reset clears the session");
    stats.OnFrameCompleted(0, MakeFrame(1, {MakeHouse(0, true, 5)}));
    stats.ApplyTo(snapshot);
    Require(snapshot.mPlayers[0].mStatistics->mOperationCount == 0u, "operation counts do not survive a reset");
}
}

int main()
{
    try
    {
        CheckClockAndEconomySampling();
        CheckNominalTime();
        CheckUpgradeTiming();
        CheckOperationCountingAndApm();
        CheckGroupCommandsMergeIntoOperations();
        CheckProductionSampling();
        CheckOngoingStallSummary();
        CheckApplyToMatchesPlayersAndHandlesMissing();
        CheckReset();
        std::cout << "Match statistics tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
