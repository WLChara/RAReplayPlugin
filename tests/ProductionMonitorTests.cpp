#include "../src/stats/ProductionMonitor.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::stats;
using rareplay::viewer::ProductionLineState;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

FactoryObservation Factory(int progress, bool suspended = false, bool manual = false, bool onHold = false)
{
    FactoryObservation factory;
    factory.mProgress = progress;
    factory.mIsSuspended = suspended;
    factory.mIsManual = manual;
    factory.mIsOnHold = onHold;
    return factory;
}

LineObservation Line(std::uint32_t buildings, std::initializer_list<FactoryObservation> factories = {})
{
    LineObservation line;
    line.mFactoryBuildings = buildings;
    line.mFactories = factories;
    return line;
}

void CheckFactoryClassification()
{
    Require(ClassifyFactory(Factory(10)) == ProductionLineState::Producing, "normal progress is producing");
    Require(ClassifyFactory(Factory(FACTORY_PROGRESS_DONE, true, false)) == ProductionLineState::Producing,
            "completed waiting for exit is not a stall or pause");
    Require(ClassifyFactory(Factory(60, true, true)) == ProductionLineState::Producing, "progress at or above 54 is done");
    Require(ClassifyFactory(Factory(20, true, true)) == ProductionLineState::Paused, "manual suspension is paused");
    Require(ClassifyFactory(Factory(20, true, false)) == ProductionLineState::PowerLoss,
            "automatic suspension means no powered factory");
    Require(ClassifyFactory(Factory(20, false, false, true)) == ProductionLineState::NoFunds, "on hold is out of funds");
    Require(ClassifyFactory(Factory(20, true, true, true)) == ProductionLineState::Paused,
            "suspension wins over the hold flag");
}

void CheckLineClassification()
{
    Require(ClassifyLine(Line(0)) == ProductionLineState::NoFactory, "no factory building");
    Require(ClassifyLine(Line(0, {Factory(10)})) == ProductionLineState::NoFactory,
            "a product without a factory building is not a line");
    Require(ClassifyLine(Line(2)) == ProductionLineState::Stalled, "factory without product is stalled");
    Require(ClassifyLine(Line(1, {Factory(10)})) == ProductionLineState::Producing, "single producing factory");
    Require(ClassifyLine(Line(2, {Factory(20, true, false), Factory(20, true, true)})) == ProductionLineState::Paused,
            "paused beats power loss among several factories");
    Require(ClassifyLine(Line(3, {Factory(20, true, true), Factory(20, false, false, true), Factory(5)})) ==
                ProductionLineState::Producing, "any producing factory makes the line producing");
    Require(ClassifyLine(Line(2, {Factory(20, true, true), Factory(20, false, false, true)})) ==
                ProductionLineState::NoFunds, "out of funds beats paused");
}

void CheckPowerOutage()
{
    Require(!HasPowerOutage(100, 100) && !HasPowerOutage(0, 0) && !HasPowerOutage(500, 0), "no outage");
    Require(HasPowerOutage(99, 100) && HasPowerOutage(0, 1), "outage when output below positive drain");
}

// 停产必须持续不短于 2 秒才会确认并写入历史；确认前仍报告上一个确认状态。
void CheckStallThresholdAndHistory()
{
    ProductionLineTracker tracker;
    tracker.Update(0, Line(1, {Factory(10)}), false);
    Require(tracker.GetStatus(0).mState == ProductionLineState::Producing, "starts producing");
    tracker.Update(1000, Line(1), false);
    Require(tracker.GetStatus(0).mState == ProductionLineState::Producing, "a fresh stall is not confirmed yet");
    tracker.Update(2999, Line(1), false);
    Require(tracker.GetStatus(0).mState == ProductionLineState::Producing, "1999 ms of stall is below the threshold");
    tracker.Update(3000, Line(1), false);
    auto status = tracker.GetStatus(0);
    Require(status.mState == ProductionLineState::Stalled && status.mStalledSinceMs == 1000 &&
                !status.mStallHadPowerOutage && !status.mHistory,
            "stall is confirmed at exactly 2000 ms and keeps its true start time");
    tracker.Update(5000, Line(1, {Factory(1)}), false);
    status = tracker.GetStatus(0);
    Require(status.mState == ProductionLineState::Producing && status.mHistory && status.mHistory->size() == 1 &&
                (*status.mHistory)[0].mStartMs == 1000 && (*status.mHistory)[0].mEndMs == 5000 &&
                !(*status.mHistory)[0].mHadPowerOutage,
            "ending a confirmed stall records its interval");
}

void CheckShortStallIsNotRecorded()
{
    ProductionLineTracker tracker;
    tracker.Update(0, Line(1, {Factory(10)}), false);
    tracker.Update(500, Line(1), false);
    tracker.Update(1500, Line(1), false);
    tracker.Update(1999, Line(1, {Factory(1)}), false);
    const auto status = tracker.GetStatus(0);
    Require(status.mState == ProductionLineState::Producing && !status.mHistory,
            "a 1499 ms stall is neither shown as stalled nor recorded");
}

