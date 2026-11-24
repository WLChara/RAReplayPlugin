#include "ReplayHudEconomyModel.h"

#include "ReplayHudFormat.h"

#include <algorithm>
#include <cmath>

namespace rareplay::ui
{
namespace
{
constexpr std::int64_t MINUTE_MS = 60'000;
// 绘制抽稀每桶保留最小与最大两点，再加首尾两点。
constexpr std::size_t MIN_DECIMATION_POINTS = 4;
// 曲线最低可接受的感知亮度；0.35 在深色图表背景（约 0.07）上对比度约 3:1。
constexpr double MIN_LINE_LUMA = 0.35;

viewer::EconomyPoint Interpolate(const viewer::EconomyPoint& before, const viewer::EconomyPoint& after,
                                 std::int64_t timeMs) noexcept
{
    if (after.mTimeMs <= before.mTimeMs)
        return {timeMs, after.mCredits};
    const double ratio = static_cast<double>(timeMs - before.mTimeMs) /
                         static_cast<double>(after.mTimeMs - before.mTimeMs);
    const double credits = static_cast<double>(before.mCredits) +
                           static_cast<double>(after.mCredits - before.mCredits) * ratio;
    return {timeMs, static_cast<std::int64_t>(std::llround(credits))};
}
}

std::optional<std::int64_t> GetEconomyRangeSpanMs(EconomyTimeRange range) noexcept
{
    switch (range)
    {
    case EconomyTimeRange::LastTenMinutes: return 10 * MINUTE_MS;
    case EconomyTimeRange::LastFiveMinutes: return 5 * MINUTE_MS;
    case EconomyTimeRange::All: break;
    }
    return std::nullopt;
}

std::optional<EconomyTimeBounds> GetEconomyTimeBounds(std::span<const viewer::EconomyCurve* const> curves) noexcept
{
    std::optional<EconomyTimeBounds> bounds;
    for (const viewer::EconomyCurve* curve : curves)
    {
        if (curve == nullptr || curve->mPoints.empty())
            continue;
        const std::int64_t first = curve->mPoints.front().mTimeMs;
        const std::int64_t last = curve->mPoints.back().mTimeMs;
        if (!bounds)
            bounds = EconomyTimeBounds{first, last};
        bounds->mStartMs = std::min(bounds->mStartMs, first);
        bounds->mEndMs = std::max(bounds->mEndMs, last);
    }
    return bounds;
}

EconomyTimeWindow ResolveEconomyTimeWindow(EconomyTimeRange range, std::optional<EconomyTimeBounds> dataBounds) noexcept
{
    EconomyTimeWindow window;
    if (!dataBounds || dataBounds->mEndMs < dataBounds->mStartMs)
        return window;
    window.mStartMs = dataBounds->mStartMs;
    if (const auto span = GetEconomyRangeSpanMs(range))
        window.mStartMs = std::max(dataBounds->mStartMs, dataBounds->mEndMs - *span);
    window.mSpanMs = std::max(dataBounds->mEndMs - window.mStartMs, ECONOMY_MIN_SPAN_MS);
    return window;
}

EconomySlice SliceEconomyCurve(const viewer::EconomyCurve& curve, std::int64_t startMs, std::int64_t endMs) noexcept
{
    EconomySlice slice;
    const auto& points = curve.mPoints;
    const auto byTime = [](const viewer::EconomyPoint& point, std::int64_t time) { return point.mTimeMs < time; };
    const auto firstInside = std::lower_bound(points.begin(), points.end(), startMs, byTime);
    const auto afterInside = std::upper_bound(points.begin(), points.end(), endMs,
        [](std::int64_t time, const viewer::EconomyPoint& point) { return time < point.mTimeMs; });
    slice.mFirst = static_cast<std::size_t>(firstInside - points.begin());
    slice.mEnd = std::max(slice.mFirst, static_cast<std::size_t>(afterInside - points.begin()));

    // 起点之前与之后各有样本（后一个样本晚于起点）：在起点处插值。
    if (slice.mFirst > 0 && slice.mFirst < points.size() && points[slice.mFirst].mTimeMs > startMs)
        slice.mLeadIn = Interpolate(points[slice.mFirst - 1], points[slice.mFirst], startMs);
    // 终点之前与之后各有样本：在终点处插值；两个样本都在窗口外时与 mLeadIn 落在同一线段上。
    if (slice.mEnd > 0 && slice.mEnd < points.size() && points[slice.mEnd - 1].mTimeMs < endMs)
        slice.mLeadOut = Interpolate(points[slice.mEnd - 1], points[slice.mEnd], endMs);
    return slice;
}

std::optional<EconomyValueBounds> GetEconomyValueBounds(std::span<const viewer::EconomyCurve* const> curves,
                                                        std::int64_t startMs, std::int64_t endMs) noexcept
{
    std::optional<EconomyValueBounds> bounds;
    const auto include = [&bounds](std::int64_t credits) {
        if (!bounds)
            bounds = EconomyValueBounds{credits, credits};
        bounds->mMin = std::min(bounds->mMin, credits);
        bounds->mMax = std::max(bounds->mMax, credits);
    };
    for (const viewer::EconomyCurve* curve : curves)
    {
        if (curve == nullptr)
            continue;
        const EconomySlice slice = SliceEconomyCurve(*curve, startMs, endMs);
        for (std::size_t index = slice.mFirst; index < slice.mEnd; ++index)
            include(curve->mPoints[index].mCredits);
        if (slice.mLeadIn)
            include(slice.mLeadIn->mCredits);
        if (slice.mLeadOut)
            include(slice.mLeadOut->mCredits);
    }
    return bounds;
}

EconomyYAxis ComputeEconomyYAxis(std::optional<EconomyValueBounds> bounds, int targetTicks) noexcept
{
    const std::int64_t lowest = bounds ? std::min<std::int64_t>(bounds->mMin, 0) : 0;
    const std::int64_t highest = std::max<std::int64_t>(bounds ? bounds->mMax : 0, 1000);
    const double ticks = static_cast<double>(std::max(targetTicks, 1));
    EconomyYAxis axis;
    axis.mStep = GetNiceCreditsStep(static_cast<double>(highest - lowest) / ticks);
    axis.mMax = (highest + axis.mStep - 1) / axis.mStep * axis.mStep;
    axis.mMin = lowest < 0 ? -((-lowest + axis.mStep - 1) / axis.mStep) * axis.mStep : 0;
    return axis;
}

std::optional<viewer::EconomyPoint> FindNearestEconomyPoint(const viewer::EconomyCurve& curve,
                                                            std::int64_t timeMs) noexcept
{
    const auto& points = curve.mPoints;
    if (points.empty())
        return std::nullopt;
    const auto next = std::lower_bound(points.begin(), points.end(), timeMs,
        [](const viewer::EconomyPoint& point, std::int64_t time) { return point.mTimeMs < time; });
    if (next == points.begin())
        return *next;
    if (next == points.end())
        return points.back();
    const viewer::EconomyPoint& before = *(next - 1);
    return timeMs - before.mTimeMs <= next->mTimeMs - timeMs ? before : *next;
}

std::vector<viewer::EconomyPoint> DecimateEconomyPoints(std::span<const viewer::EconomyPoint> points,
                                                        std::size_t maxPoints)
{
    maxPoints = std::max(maxPoints, MIN_DECIMATION_POINTS);
    if (points.size() <= maxPoints)
        return std::vector<viewer::EconomyPoint>(points.begin(), points.end());

    std::vector<viewer::EconomyPoint> result;
    result.reserve(maxPoints);
    result.push_back(points.front());
    const std::size_t interior = points.size() - 2;
    const std::size_t buckets = (maxPoints - 2) / 2;
    for (std::size_t bucket = 0; bucket < buckets; ++bucket)
    {
        // interior > maxPoints - 2 >= buckets，所以每个桶至少有一个点。
        const std::size_t begin = 1 + bucket * interior / buckets;
        const std::size_t end = 1 + (bucket + 1) * interior / buckets;
        std::size_t lowIndex = begin;
        std::size_t highIndex = begin;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (points[index].mCredits < points[lowIndex].mCredits)
                lowIndex = index;
            if (points[index].mCredits > points[highIndex].mCredits)
                highIndex = index;
        }
        result.push_back(points[std::min(lowIndex, highIndex)]);
        if (lowIndex != highIndex)
            result.push_back(points[std::max(lowIndex, highIndex)]);
    }
    result.push_back(points.back());
    return result;
}

