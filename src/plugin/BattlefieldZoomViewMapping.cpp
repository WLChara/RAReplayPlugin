#include "BattlefieldZoomViewMapping.h"

#include "../zoom/ZoomLevels.h"

#include <algorithm>
#include <cstddef>

namespace rareplay::plugin
{
namespace
{
using game_api::zoom::ZoomDisableReason;

viewer::BattlefieldZoomView MakeUnavailable(const char* reason)
{
    viewer::BattlefieldZoomView view;
    view.mUnavailableReason = reason;
    return view;
}
}

int GetBattlefieldZoomPercent(int levelIndex) noexcept
{
    if (levelIndex < 0 || levelIndex >= static_cast<int>(rareplay::zoom::ZOOM_LEVELS.size()))
        return 100;
    const rareplay::zoom::ZoomLevel level = rareplay::zoom::ZOOM_LEVELS[static_cast<std::size_t>(levelIndex)];
    // 档位都是小正整数之比，整数四舍五入不会溢出。
    return (200 * level.mNumerator + level.mDenominator) / (2 * level.mDenominator);
}

viewer::BattlefieldZoomView BuildBattlefieldZoomView(const game_api::zoom::BattlefieldZoomStatus& status)
{
    switch (status.mDisableReason)
    {
    case ZoomDisableReason::None:
        break;
    case ZoomDisableReason::NotEnabled:
        return {};
    case ZoomDisableReason::ResourceUnavailable:
        return MakeUnavailable(ZOOM_REASON_OUT_OF_MEMORY);
    case ZoomDisableReason::ScaleFailed:
    case ZoomDisableReason::UnexpectedEngineState:
    case ZoomDisableReason::Exception:
        return MakeUnavailable(ZOOM_REASON_FAILED);
    }
    // 适配器启用时原因必为 None；未启用而没有原因不应出现，按未启用处理。
    if (!status.mIsEnabled)
        return {};
    if (status.mIsGateClosed)
        return MakeUnavailable(ZOOM_REASON_LAYOUT_UNSUPPORTED);

    // 下标越小倍数越大；允许范围两端都含。
    const int mostMagnified = std::min(status.mMostMagnifiedIndex, status.mFarthestShrinkIndex);
    const int farthestShrink = std::max(status.mMostMagnifiedIndex, status.mFarthestShrinkIndex);
    viewer::BattlefieldZoomView view;
    view.mIsAvailable = true;
    view.mIsActive = status.mIsActive;
    view.mPercent = GetBattlefieldZoomPercent(status.mAppliedIndex);
    view.mCanZoomIn = status.mRequestedIndex > mostMagnified;
    view.mCanZoomOut = status.mRequestedIndex < farthestShrink;
    view.mHasPendingChange = status.mRequestedIndex != status.mAppliedIndex;
    return view;
}
}
