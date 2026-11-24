#include "ZoomFramePlan.h"

#include "../../target/TargetAbi.h"
#include "../../zoom/ZoomLevels.h"

#include <algorithm>
#include <cstdlib>

namespace rareplay::game_api::zoom
{
namespace
{
using rareplay::zoom::IDENTITY_ZOOM_INDEX;

ZoomFramePlan MakePlan(ZoomFrameAction action, int targetIndex, bool isFullRedraw, bool isLevelChange) noexcept
{
    ZoomFramePlan plan;
    plan.mAction = action;
    plan.mTargetIndex = targetIndex;
    plan.mIsFullRedraw = isFullRedraw;
    plan.mIsLevelChange = isLevelChange;
    return plan;
}

// 不进入窗口：激活时完整退回 1× 并全重绘（物理 Tile 在缩放期间没有更新），否则原样返回。
ZoomFramePlan LeaveOrKeep(bool isActive, bool isLevelChange) noexcept
{
    return isActive ? MakePlan(ZoomFrameAction::ExitToIdentity, IDENTITY_ZOOM_INDEX, true, isLevelChange)
                    : MakePlan(ZoomFrameAction::KeepNative, IDENTITY_ZOOM_INDEX, false, false);
}
}

bool IsZoomGateOpen(const ZoomGateState& gate) noexcept
{
    return gate.mHasTactical && gate.mIsZoomInFactorIdentity && gate.mIsSidebarOnRight && !gate.mIsArmageddonMode &&
        gate.mIsViewOriginZero && gate.mIsFrameSurfaceSupported;
}

ZoomFramePlan PlanZoomFrame(const ZoomFrameInput& input) noexcept
{
    const bool isActive = input.mAppliedIndex != IDENTITY_ZOOM_INDEX;
    // 停用与门控失败都不推迟：常驻 L 视口下跳过窗口会让 Render 以 L 画进物理面。
    if (!input.mIsEnabled || !IsZoomGateOpen(input.mGate) || input.mPhysicalWidth <= 0 || input.mPhysicalHeight <= 0)
        return LeaveOrKeep(isActive, false);

    const int lowest = std::min(input.mMostMagnifiedIndex, input.mFarthestShrinkIndex);
    const int highest = std::max(input.mMostMagnifiedIndex, input.mFarthestShrinkIndex);
    const int applied = std::clamp(input.mAppliedIndex, lowest, highest);
    const int requested = std::clamp(input.mRequestedIndex, lowest, highest);
    // 拖框的起点与终点必须处于同一映射；平移动画会在之后各帧覆盖锚点；嵌套离屏重绘与前后帧保持同一档位。
    const bool isDeferred = input.mIsGestureActive || input.mIsCameraPanActive || input.mIsNestedOffscreenRedraw;
    const int targetIndex = isDeferred ? applied : requested;
    const bool isLevelChange = targetIndex != input.mAppliedIndex;
    if (targetIndex == IDENTITY_ZOOM_INDEX)
        return LeaveOrKeep(isActive, isLevelChange);

    const bool isPhysicalChanged = isActive &&
        (input.mPhysicalWidth != input.mAppliedPhysicalWidth || input.mPhysicalHeight != input.mAppliedPhysicalHeight);
    const bool isViewportReset = isActive &&
        (input.mViewportWidth != input.mAppliedLogicalWidth || input.mViewportHeight != input.mAppliedLogicalHeight);
    if (!isActive || isLevelChange || isPhysicalChanged || isViewportReset || !input.mAreResourcesReady)
        return MakePlan(ZoomFrameAction::ApplyLevelThenEnter, targetIndex, true, isLevelChange);
    // VisibleCells 溢出后增量重绘会漏掉被丢弃的格子；全量重绘下没有可见后果。
    return MakePlan(ZoomFrameAction::Enter, targetIndex, input.mVisibleCellCount >= target::VISIBLE_CELL_LIMIT, false);
}

int StepTowardIdentity(int index) noexcept
{
    if (index < IDENTITY_ZOOM_INDEX)
        return index + 1;
    if (index > IDENTITY_ZOOM_INDEX)
        return index - 1;
    return index;
}

ExposedBands ComputeExposedBands(int scrollX, int scrollY, int logicalWidth, int logicalHeight, bool isFullRedraw) noexcept
{
    ExposedBands bands;
    if (logicalWidth <= 0 || logicalHeight <= 0)
        return bands;
    const auto appendBand = [&bands](int x, int y, int width, int height) noexcept {
        bands.mRects[static_cast<std::size_t>(bands.mCount++)] = {x, y, width, height};
    };
    // 64 位取绝对值，避免 INT_MIN 溢出；Render(0) 在 |dx| ≥ VB.W 或 |dy| ≥ VB.H 时本就置全重绘。
    const long long distanceX = std::llabs(static_cast<long long>(scrollX));
    const long long distanceY = std::llabs(static_cast<long long>(scrollY));
    if (isFullRedraw || distanceX >= logicalWidth || distanceY >= logicalHeight)
    {
        appendBand(0, 0, logicalWidth, logicalHeight);
        return bands;
    }
    const int bandWidth = static_cast<int>(distanceX);
    const int bandHeight = static_cast<int>(distanceY);
    if (scrollX > 0)
        appendBand(0, 0, bandWidth, logicalHeight);
    else if (scrollX < 0)
        appendBand(logicalWidth - bandWidth, 0, bandWidth, logicalHeight);
    if (scrollY > 0)
        appendBand(0, 0, logicalWidth, bandHeight);
    else if (scrollY < 0)
        appendBand(0, logicalHeight - bandHeight, logicalWidth, bandHeight);
    return bands;
}

std::uint32_t HalfExtentFloatBits(int extent) noexcept
{
    constexpr int MAX_EXACT_EXTENT = (1 << 24) - 1;
    const auto value = static_cast<std::uint32_t>(std::clamp(extent, 0, MAX_EXACT_EXTENT));
    if (value == 0)
        return 0;
    // value = 1.m × 2^k（k ≤ 23，尾数精确）；乘 0.5 只把指数减 1，即 2^(k−1)，偏置后为 k − 1 + 127。
    int highestBit = 0;
    while ((value >> (highestBit + 1)) != 0)
        ++highestBit;
    const std::uint32_t exponent = static_cast<std::uint32_t>(highestBit - 1 + 127);
    const std::uint32_t mantissa = (value << (23 - highestBit)) & 0x007FFFFFu;
    return (exponent << 23) | mantissa;
}
}