std::uint32_t EnsureChartLineColor(std::uint32_t rgb) noexcept
{
    const double red = static_cast<double>((rgb >> 16) & 0xFF);
    const double green = static_cast<double>((rgb >> 8) & 0xFF);
    const double blue = static_cast<double>(rgb & 0xFF);
    const double luma = (0.299 * red + 0.587 * green + 0.114 * blue) / 255.0;
    if (luma >= MIN_LINE_LUMA)
        return rgb & 0xFFFFFF;
    // 向白色混合 t：亮度从 luma 线性升到 MIN_LINE_LUMA。
    const double mix = (MIN_LINE_LUMA - luma) / (1.0 - luma);
    const auto lift = [mix](double channel) {
        return static_cast<std::uint32_t>(std::lround(std::min(255.0, channel + (255.0 - channel) * mix)));
    };
    return (lift(red) << 16) | (lift(green) << 8) | lift(blue);
}

bool EconomyPlayerFilter::IsVisible(int houseIndex) const noexcept
{
    return !std::binary_search(mHidden.begin(), mHidden.end(), houseIndex);
}

void EconomyPlayerFilter::ShowOnly(int houseIndex, std::span<const int> candidates)
{
    if (std::find(candidates.begin(), candidates.end(), houseIndex) == candidates.end())
    {
        ShowAll();
        return;
    }
    mHidden.clear();
    for (const int candidate : candidates)
    {
        if (candidate != houseIndex)
            mHidden.push_back(candidate);
    }
    std::sort(mHidden.begin(), mHidden.end());
    mHidden.erase(std::unique(mHidden.begin(), mHidden.end()), mHidden.end());
}