void CheckOutageAnnotation()
{
    ProductionLineTracker tracker;
    tracker.Update(0, Line(1, {Factory(10)}), true);   // 停产之前的断电不属于这段停产
    tracker.Update(1000, Line(1), false);
    tracker.Update(2000, Line(1), true);
    tracker.Update(3000, Line(1), false);
    Require(tracker.GetStatus(0).mStallHadPowerOutage, "outage during the stall is annotated on the current stall");
    tracker.Update(4000, Line(1, {Factory(1)}), false);
    Require((*tracker.GetStatus(0).mHistory)[0].mHadPowerOutage, "recorded interval carries the outage annotation");

    tracker.Update(5000, Line(1), false);
    tracker.Update(8000, Line(1), false);
    tracker.Update(9000, Line(1, {Factory(1)}), false);
    const auto history = *tracker.GetStatus(0).mHistory;
    Require(history.size() == 2 && !history[1].mHadPowerOutage, "a stall without outage is not annotated");
}

// 开局就没有生产也是真实的停产，第一次观测直接采用当前状态；无工厂不计停产且结束正在进行的停产。
void CheckFirstObservationAndNoFactory()
{
    ProductionLineTracker tracker;
    tracker.Update(250, Line(1), false);
    auto status = tracker.GetStatus(0);
    Require(status.mState == ProductionLineState::Stalled && status.mStalledSinceMs == 250,
            "stall at the first observation is reported immediately");
    tracker.Update(2500, Line(0), false);
    status = tracker.GetStatus(0);
    Require(status.mState == ProductionLineState::NoFactory && status.mHistory && status.mHistory->size() == 1 &&
                (*status.mHistory)[0].mEndMs == 2500, "losing the last factory ends the stall");

    ProductionLineTracker noFactory;
    noFactory.Update(0, Line(0), false);
    noFactory.Update(10000, Line(0), false);
    Require(noFactory.GetStatus(0).mState == ProductionLineState::NoFactory && !noFactory.GetStatus(0).mHistory,
            "no factory is never a stall");
}

// 已发布的历史是不可变共享对象：之后追加记录必须生成新对象，旧快照持有的对象不变。
void CheckHistoryIsImmutableOnceShared()
{
    ProductionLineTracker tracker;
    tracker.Update(0, Line(1, {Factory(1)}), false);
    tracker.Update(1000, Line(1), false);
    tracker.Update(4000, Line(1, {Factory(1)}), false);
    const auto first = tracker.GetStatus(0).mHistory;
    Require(first && first->size() == 1, "first record");
    Require(tracker.GetStatus(0).mHistory == first, "unchanged history is shared, not copied");
    tracker.Update(5000, Line(1), false);
    tracker.Update(8000, Line(1, {Factory(1)}), false);
    const auto second = tracker.GetStatus(0).mHistory;
    Require(second != first && second->size() == 2 && first->size() == 1, "appending creates a new object");
}

