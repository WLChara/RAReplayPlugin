#include "ZoomLevels.h"

#include "ZoomMapping.h"

#include <algorithm>
#include <limits>

namespace rareplay::zoom
{
namespace
{
bool IsValidExtent(int size) noexcept
{
    return size > 0 && size <= MAX_ZOOM_EXTENT;
}

int ComputeLogicalExtent(int physicalSize, ZoomLevel level) noexcept
{
    if (level.mNumerator == level.mDenominator)
        return physicalSize;
    // 最近的偶数 2k：k = round(P·den / (2·num))，恰在中间时进位。P ≤ 2^15、den < 2^31，乘积在 64 位内。
    const std::int64_t numerator = static_cast<std::int64_t>(physicalSize) * level.mDenominator + level.mNumerator;
    const std::int64_t denominator = 2 * static_cast<std::int64_t>(level.mNumerator);
    const std::int64_t extent = 2 * FloorDivide(numerator, denominator);
    // 结果超出 int 时饱和；它必然超过 MAX_ZOOM_EXTENT，由调用方判定不可用。
    return static_cast<int>(std::clamp<std::int64_t>(extent, MIN_LOGICAL_EXTENT, std::numeric_limits<int>::max()));
}

bool TryMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& outProduct) noexcept
{
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    outProduct = left * right;
    return true;
}

// 2 块缓冲 × 2 字节 × 面积。
bool TryTwoBufferBytes(LogicalSize size, std::uint64_t& outBytes) noexcept
{
    if (size.mWidth < 0 || size.mHeight < 0)
        return false;
    std::uint64_t area = 0;
    return TryMultiply(static_cast<std::uint64_t>(size.mWidth), static_cast<std::uint64_t>(size.mHeight), area) &&
        TryMultiply(area, 4, outBytes);
}
}

LogicalSize ComputeLogicalSize(int physicalWidth, int physicalHeight, ZoomLevel level) noexcept
{
    if (!IsValidExtent(physicalWidth) || !IsValidExtent(physicalHeight) ||
        level.mNumerator <= 0 || level.mDenominator <= 0)
        return {};
    return {ComputeLogicalExtent(physicalWidth, level), ComputeLogicalExtent(physicalHeight, level)};
}

std::optional<std::uint64_t> EstimateLogicalResourceBytes(LogicalSize current, LogicalSize farthest) noexcept
{
    std::uint64_t farthestBytes = 0;
    std::uint64_t currentBytes = 0;
    if (!TryTwoBufferBytes(farthest, farthestBytes) || !TryTwoBufferBytes(current, currentBytes) ||
        currentBytes > std::numeric_limits<std::uint64_t>::max() - farthestBytes)
        return std::nullopt;
    return farthestBytes + currentBytes;
}

ZoomLevelPolicy::ZoomLevelPolicy(int physicalWidth, int physicalHeight, std::uint64_t memoryBudgetBytes,
    int configuredFarthestShrinkIndex) noexcept
    : mPhysicalWidth(physicalWidth)
    , mPhysicalHeight(physicalHeight)
{
    if (!IsValidExtent(physicalWidth) || !IsValidExtent(physicalHeight))
        return;
    if (physicalWidth >= MIN_LOGICAL_EXTENT && physicalHeight >= MIN_LOGICAL_EXTENT)
        mMostMagnifiedIndex = MOST_MAGNIFIED_ZOOM_INDEX;
    const int configured = std::clamp(configuredFarthestShrinkIndex, IDENTITY_ZOOM_INDEX, FARTHEST_SHRINK_ZOOM_INDEX);
    for (int index = configured; index > IDENTITY_ZOOM_INDEX; --index)
    {
        const LogicalSize size = ComputeLogicalSize(physicalWidth, physicalHeight, ZOOM_LEVELS[static_cast<std::size_t>(index)]);
        const ZoomTransform transform{physicalWidth, physicalHeight, size.mWidth, size.mHeight};
        const std::optional<std::uint64_t> bytes = EstimateLogicalResourceBytes(size, size);
        if (transform.IsValid() && bytes && *bytes <= memoryBudgetBytes)
        {
            mFarthestShrinkIndex = index;
            return;
        }
    }
}

int ZoomLevelPolicy::ClampIndex(int index) const noexcept
{
    return std::clamp(index, mMostMagnifiedIndex, mFarthestShrinkIndex);
}

int ZoomLevelPolicy::Step(int currentIndex, int steps) const noexcept
{
    const std::int64_t target = static_cast<std::int64_t>(ClampIndex(currentIndex)) - steps;
    return static_cast<int>(std::clamp<std::int64_t>(target, mMostMagnifiedIndex, mFarthestShrinkIndex));
}

LogicalSize ZoomLevelPolicy::GetLogicalSize(int index) const noexcept
{
    return ComputeLogicalSize(mPhysicalWidth, mPhysicalHeight, ZOOM_LEVELS[static_cast<std::size_t>(ClampIndex(index))]);
}
}
