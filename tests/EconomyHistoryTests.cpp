#include "../src/stats/EconomyHistory.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::stats;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static_assert(static_cast<std::int64_t>(ECONOMY_SAMPLE_CAPACITY) * ECONOMY_SAMPLE_INTERVAL_MS >= 3LL * 3600 * 1000,
              "ring buffer must cover at least three hours of game time");
static_assert(ECONOMY_PUBLISH_MAX_POINTS == 1500, "a published curve has 1500 points at most");

void CheckEmptyAndBasic()
{
    EconomyHistory history(8);
    auto curve = history.BuildCurve();
    Require(curve && curve->mPoints.empty(), "no samples gives an empty curve, not a null one");
    Require(history.Add(1000, 500) && history.Add(2000, 700) && history.Add(2000, 900), "samples are appended");
    curve = history.BuildCurve();
    Require(curve->mPoints.size() == 3 && curve->mPoints[0].mTimeMs == 1000 && curve->mPoints[0].mCredits == 500 &&
                curve->mPoints[2].mCredits == 900, "samples keep their order");
    Require(!history.Add(1500, 1) && history.GetSize() == 3, "time going backwards is rejected");
    Require(!history.Add(-1, 1) && !history.Add(std::numeric_limits<std::int64_t>::max(), 1),
            "unrepresentable time is rejected");
}

void CheckRingOverwritesOldest()
{
    EconomyHistory history(5);
    for (int index = 0; index < 8; ++index)
        history.Add(index * 1000, index * 10);
    Require(history.GetSize() == 5 && history.GetCapacity() == 5, "ring stays at capacity");
    const auto curve = history.BuildCurve();
    Require(curve->mPoints.size() == 5 && curve->mPoints.front().mTimeMs == 3000 &&
                curve->mPoints.back().mTimeMs == 7000 && curve->mPoints.back().mCredits == 70,
            "the oldest samples are overwritten and the order survives wrap-around");
    for (std::size_t index = 1; index < curve->mPoints.size(); ++index)
        Require(curve->mPoints[index].mTimeMs > curve->mPoints[index - 1].mTimeMs, "ascending after wrap");
}

void CheckCreditsSaturate()
{
    EconomyHistory history(4);
    history.Add(0, std::numeric_limits<std::int64_t>::max());
    history.Add(1000, std::numeric_limits<std::int64_t>::min());
    const auto curve = history.BuildCurve();
    Require(curve->mPoints[0].mCredits == std::numeric_limits<std::int32_t>::max() &&
                curve->mPoints[1].mCredits == std::numeric_limits<std::int32_t>::min(),
            "credits saturate to 32 bits instead of wrapping");
}

void CheckSelectIndices()
{
    Require(SelectEconomyIndices(0, 600).empty(), "no samples selects nothing");
    Require(SelectEconomyIndices(1, 600) == std::vector<std::size_t>{0}, "one sample");
    const auto all = SelectEconomyIndices(600, 600);
    Require(all.size() == 600 && all.front() == 0 && all.back() == 599, "at the limit everything is kept");
    for (const std::size_t count : {601u, 700u, 1199u, 1200u, 5000u, 10800u, 12000u, 100000u})
    {
        const auto indices = SelectEconomyIndices(count, 600);
        Require(indices.size() <= 600 && indices.size() >= 2, "selection never exceeds the cap: " + std::to_string(count));
        Require(indices.front() == 0 && indices.back() == count - 1, "first and last samples are kept: " + std::to_string(count));
        for (std::size_t index = 1; index < indices.size(); ++index)
            Require(indices[index] > indices[index - 1], "indices ascend: " + std::to_string(count));
    }
    // 发布用的上限 1500：不超过上限时全部保留，超过后仍保留首尾且不超限，步长随样本数增长。
    constexpr std::size_t CAP = ECONOMY_PUBLISH_MAX_POINTS;
    Require(SelectEconomyIndices(CAP, CAP).size() == CAP, "exactly at the publish cap everything is kept");
    for (const std::size_t count : std::initializer_list<std::size_t>{CAP - 1, CAP + 1, CAP + 2, 2 * CAP, 3600, 10800,
                                                                       ECONOMY_SAMPLE_CAPACITY})
    {
        const auto indices = SelectEconomyIndices(count, CAP);
        Require(indices.size() <= CAP && indices.size() >= std::min<std::size_t>(count, 2),
                "publish cap is never exceeded: " + std::to_string(count));
        Require(indices.front() == 0 && indices.back() == count - 1,
                "publish selection keeps the first and last samples: " + std::to_string(count));
        for (std::size_t index = 1; index < indices.size(); ++index)
            Require(indices[index] > indices[index - 1], "publish indices ascend: " + std::to_string(count));
    }
    // 12000 个样本（3 小时 20 分钟）约每 9 个取一点，仍有 1300 点以上。
    Require(SelectEconomyIndices(ECONOMY_SAMPLE_CAPACITY, CAP).size() > 1300, "a full ring keeps over 1300 points");

    const auto tiny = SelectEconomyIndices(10, 2);
    Require(tiny == std::vector<std::size_t>({0, 9}), "a cap below two is treated as two");
    const auto clamped = SelectEconomyIndices(10, 0);
    Require(clamped == std::vector<std::size_t>({0, 9}), "zero cap is treated as two");
}