bool EconomyPlayerFilter::SetVisible(int houseIndex, bool isVisible, std::span<const int> candidates)
{
    const auto position = std::lower_bound(mHidden.begin(), mHidden.end(), houseIndex);
    const bool isHidden = position != mHidden.end() && *position == houseIndex;
    if (isVisible)
    {
        if (isHidden)
            mHidden.erase(position);
        return true;
    }
    if (isHidden)
        return true;
    // 隐藏它之后候选中必须还有可见者：本来可见的候选只有它自己时拒绝。
    const std::size_t visible = CountVisible(candidates);
    const bool isCandidate = std::find(candidates.begin(), candidates.end(), houseIndex) != candidates.end();
    if (isCandidate && visible <= 1)
        return false;
    mHidden.insert(position, houseIndex);
    return true;
}

std::size_t EconomyPlayerFilter::CountVisible(std::span<const int> candidates) const noexcept
{
    return static_cast<std::size_t>(std::count_if(candidates.begin(), candidates.end(),
        [this](int houseIndex) { return IsVisible(houseIndex); }));
}

std::size_t EconomyPlayerFilter::CountHidden(std::span<const int> candidates) const noexcept
{
    return candidates.size() - CountVisible(candidates);
}

std::optional<int> EconomyPlayerFilter::GetOnlyVisible(std::span<const int> candidates) const noexcept
{
    if (candidates.size() < 2 || CountVisible(candidates) != 1)
        return std::nullopt;
    for (const int houseIndex : candidates)
    {
        if (IsVisible(houseIndex))
            return houseIndex;
    }
    return std::nullopt;
}

void EconomyPlayerFilter::Normalize(std::span<const int> candidates) noexcept
{
    if (!candidates.empty() && CountVisible(candidates) == 0)
        ShowAll();
}

bool ShouldPlaceEconomyLegendBeside(float contentWidth, float legendWidth, float minPlotWidth, float gap) noexcept
{
    return contentWidth - legendWidth - gap >= minPlotWidth;
}
}