void CheckReset()
{
    ProductionLineTracker tracker;
    tracker.Update(0, Line(1), false);
    tracker.Update(5000, Line(1, {Factory(1)}), false);
    tracker.Reset();
    const auto status = tracker.GetStatus(0);
    Require(status.mState == ProductionLineState::NoFactory && !status.mHistory, "reset clears the tracker");
    Require(tracker.GetStatus(60'000).mTotalStalledMs == 0, "reset also clears the stall total");
}

// 总停产时间的纯函数：历史之和，加上持续满 2 秒的进行中停产；不足 2 秒不计。
void CheckTotalStalledFunction()
{
    Require(ComputeTotalStalledMs(0, false, 0, 100'000) == 0, "no history and no stall");
    Require(ComputeTotalStalledMs(7000, false, 5000, 100'000) == 7000, "an inactive stall is ignored");
    Require(ComputeTotalStalledMs(7000, true, 10'000, 11'999) == 7000, "1999 ms of ongoing stall is not counted");
    Require(ComputeTotalStalledMs(7000, true, 10'000, 12'000) == 9000, "an ongoing stall counts from 2000 ms on, in full");
    Require(ComputeTotalStalledMs(7000, true, 10'000, 70'000) == 67'000, "a long ongoing stall adds its whole length");
    Require(ComputeTotalStalledMs(7000, true, 10'000, 5000) == 7000, "a clock earlier than the start adds nothing");
}

void CheckStallRateFunction()
{
    Require(!ComputeStallRatePercent(0, 0) && !ComputeStallRatePercent(5000, 0) && !ComputeStallRatePercent(5000, -1),
            "no game time means no rate");
    Require(ComputeStallRatePercent(0, 60'000) && *ComputeStallRatePercent(0, 60'000) == 0.0f, "no stall is 0%");
    Require(std::abs(*ComputeStallRatePercent(192'000, 2'892'000) - 6.639f) < 0.01f, "192 s of 2892 s is about 6.6%");
    Require(*ComputeStallRatePercent(60'000, 60'000) == 100.0f, "the whole time is 100%");
    Require(*ComputeStallRatePercent(90'000, 60'000) == 100.0f, "the rate never exceeds 100%");
    Require(*ComputeStallRatePercent(-5, 60'000) == 0.0f, "a negative total is treated as zero");
}

// 跟踪器给出的汇总：历史时长之和 + 进行中且已满 2 秒的停产；发布时刻晚于采样时刻时用发布时刻。
void CheckTrackerSummary()
{
    ProductionLineTracker tracker;
    tracker.Update(0, Line(1, {Factory(10)}), false);
    auto status = tracker.GetStatus(0);
    Require(status.mTotalStalledMs == 0 && !status.mStallRatePercent,
            "producing from the start has nothing stalled, and a zero game time has no rate");

    // 1000–5000 停产 4000 ms，写入历史；之后 9000 起再次停产。
    tracker.Update(1000, Line(1), false);
    tracker.Update(3000, Line(1), false);
    tracker.Update(5000, Line(1, {Factory(1)}), false);
    status = tracker.GetStatus(5000);
    Require(status.mTotalStalledMs == 4000 && status.mStallRatePercent && std::abs(*status.mStallRatePercent - 80.0f) < 0.001f,
            "an ended stall contributes its recorded length");

    tracker.Update(9000, Line(1), false);
    status = tracker.GetStatus(9000);
    Require(status.mState == ProductionLineState::Producing && status.mTotalStalledMs == 4000,
            "a brand new stall is not counted yet");
    tracker.Update(10'000, Line(1), false);
    status = tracker.GetStatus(10'999);
    Require(status.mTotalStalledMs == 4000, "1999 ms into the stall nothing is added");
    tracker.Update(11'000, Line(1), false);   // 采样确认停产（9000 起已满 2000 ms）
    status = tracker.GetStatus(11'000);
    Require(status.mState == ProductionLineState::Stalled && status.mStalledSinceMs == 9000 &&
                status.mTotalStalledMs == 4000 + 2000,
            "after 2000 ms the ongoing stall counts in full");
    // 发布时刻晚于最近一次采样时刻：以发布时刻为准。
    status = tracker.GetStatus(19'000);
    Require(status.mTotalStalledMs == 4000 + 10'000 && status.mStallRatePercent &&
                std::abs(*status.mStallRatePercent - 14000.0f * 100.0f / 19000.0f) < 0.01f,
            "the ongoing part follows the publishing clock, and the rate divides by the same clock");
    // 结束后进行中的部分转入历史，总数不重复也不丢失。
    tracker.Update(20'000, Line(1, {Factory(1)}), false);
    status = tracker.GetStatus(20'000);
    Require(status.mTotalStalledMs == 4000 + 11'000 && status.mHistory && status.mHistory->size() == 2,
            "the finished stall moves into the history without being counted twice");
}

// 开局就停产：状态立即是“停产”，但总时间等满 2 秒才计入，之后包含从开局起的整段。
void CheckStallAtGameStartSummary()
{
    ProductionLineTracker tracker;
    tracker.Update(250, Line(1), false);
    auto status = tracker.GetStatus(250);
    Require(status.mState == ProductionLineState::Stalled && status.mTotalStalledMs == 0,
            "a stall at the first observation is shown but not totalled yet");
    status = tracker.GetStatus(2250);
    Require(status.mTotalStalledMs == 2000, "it is totalled in full once it reaches 2000 ms");
    // 不足 2 秒就结束的开局空档：既不入历史也不入总数。
    ProductionLineTracker brief;
    brief.Update(0, Line(1), false);
    brief.Update(1500, Line(1, {Factory(1)}), false);
    status = brief.GetStatus(1500);
    Require(status.mTotalStalledMs == 0 && !status.mHistory, "a start-up gap shorter than 2 seconds is not counted");
}

// 不同产线、无工厂与多次停产的汇总彼此独立。
void CheckSummaryAcrossStates()
{
    ProductionLineTracker tracker;
    tracker.Update(0, Line(0), false);
    Require(tracker.GetStatus(60'000).mTotalStalledMs == 0, "no factory never accumulates stall time");
    tracker.Update(60'000, Line(1), false);
    tracker.Update(70'000, Line(1, {Factory(1)}), false);
    tracker.Update(80'000, Line(1), false);
    tracker.Update(90'000, Line(0), false);
    const auto status = tracker.GetStatus(100'000);
    Require(status.mState == ProductionLineState::NoFactory && status.mTotalStalledMs == 20'000 &&
                status.mHistory && status.mHistory->size() == 2,
            "stalls before and after a factory change add up");
    Require(std::abs(*status.mStallRatePercent - 20.0f) < 0.001f, "the rate is measured over the whole game time");
}
}

int main()
{
    try
    {
        CheckFactoryClassification();
        CheckLineClassification();
        CheckPowerOutage();
        CheckStallThresholdAndHistory();
        CheckShortStallIsNotRecorded();
        CheckOutageAnnotation();
        CheckFirstObservationAndNoFactory();
        CheckHistoryIsImmutableOnceShared();
        CheckReset();
        CheckTotalStalledFunction();
        CheckStallRateFunction();
        CheckTrackerSummary();
        CheckStallAtGameStartSummary();
        CheckSummaryAcrossStates();
        std::cout << "Production monitor tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
