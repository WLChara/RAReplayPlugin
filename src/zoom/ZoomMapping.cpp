#include "ZoomMapping.h"

#include <algorithm>
#include <limits>

namespace rareplay::zoom
{
namespace
{
constexpr std::int64_t INT_LOWEST = std::numeric_limits<int>::min();
constexpr std::int64_t INT_HIGHEST = std::numeric_limits<int>::max();

bool IsValidExtent(int size) noexcept
{
    return size > 0 && size <= MAX_ZOOM_EXTENT;
}

int SaturateToInt(std::int64_t value) noexcept
{
    return static_cast<int>(std::clamp(value, INT_LOWEST, INT_HIGHEST));
}

// 饱和后仍须满足 mEnd > mBegin，所以起点最多到 INT_MAX − 1。
SourceSpan MakeSaturatedSpan(std::int64_t begin, std::int64_t end) noexcept
{
    const std::int64_t clampedBegin = std::clamp(begin, INT_LOWEST, INT_HIGHEST - 1);
    const std::int64_t clampedEnd = std::clamp(end, clampedBegin + 1, INT_HIGHEST);
    return {static_cast<int>(clampedBegin), static_cast<int>(clampedEnd)};
}

std::int64_t CeilDivide(std::int64_t numerator, std::int64_t denominator) noexcept
{
    return -FloorDivide(-numerator, denominator);
}

struct WideSpan
{
    std::int64_t mBegin = 0;
    std::int64_t mEnd = 1;
};

// 未饱和的区间；尺寸不合法时为恒等区间 [p, p+1)。
WideSpan ComputeWideSpan(int physicalPixel, int physicalSize, int logicalSize) noexcept
{
    const std::int64_t pixel = physicalPixel;
    if (!IsValidExtent(physicalSize) || !IsValidExtent(logicalSize))
        return {pixel, pixel + 1};
    // |p| < 2^31、L ≤ 2^15，乘积小于 2^47，64 位内不会溢出。
    const std::int64_t begin = FloorDivide(pixel * logicalSize, physicalSize);
    const std::int64_t end = std::max(begin + 1, FloorDivide((pixel + 1) * logicalSize, physicalSize));
    return {begin, end};
}
}

SourceSpan SpanForPhysicalPixel(int physicalPixel, int physicalSize, int logicalSize) noexcept
{
    const WideSpan span = ComputeWideSpan(physicalPixel, physicalSize, logicalSize);
    return MakeSaturatedSpan(span.mBegin, span.mEnd);
}

int MapPhysicalToLogical(int physicalPixel, int physicalSize, int logicalSize) noexcept
{
    // 用未饱和的区间求中点，L == P 时对全部 int（含 INT_MAX）恒等。
    // 区间长度至少为 1，(length − 1) / 2 非负，截断除法即向下取整；结果落在 [mBegin, mEnd) 内。
    const WideSpan span = ComputeWideSpan(physicalPixel, physicalSize, logicalSize);
    return SaturateToInt(span.mBegin + (span.mEnd - span.mBegin - 1) / 2);
}

int MapLogicalToPhysical(int logicalPixel, int physicalSize, int logicalSize) noexcept
{
    if (!IsValidExtent(physicalSize) || !IsValidExtent(logicalSize))
        return logicalPixel;
    // 区间包含 x 的物理像素满足 s0(p) ≤ x < s1(p)。s1 单调不减，取 s1(p) > x 的最小 p：
    // s1(p) > x ⇔ floor(p·L/P) ≥ x 或 floor((p+1)·L/P) ≥ x+1 ⇔ p ≥ ceil(x·P/L) 或 p ≥ ceil((x+1)·P/L) − 1。
    // 直接取 floor(x·P/L) 不保证区间包含 x（例：P=3、L=2、x=1 得 1，而物理像素 1 显示逻辑像素 0）。
    const std::int64_t logical = logicalPixel;
    const std::int64_t first = CeilDivide(logical * physicalSize, logicalSize);
    const std::int64_t last = CeilDivide((logical + 1) * physicalSize, logicalSize) - 1;
    return SaturateToInt(std::min(first, last));
}

bool ZoomTransform::IsValid() const noexcept
{
    return IsValidExtent(mPhysicalWidth) && IsValidExtent(mPhysicalHeight) &&
        IsValidExtent(mLogicalWidth) && IsValidExtent(mLogicalHeight);
}

bool ZoomTransform::IsIdentity() const noexcept
{
    return IsValid() && mPhysicalWidth == mLogicalWidth && mPhysicalHeight == mLogicalHeight;
}

ZoomPoint ZoomTransform::MapPhysicalToLogical(ZoomPoint physicalPoint) const noexcept
{
    return {zoom::MapPhysicalToLogical(physicalPoint.mX, mPhysicalWidth, mLogicalWidth),
        zoom::MapPhysicalToLogical(physicalPoint.mY, mPhysicalHeight, mLogicalHeight)};
}

ZoomPoint ZoomTransform::MapLogicalToPhysical(ZoomPoint logicalPoint) const noexcept
{
    return {zoom::MapLogicalToPhysical(logicalPoint.mX, mPhysicalWidth, mLogicalWidth),
        zoom::MapLogicalToPhysical(logicalPoint.mY, mPhysicalHeight, mLogicalHeight)};
}

bool SpanTable::IsBuiltFor(int physicalSize, int logicalSize) const noexcept
{
    return !mSpans.empty() && mPhysicalSize == physicalSize && mLogicalSize == logicalSize;
}

SpanTable BuildSpanTable(int physicalSize, int logicalSize)
{
    SpanTable table;
    if (!IsValidExtent(physicalSize) || !IsValidExtent(logicalSize))
        return table;
    table.mSpans.resize(static_cast<std::size_t>(physicalSize));
    for (int pixel = 0; pixel < physicalSize; ++pixel)
    {
        const SourceSpan span = SpanForPhysicalPixel(pixel, physicalSize, logicalSize);
        table.mSpans[static_cast<std::size_t>(pixel)] = span;
        table.mMaxSpanLength = std::max(table.mMaxSpanLength, span.mEnd - span.mBegin);
    }
    table.mPhysicalSize = physicalSize;
    table.mLogicalSize = logicalSize;
    return table;
}
}