// 三小时又一点的样本（每秒一个）发布时不超过 1500 点，且首尾都在。
void CheckDownsampledCurve()
{
    EconomyHistory history;
    const int seconds = 3 * 3600 + 120;
    for (int second = 0; second < seconds; ++second)
        history.Add(static_cast<std::int64_t>(second) * 1000, second);
    Require(history.GetSize() == static_cast<std::size_t>(seconds), "three hours fit in the default ring");
    const auto curve = history.BuildCurve();
    Require(curve->mPoints.size() <= ECONOMY_PUBLISH_MAX_POINTS && curve->mPoints.size() > 1000,
            "about 1500 points at most, more than the old 600-point resolution");
    Require(curve->mPoints.front().mTimeMs == 0 && curve->mPoints.back().mTimeMs == (seconds - 1) * 1000 &&
                curve->mPoints.back().mCredits == seconds - 1, "first and last samples survive downsampling");
}

void CheckDefaultCapacityWraps()
{
    EconomyHistory history;
    const std::size_t extra = 500;
    for (std::size_t second = 0; second < ECONOMY_SAMPLE_CAPACITY + extra; ++second)
        history.Add(static_cast<std::int64_t>(second) * 1000, 1);
    Require(history.GetSize() == ECONOMY_SAMPLE_CAPACITY, "default ring is full");
    const auto curve = history.BuildCurve();
    Require(curve->mPoints.front().mTimeMs == static_cast<std::int64_t>(extra) * 1000 &&
                curve->mPoints.size() <= ECONOMY_PUBLISH_MAX_POINTS,
            "after wrapping the curve starts at the oldest retained sample");
}

// 样本没有变化时重复发布返回同一个不可变对象；新样本或不同点数上限才重建。
void CheckCurveSharing()
{
    EconomyHistory history(100);
    for (int index = 0; index < 50; ++index)
        history.Add(index * 1000, index);
    const auto first = history.BuildCurve();
    Require(history.BuildCurve() == first, "unchanged samples share one curve object");
    const auto snapshotCopy = first;
    history.Add(50'000, 50);
    const auto second = history.BuildCurve();
    Require(second != first && first->mPoints.size() == 50 && second->mPoints.size() == 51 && snapshotCopy == first,
            "a new sample builds a new curve and leaves the published one untouched");
    const auto limited = history.BuildCurve(10);
    Require(limited != second && limited->mPoints.size() <= 10 && history.BuildCurve(10) == limited,
            "a different point limit rebuilds once and is then shared");
}

// 发布开销：满环缓冲（12000 个样本）每来一个新样本就要重建一次曲线。只记录耗时，不设阈值（机器差异大）。
void ReportBuildCost()
{
    for (const std::size_t cap : {std::size_t{600}, ECONOMY_PUBLISH_MAX_POINTS})
    {
        EconomyHistory history;
        for (std::size_t second = 0; second < ECONOMY_SAMPLE_CAPACITY; ++second)
            history.Add(static_cast<std::int64_t>(second) * 1000, static_cast<std::int64_t>(second));
        constexpr int REBUILDS = 2000;
        std::size_t points = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (int rebuild = 0; rebuild < REBUILDS; ++rebuild)
        {
            history.Add(static_cast<std::int64_t>(ECONOMY_SAMPLE_CAPACITY + rebuild) * 1000, rebuild);
            points = history.BuildCurve(cap)->mPoints.size();
        }
        const double microseconds =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / REBUILDS;
        std::cout << "BuildCurve of a full ring, cap " << cap << ": " << points << " points, " << microseconds
                  << " us per rebuild (" << points * sizeof(rareplay::viewer::EconomyPoint) << " bytes)\n";
    }
}
}

int main()
{
    try
    {
        ReportBuildCost();
        CheckEmptyAndBasic();
        CheckRingOverwritesOldest();
        CheckCreditsSaturate();
        CheckSelectIndices();
        CheckDownsampledCurve();
        CheckDefaultCapacityWraps();
        CheckCurveSharing();
        std::cout << "Economy history tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
