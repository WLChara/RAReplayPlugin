#include "../src/ui/ReplayHudEconomyModel.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace rareplay::ui;
using rareplay::viewer::EconomyCurve;
using rareplay::viewer::EconomyPoint;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

EconomyCurve MakeCurve(std::initializer_list<EconomyPoint> points)
{
    EconomyCurve curve;
    curve.mPoints = points;
    return curve;
}

// 每秒一个点，资金由函数给出。
template <typename TFunction>
EconomyCurve MakeSecondCurve(int seconds, TFunction credits)
{
    EconomyCurve curve;
    for (int second = 0; second < seconds; ++second)
        curve.mPoints.push_back({static_cast<std::int64_t>(second) * 1000, credits(second)});
    return curve;
}

std::span<const EconomyCurve* const> Spans(const std::vector<const EconomyCurve*>& curves)
{
    return curves;
}

void CheckRangeSpans()
{
    Require(!GetEconomyRangeSpanMs(EconomyTimeRange::All), "all has no span");
    Require(GetEconomyRangeSpanMs(EconomyTimeRange::LastTenMinutes) == 600'000, "ten minutes");
    Require(GetEconomyRangeSpanMs(EconomyTimeRange::LastFiveMinutes) == 300'000, "five minutes");
}

void CheckTimeBoundsAndWindow()
{
    const EconomyCurve a = MakeCurve({{5000, 1}, {70'000, 2}});
    const EconomyCurve b = MakeCurve({{0, 1}, {60'000, 2}});
    const EconomyCurve empty;
    std::vector<const EconomyCurve*> curves = {&a, nullptr, &empty, &b};
    const auto bounds = GetEconomyTimeBounds(Spans(curves));
    Require(bounds && bounds->mStartMs == 0 && bounds->mEndMs == 70'000, "bounds span every curve, ignoring null and empty");
    curves = {nullptr, &empty};
    Require(!GetEconomyTimeBounds(Spans(curves)), "no points means no bounds");

    // 全部：从数据起点到终点。
    auto window = ResolveEconomyTimeWindow(EconomyTimeRange::All, EconomyTimeBounds{3000, 1'803'000});
    Require(window.mStartMs == 3000 && window.mSpanMs == 1'800'000 && window.GetEndMs() == 1'803'000, "all range");
    // 最近 10/5 分钟：以数据末点为终点向前取。
    window = ResolveEconomyTimeWindow(EconomyTimeRange::LastTenMinutes, EconomyTimeBounds{0, 1'800'000});
    Require(window.mStartMs == 1'200'000 && window.mSpanMs == 600'000, "last ten minutes of a 30 minute game");
    window = ResolveEconomyTimeWindow(EconomyTimeRange::LastFiveMinutes, EconomyTimeBounds{0, 1'800'000});
    Require(window.mStartMs == 1'500'000 && window.mSpanMs == 300'000 && window.GetEndMs() == 1'800'000,
            "last five minutes of a 30 minute game");
    // 数据不足 N 分钟：从数据起点开始，不凭空延长。
    window = ResolveEconomyTimeWindow(EconomyTimeRange::LastTenMinutes, EconomyTimeBounds{0, 240'000});
    Require(window.mStartMs == 0 && window.mSpanMs == 240'000, "a young game shows what it has");
    // 没有数据、单个点：最小跨度。
    window = ResolveEconomyTimeWindow(EconomyTimeRange::All, std::nullopt);
    Require(window.mStartMs == 0 && window.mSpanMs == ECONOMY_MIN_SPAN_MS, "no data has the minimum span");
    window = ResolveEconomyTimeWindow(EconomyTimeRange::LastFiveMinutes, EconomyTimeBounds{9000, 9000});
    Require(window.mStartMs == 9000 && window.mSpanMs == ECONOMY_MIN_SPAN_MS, "one point has the minimum span");
    window = ResolveEconomyTimeWindow(EconomyTimeRange::All, EconomyTimeBounds{10, 5});
    Require(window.mStartMs == 0 && window.mSpanMs == ECONOMY_MIN_SPAN_MS, "inverted bounds count as no data");
}

void CheckSlice()
{
    // 点：0、10、20、30、40 秒，资金 0、100、200、300、400。
    const EconomyCurve curve = MakeCurve({{0, 0}, {10'000, 100}, {20'000, 200}, {30'000, 300}, {40'000, 400}});
    auto slice = SliceEconomyCurve(curve, 10'000, 30'000);
    Require(slice.mFirst == 1 && slice.mEnd == 4 && !slice.mLeadIn && !slice.mLeadOut,
            "a window on sample times takes exactly those samples and needs no interpolation");
    slice = SliceEconomyCurve(curve, 15'000, 35'000);
    Require(slice.mFirst == 2 && slice.mEnd == 4, "inside samples");
    Require(slice.mLeadIn && slice.mLeadIn->mTimeMs == 15'000 && slice.mLeadIn->mCredits == 150,
            "lead-in is interpolated at the window start");
    Require(slice.mLeadOut && slice.mLeadOut->mTimeMs == 35'000 && slice.mLeadOut->mCredits == 350,
            "lead-out is interpolated at the window end");
    // 窗口在两个样本之间：没有窗口内的点，但有一段线。
    slice = SliceEconomyCurve(curve, 12'000, 18'000);
    Require(slice.mFirst == slice.mEnd && slice.mFirst == 2 && slice.mLeadIn && slice.mLeadIn->mCredits == 120 &&
                slice.mLeadOut && slice.mLeadOut->mCredits == 180,
            "a window between two samples still has a visible segment");
    // 窗口超出数据：只取实际存在的部分，不外推。
    slice = SliceEconomyCurve(curve, -5000, 100'000);
    Require(slice.mFirst == 0 && slice.mEnd == 5 && !slice.mLeadIn && !slice.mLeadOut, "a wide window takes everything");
    slice = SliceEconomyCurve(curve, 50'000, 60'000);
    Require(slice.mFirst == slice.mEnd && !slice.mLeadIn && !slice.mLeadOut, "a window after the data is empty");
    slice = SliceEconomyCurve(curve, -20'000, -10'000);
    Require(slice.mFirst == slice.mEnd && !slice.mLeadIn && !slice.mLeadOut, "a window before the data is empty");
    // 重复时间的点（同一时刻两个样本）不会让插值除零。
    const EconomyCurve duplicate = MakeCurve({{0, 10}, {1000, 20}, {1000, 40}, {2000, 60}});
    slice = SliceEconomyCurve(duplicate, 500, 1500);
    Require(slice.mLeadIn && slice.mLeadIn->mCredits == 15 && slice.mLeadOut && slice.mLeadOut->mCredits == 50,
            "duplicate sample times interpolate safely");
    Require(SliceEconomyCurve(EconomyCurve{}, 0, 1000).mEnd == 0, "an empty curve has an empty slice");
}

void CheckValueBoundsAndYAxis()
{
    // 玩家 A 开局有尖峰 20000，之后降到 3000；玩家 B 一直在 500–900。
    EconomyCurve a;
    for (int second = 0; second < 1200; ++second)
        a.mPoints.push_back({static_cast<std::int64_t>(second) * 1000, second < 60 ? 20'000 : 3000});
    const EconomyCurve b = MakeSecondCurve(1200, [](int second) { return 500 + second % 5 * 100; });
    std::vector<const EconomyCurve*> both = {&a, &b};
    std::vector<const EconomyCurve*> onlyB = {&b};
    std::vector<const EconomyCurve*> onlyA = {&a};

    auto bounds = GetEconomyValueBounds(Spans(both), 0, 1'200'000);
    Require(bounds && bounds->mMin == 500 && bounds->mMax == 20'000, "whole range sees the early spike");
    // 最近 5 分钟（尖峰早已过去）：只看窗口内的数据，不受窗口外的尖峰影响。
    bounds = GetEconomyValueBounds(Spans(both), 900'000, 1'199'000);
    Require(bounds && bounds->mMax == 3000 && bounds->mMin == 500, "a late window ignores the early spike");
    // 只看某位玩家：纵轴只按它的数据缩放。
    bounds = GetEconomyValueBounds(Spans(onlyB), 0, 1'200'000);
    Require(bounds && bounds->mMin == 500 && bounds->mMax == 900, "hiding players removes them from the scale");
    auto axis = ComputeEconomyYAxis(bounds, 4);
    Require(axis.mMin == 0 && axis.mMax == 1000 && axis.mStep == 500, "a 500-900 player gets a 0-1k axis");
    bounds = GetEconomyValueBounds(Spans(onlyA), 0, 1'200'000);
    axis = ComputeEconomyYAxis(bounds, 4);
    Require(axis.mMin == 0 && axis.mMax == 20'000 && axis.mStep == 5000, "the spiking player gets a 0-20k axis");
    // 边界插值点参与缩放：窗口从尖峰到低谷的斜线中间开始。
    const EconomyCurve ramp = MakeCurve({{0, 10'000}, {100'000, 0}});
    std::vector<const EconomyCurve*> rampCurves = {&ramp};
    bounds = GetEconomyValueBounds(Spans(rampCurves), 50'000, 100'000);
    Require(bounds && bounds->mMax == 5000 && bounds->mMin == 0, "the interpolated start value is part of the window");

    // 没有数据与空指针。
    std::vector<const EconomyCurve*> none = {nullptr};
    Require(!GetEconomyValueBounds(Spans(none), 0, 1000), "no curves, no bounds");
    axis = ComputeEconomyYAxis(std::nullopt, 4);
    Require(axis.mMin == 0 && axis.mMax == 1000 && axis.mStep == 500, "no data still has a readable axis");

    // 负值向下延伸；刻度取整；上沿向上取整到刻度。
    axis = ComputeEconomyYAxis(EconomyValueBounds{-300, 7350}, 4);
    Require(axis.mMin == -2000 && axis.mMax == 8000 && axis.mStep == 2000, "negative values extend the axis");
    axis = ComputeEconomyYAxis(EconomyValueBounds{0, 7350}, 4);
    Require(axis.mMin == 0 && axis.mMax == 8000 && axis.mStep == 2000, "the top is rounded up to a tick");
    axis = ComputeEconomyYAxis(EconomyValueBounds{0, 2'500'000}, 5);
    Require(axis.mMax >= 2'500'000 && axis.mStep == 500'000, "millions use round steps");
    axis = ComputeEconomyYAxis(EconomyValueBounds{0, 10}, 0);
    Require(axis.mMax == 1000 && axis.mStep >= 1, "a tick target below one is treated as one");
}

void CheckNearestPoint()
{
    const EconomyCurve curve = MakeCurve({{1000, 10}, {2000, 20}, {4000, 40}});
    Require(!FindNearestEconomyPoint(EconomyCurve{}, 0), "empty curve");
    Require(FindNearestEconomyPoint(curve, -500)->mCredits == 10, "before the first sample takes the first");
    Require(FindNearestEconomyPoint(curve, 9999)->mCredits == 40, "after the last sample takes the last");
    Require(FindNearestEconomyPoint(curve, 1000)->mCredits == 10 && FindNearestEconomyPoint(curve, 4000)->mCredits == 40,
            "exact times");
    Require(FindNearestEconomyPoint(curve, 1400)->mCredits == 10 && FindNearestEconomyPoint(curve, 1600)->mCredits == 20,
            "the closer sample wins");
    Require(FindNearestEconomyPoint(curve, 3000)->mCredits == 20, "a tie takes the earlier sample");
    Require(FindNearestEconomyPoint(curve, 3001)->mCredits == 40, "just past the tie takes the later sample");
}

void CheckDecimation()
{
    std::vector<EconomyPoint> small;
    for (int index = 0; index < 50; ++index)
        small.push_back({index * 1000LL, index});
    Require(DecimateEconomyPoints(small, 64).size() == 50 && DecimateEconomyPoints(small, 50).size() == 50,
            "points within the cap are returned unchanged");
    Require(DecimateEconomyPoints({}, 10).empty(), "no points");

    // 1500 个点压到 200：保留首尾，不超限，升序，不丢尖峰与谷底。
    std::vector<EconomyPoint> large;
    for (int index = 0; index < 1500; ++index)
        large.push_back({index * 1000LL, 1000 + index % 7});
    large[777].mCredits = 99'999;
    large[1111].mCredits = -4;
    for (const std::size_t cap : {4u, 5u, 64u, 200u, 1499u})
    {
        const auto drawn = DecimateEconomyPoints(large, cap);
        Require(drawn.size() <= std::max<std::size_t>(cap, 4) && drawn.size() >= 2, "cap " + std::to_string(cap));
        Require(drawn.front().mTimeMs == large.front().mTimeMs && drawn.back().mTimeMs == large.back().mTimeMs,
                "first and last survive at cap " + std::to_string(cap));
        for (std::size_t index = 1; index < drawn.size(); ++index)
            Require(drawn[index].mTimeMs > drawn[index - 1].mTimeMs, "time ascends at cap " + std::to_string(cap));
        if (cap >= 64)
        {
            const auto has = [&drawn](std::int64_t credits) {
                return std::any_of(drawn.begin(), drawn.end(),
                                   [credits](const EconomyPoint& point) { return point.mCredits == credits; });
            };
            Require(has(99'999) && has(-4), "extremes survive at cap " + std::to_string(cap));
        }
    }
    // 单调曲线的抽稀仍然升序并保留端点。
    std::vector<EconomyPoint> ramp;
    for (int index = 0; index < 12'000; ++index)
        ramp.push_back({index * 1000LL, index});
    const auto rampDrawn = DecimateEconomyPoints(ramp, 300);
    Require(rampDrawn.size() <= 300 && rampDrawn.front().mCredits == 0 && rampDrawn.back().mCredits == 11'999,
            "a long ramp keeps its ends");
}

void CheckLineColor()
{
    const auto luma = [](std::uint32_t rgb) {
        return (0.299 * ((rgb >> 16) & 0xFF) + 0.587 * ((rgb >> 8) & 0xFF) + 0.114 * (rgb & 0xFF)) / 255.0;
    };
    Require(EnsureChartLineColor(0xFFFFFF) == 0xFFFFFF && EnsureChartLineColor(0xDFC13B) == 0xDFC13B &&
                EnsureChartLineColor(0x4E9A78) == 0x4E9A78, "bright player colors are untouched");
    Require(EnsureChartLineColor(0x8B68A8) == 0x8B68A8, "a mid-tone purple is untouched");
    for (const std::uint32_t dark : {0x000000u, 0x0000FFu, 0x101820u, 0x202020u, 0x400000u})
    {
        const std::uint32_t lifted = EnsureChartLineColor(dark);
        Require(luma(lifted) >= 0.345 && (lifted >> 24) == 0, "a dark color is lifted to a readable level");
    }
    Require(EnsureChartLineColor(0xFF0000FF) == EnsureChartLineColor(0x0000FF), "alpha bits are ignored");
}

void CheckFilter()
{
    const std::array<int, 4> houses = {0, 1, 2, 5};
    EconomyPlayerFilter filter;
    Require(filter.CountVisible(houses) == 4 && filter.CountHidden(houses) == 0 && !filter.GetOnlyVisible(houses),
            "everything is visible at first");
    Require(filter.IsVisible(0) && filter.IsVisible(99), "unknown players are visible by default");

    // 只看某位。
    filter.ShowOnly(2, houses);
    Require(filter.CountVisible(houses) == 1 && filter.IsVisible(2) && !filter.IsVisible(0) && !filter.IsVisible(5),
            "only one player is shown");
    Require(filter.GetOnlyVisible(houses) == 2 && filter.CountHidden(houses) == 3, "the sole visible player");
    Require(filter.IsVisible(77), "a player who joins later is visible");
    // 最后一位可见玩家不能被取消。
    Require(!filter.SetVisible(2, false, houses) && filter.IsVisible(2), "the last visible player cannot be hidden");
    // 勾选回别的玩家。
    Require(filter.SetVisible(5, true, houses) && filter.CountVisible(houses) == 2 && !filter.GetOnlyVisible(houses),
            "re-showing a player");
    Require(filter.SetVisible(2, false, houses) && !filter.IsVisible(2) && filter.GetOnlyVisible(houses) == 5,
            "with another player visible the first can be hidden");
    Require(!filter.SetVisible(5, false, houses), "and now the other is the last one");
    // 重复操作无副作用。
    Require(filter.SetVisible(5, true, houses) && filter.SetVisible(2, false, houses) && filter.CountHidden(houses) == 3,
            "idempotent");
    // 全部显示。
    filter.ShowAll();
    Require(filter.CountVisible(houses) == 4, "show all restores everyone");
    // 逐个隐藏直到只剩一位。
    Require(filter.SetVisible(0, false, houses) && filter.SetVisible(1, false, houses) &&
                filter.SetVisible(2, false, houses) && !filter.SetVisible(5, false, houses) && filter.IsVisible(5),
            "hiding stops before the last player");

    // 只看一位不存在的玩家：无效，改为显示全部。
    filter.ShowOnly(42, houses);
    Require(filter.CountVisible(houses) == 4, "an unknown player cannot be isolated");
    // 单个候选时不能隐藏。
    const std::array<int, 1> single = {3};
    EconomyPlayerFilter solo;
    Require(!solo.SetVisible(3, false, single) && !solo.GetOnlyVisible(single), "a lone player stays visible");
    // 隐藏非候选玩家（没有曲线的玩家）总是允许。
    Require(filter.SetVisible(9, false, houses) && !filter.IsVisible(9) && filter.CountVisible(houses) == 4,
            "hiding a non-candidate does not touch the visible count");

    // 候选变化导致没有可见者：Normalize 恢复显示全部。
    EconomyPlayerFilter stale;
    stale.ShowOnly(1, houses);
    const std::array<int, 2> later = {0, 2};
    Require(stale.CountVisible(later) == 0, "the earlier isolation hides every current player");
    stale.Normalize(later);
    Require(stale.CountVisible(later) == 2, "Normalize heals an all-hidden filter");
    const std::array<int, 0> none = {};
    stale.ShowOnly(1, houses);
    stale.Normalize(none);
    Require(!stale.IsVisible(0), "Normalize leaves the filter alone when there are no candidates");

    stale.Reset();
    Require(stale.IsVisible(0) && stale.IsVisible(2), "Reset clears the filter");
}

void CheckLegendPlacementAndState()
{
    Require(ShouldPlaceEconomyLegendBeside(900.0f, 300.0f, 260.0f, 6.0f), "a wide window puts the legend beside the plot");
    Require(ShouldPlaceEconomyLegendBeside(566.0f, 300.0f, 260.0f, 6.0f), "exactly enough room puts it beside");
    Require(!ShouldPlaceEconomyLegendBeside(565.0f, 300.0f, 260.0f, 6.0f), "one pixel short puts it below");
    Require(!ShouldPlaceEconomyLegendBeside(300.0f, 300.0f, 260.0f, 6.0f), "a narrow window puts it below");

    const EconomyUiState state;
    Require(!state.mWindow.mIsOpen && !state.mWindow.mHasPosition && !state.mWindow.mIsDragging &&
                state.mRange == EconomyTimeRange::All && state.mFilter.IsVisible(0),
            "a new session starts closed, centered, unfiltered and on the full range");
}
}

int main()
{
    try
    {
        CheckRangeSpans();
        CheckTimeBoundsAndWindow();
        CheckSlice();
        CheckValueBoundsAndYAxis();
        CheckNearestPoint();
        CheckDecimation();
        CheckLineColor();
        CheckFilter();
        CheckLegendPlacementAndState();
        std::cout << "Replay HUD economy model tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
