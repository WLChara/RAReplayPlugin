#include "ViewerCommandRouter.h"

#include <algorithm>

namespace rareplay::plugin
{
namespace
{
// 快进中不执行的命令（界面同时禁用对应控件）：快进结束时恢复跳转前的暂停状态，期间不接受暂停；
// 半黑幕、镜头跳转与战局缩放要重绘或移动镜头，而快进期间战场不重绘（只有兜底时每秒一次）。
bool IsBlockedWhileSeeking(viewer::ViewerCommandKind kind) noexcept
{
    switch (kind)
    {
    case viewer::ViewerCommandKind::SetPaused:
    case viewer::ViewerCommandKind::SetHalfShroud:
    case viewer::ViewerCommandKind::CenterOnRadarEvent:
    case viewer::ViewerCommandKind::AdjustBattlefieldZoom:
    case viewer::ViewerCommandKind::AdjustBattlefieldZoomAtCenter:
    case viewer::ViewerCommandKind::ResetBattlefieldZoom:
        return true;
    default:
        return false;
    }
}
}

std::optional<ViewerAction> RouteViewerCommand(const viewer::ViewerCommand& command,
    const ViewerRouterState& state) noexcept
{
    if (command.mSessionId != state.mSessionId)
        return std::nullopt;
    if (state.mIsSeeking && IsBlockedWhileSeeking(command.mKind))
        return std::nullopt;
    ViewerAction action;
    switch (command.mKind)
    {
    case viewer::ViewerCommandKind::SetPlaybackSpeed:
        action.mKind = ViewerActionKind::SetPlaybackSpeed;
        action.mPlaybackSpeedSetting = command.mValue;
        return action;
    case viewer::ViewerCommandKind::SetPaused:
        action.mKind = ViewerActionKind::SetPaused;
        action.mIsPaused = command.mValue != 0;
        return action;
    case viewer::ViewerCommandKind::RestartPlayback:
        if (state.mHasExitRequested)
            return std::nullopt;
        action.mKind = ViewerActionKind::RequestRestart;
        action.mIsHalfShroudEnabled = state.mIsHalfShroudEnabled;
        return action;
    case viewer::ViewerCommandKind::SetHalfShroud:
    {
        if (state.mHasExitRequested)
            return std::nullopt;
        const bool isEnabled = command.mValue != 0;
        if (isEnabled == state.mIsHalfShroudEnabled)
            return std::nullopt;
        action.mKind = ViewerActionKind::SetHalfShroud;
        action.mIsHalfShroudEnabled = isEnabled;
        return action;
    }
    case viewer::ViewerCommandKind::ExitPlayback:
        if (state.mHasExitRequested)
            return std::nullopt;
        action.mKind = ViewerActionKind::ExitPlayback;
        return action;
    case viewer::ViewerCommandKind::SetHudScale:
    {
        const auto setting = viewer::ToHudScaleSetting(command.mValue);
        if (!setting || *setting == state.mHudScale)
            return std::nullopt;
        action.mKind = ViewerActionKind::SetHudScale;
        action.mHudScale = *setting;
        return action;
    }
    case viewer::ViewerCommandKind::CenterOnRadarEvent:
        if (state.mHasExitRequested)
            return std::nullopt;
        action.mKind = ViewerActionKind::CenterOnRadarEvent;
        // 命令值是 int；事件编号在界面侧收窄后经此还原，沿用原有的转换。
        action.mRadarEventId = static_cast<std::uint64_t>(command.mValue);
        return action;
    case viewer::ViewerCommandKind::AdjustBattlefieldZoom:
    case viewer::ViewerCommandKind::AdjustBattlefieldZoomAtCenter:
        if (state.mHasExitRequested || command.mValue == 0)
            return std::nullopt;
        action.mKind = ViewerActionKind::AdjustBattlefieldZoom;
        action.mZoomSteps = std::clamp(command.mValue, -MAX_BATTLEFIELD_ZOOM_STEPS, MAX_BATTLEFIELD_ZOOM_STEPS);
        action.mIsZoomAnchoredAtPointer = command.mKind == viewer::ViewerCommandKind::AdjustBattlefieldZoom;
        return action;
    case viewer::ViewerCommandKind::ResetBattlefieldZoom:
        if (state.mHasExitRequested)
            return std::nullopt;
        action.mKind = ViewerActionKind::ResetBattlefieldZoom;
        return action;
    case viewer::ViewerCommandKind::SeekToFrame:
    {
        if (state.mHasExitRequested || !state.mTotalFrames || *state.mTotalFrames == 0 || command.mValue < 0)
            return std::nullopt;
        // 目标与 mFrameNumber 同一单位：停在第 L 帧完成之后，最后一帧是总帧数 − 1。
        const std::uint32_t target = std::min(static_cast<std::uint32_t>(command.mValue), *state.mTotalFrames - 1);
        if (target <= state.mLastCompletedFrame || (state.mIsSeeking && target <= state.mSeekTargetFrame))
            return std::nullopt;
        action.mKind = ViewerActionKind::SeekToFrame;
        action.mSeekTargetFrame = target;
        action.mIsSeekRetarget = state.mIsSeeking;
        return action;
    }
    case viewer::ViewerCommandKind::CancelSeek:
        if (state.mHasExitRequested || !state.mIsSeeking)
            return std::nullopt;
        action.mKind = ViewerActionKind::CancelSeek;
        return action;
    }
    return std::nullopt;
}
}
