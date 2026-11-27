#include "../src/plugin/ViewerCommandRouter.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using rareplay::plugin::RouteViewerCommand;
using rareplay::plugin::ViewerAction;
using rareplay::plugin::ViewerActionKind;
using rareplay::plugin::ViewerRouterState;
using rareplay::viewer::HudScaleSetting;
using rareplay::viewer::ViewerCommand;
using rareplay::viewer::ViewerCommandKind;

constexpr std::uint64_t SESSION_ID = 7;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Require(bool condition, const std::string& message)
{
    Require(condition, message.c_str());
}

ViewerCommand MakeCommand(ViewerCommandKind kind, int value, std::uint64_t sessionId = SESSION_ID)
{
    ViewerCommand command;
    command.mSessionId = sessionId;
    command.mKind = kind;
    command.mValue = value;
    return command;
}

ViewerRouterState MakeState(bool hasExitRequested = false)
{
    ViewerRouterState state;
    state.mSessionId = SESSION_ID;
    state.mHasExitRequested = hasExitRequested;
    return state;
}

constexpr std::uint32_t TOTAL_FRAMES = 1'000;

// 总帧数已知、停在第 lastCompleted 帧完成之后；seekTarget 有值时表示正在快进到该帧。
ViewerRouterState MakeSeekState(std::uint32_t lastCompleted, std::optional<std::uint32_t> seekTarget = std::nullopt,
                                bool hasExitRequested = false)
{
    ViewerRouterState state = MakeState(hasExitRequested);
    state.mLastCompletedFrame = lastCompleted;
    state.mTotalFrames = TOTAL_FRAMES;
    state.mIsSeeking = seekTarget.has_value();
    state.mSeekTargetFrame = seekTarget.value_or(0);
    return state;
}

// 只模拟 ReplayRuntime::ExecuteViewerAction 对路由状态的影响，以及各动作是否被执行。
struct SimulatedRuntime
{
    ViewerRouterState mState = MakeState();
    bool mDoesRestartSucceed = true;
    int mRestartRequests = 0;
    int mEndReplayCalls = 0;
    int mShroudInvalidations = 0;
    int mSettingsSaves = 0;
    int mSeekEnds = 0;
    std::vector<ViewerActionKind> mExecuted;
    std::vector<int> mZoomSteps;
    std::vector<bool> mZoomAnchoredAtPointer;
    std::vector<std::uint32_t> mSeekTargets;
    std::vector<bool> mSeekRetargets;
    // 进程内重开的前提是否满足（执行方按它决定记下请求还是拒绝），以及记下的请求的目标与暂停状态。
    bool mCanRestartInProcess = true;
    std::vector<std::uint32_t> mRestartTargets;
    std::vector<bool> mRestartPausedAfter;
    int mRestartCancels = 0;

    // 退出与重新播放由执行方先结束快进（这是运行时的职责），这里同样模拟。
    void EndSeek()
    {
        if (!mState.mIsSeeking)
            return;
        mState.mIsSeeking = false;
        ++mSeekEnds;
    }

    void CancelRestart()
    {
        if (!mState.mIsRestartPending)
            return;
        mState.mIsRestartPending = false;
        mState.mPendingRestartTargetFrame.reset();
        ++mRestartCancels;
    }

    void Execute(const ViewerAction& action)
    {
        mExecuted.push_back(action.mKind);
        switch (action.mKind)
        {
        case ViewerActionKind::RequestRestart:
            ++mRestartRequests;
            if (mDoesRestartSucceed)
            {
                EndSeek();
                mState.mHasExitRequested = true;
                ++mEndReplayCalls;
            }
            break;
        case ViewerActionKind::SetHalfShroud:
            mState.mIsHalfShroudEnabled = action.mIsHalfShroudEnabled;
            ++mShroudInvalidations;
            break;
        case ViewerActionKind::ExitPlayback:
            CancelRestart();
            EndSeek();
            mState.mHasExitRequested = true;
            ++mEndReplayCalls;
            break;
        case ViewerActionKind::SeekToFrame:
            mSeekTargets.push_back(action.mSeekTargetFrame);
            mSeekRetargets.push_back(action.mIsSeekRetarget);
            mState.mIsSeeking = true;
            mState.mSeekTargetFrame = action.mSeekTargetFrame;
            break;
        case ViewerActionKind::SeekViaRestart:
            // 记下请求不改变会话与快进（执行点才结束快进）；前提不满足时执行方拒绝。
            if (!mCanRestartInProcess)
                break;
            mRestartTargets.push_back(action.mSeekTargetFrame);
            mRestartPausedAfter.push_back(action.mIsPausedAfterSeek);
            mState.mIsRestartPending = true;
            mState.mPendingRestartTargetFrame = action.mSeekTargetFrame;
            break;
        case ViewerActionKind::CancelSeek:
            CancelRestart();
            EndSeek();
            break;
        case ViewerActionKind::SetHudScale:
            mState.mHudScale = action.mHudScale;
            ++mSettingsSaves;
            break;
        case ViewerActionKind::AdjustBattlefieldZoom:
            mZoomSteps.push_back(action.mZoomSteps);
            mZoomAnchoredAtPointer.push_back(action.mIsZoomAnchoredAtPointer);
            break;
        default:
            break;
        }
    }

    // 与运行时相同：逐条按当前状态路由，执行后再路由下一条。
    void Consume(const std::vector<ViewerCommand>& commands)
    {
        for (const auto& command : commands)
        {
            if (const auto action = RouteViewerCommand(command, mState))
                Execute(*action);
        }
    }
};

void CheckSessionFilter()
{
    const auto state = MakeState();
    for (const auto kind : {ViewerCommandKind::SetPlaybackSpeed, ViewerCommandKind::SetPaused,
             ViewerCommandKind::ExitPlayback, ViewerCommandKind::RestartPlayback, ViewerCommandKind::SetHalfShroud,
             ViewerCommandKind::SetHudScale, ViewerCommandKind::CenterOnRadarEvent,
             ViewerCommandKind::AdjustBattlefieldZoom, ViewerCommandKind::ResetBattlefieldZoom,
             ViewerCommandKind::AdjustBattlefieldZoomAtCenter})
    {
        Require(!RouteViewerCommand(MakeCommand(kind, 125, SESSION_ID + 1), state),
            "Commands from another session are dropped");
        Require(!RouteViewerCommand(MakeCommand(kind, 125, 0), state), "Commands without a session are dropped");
        Require(RouteViewerCommand(MakeCommand(kind, 125), state).has_value(),
            "Commands of the current session are routed");
    }
    // 只比较相等：发布器未开始（会话 0）时，会话 0 的命令照常路由，与拆分前一致。
    ViewerRouterState idle;
    Require(RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPaused, 1, 0), idle).has_value(),
        "Session filter is a plain equality check");

    // 两种跳转命令需要总帧数与快进状态才会路由，单独用能路由的状态核对会话过滤。
    const auto seekable = MakeSeekState(10);
    const auto seeking = MakeSeekState(10, 500);
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 125, SESSION_ID + 1), seekable) &&
                !RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 125, 0), seekable) &&
                RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 125), seekable).has_value(),
        "Seek commands are filtered by session");
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, 0, SESSION_ID + 1), seeking) &&
                !RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, 0, 0), seeking) &&
                RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, 0), seeking).has_value(),
        "Cancel commands are filtered by session");
}

void CheckEachKindAroundExit()
{
    for (const bool hasExitRequested : {false, true})
    {
        const auto state = MakeState(hasExitRequested);

        auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPlaybackSpeed, 3), state);
        Require(action && action->mKind == ViewerActionKind::SetPlaybackSpeed && action->mPlaybackSpeedSetting == 3,
            "Speed is routed with its value regardless of exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPlaybackSpeed, -5), state);
        Require(action && action->mPlaybackSpeedSetting == -5, "Speed value is passed through unchecked");

        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPaused, 1), state);
        Require(action && action->mKind == ViewerActionKind::SetPaused && action->mIsPaused,
            "Pause is routed regardless of exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPaused, 0), state);
        Require(action && !action->mIsPaused, "Zero resumes playback");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPaused, -2), state);
        Require(action && action->mIsPaused, "Any non-zero value pauses");

        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, 150), state);
        Require(action && action->mKind == ViewerActionKind::SetHudScale &&
                action->mHudScale == HudScaleSetting::Percent150,
            "HUD scale is routed regardless of exit");

        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::RestartPlayback, 0), state);
        Require(action.has_value() != hasExitRequested, "Restart is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHalfShroud, 1), state);
        Require(action.has_value() != hasExitRequested, "Half shroud is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::ExitPlayback, 0), state);
        Require(action.has_value() != hasExitRequested, "Exit is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::CenterOnRadarEvent, 4), state);
        Require(action.has_value() != hasExitRequested, "Radar jump is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 1), state);
        Require(action.has_value() != hasExitRequested, "Battlefield zoom step is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::AdjustBattlefieldZoomAtCenter, -1), state);
        Require(action.has_value() != hasExitRequested, "Centered battlefield zoom step is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::ResetBattlefieldZoom, 0), state);
        Require(action.has_value() != hasExitRequested, "Battlefield zoom reset is blocked only after exit");

        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 300),
            MakeSeekState(100, std::nullopt, hasExitRequested));
        Require(action.has_value() != hasExitRequested, "Seek is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 800),
            MakeSeekState(100, 300, hasExitRequested));
        Require(action.has_value() != hasExitRequested, "Retarget is blocked only after exit");
        action = RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, 0),
            MakeSeekState(100, 300, hasExitRequested));
        Require(action.has_value() != hasExitRequested, "Cancel is blocked only after exit");
    }

    auto state = MakeState();
    auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::ExitPlayback, 0), state);
    Require(action && action->mKind == ViewerActionKind::ExitPlayback, "Exit is routed as exit");

    state.mIsHalfShroudEnabled = true;
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::RestartPlayback, 0), state);
    Require(action && action->mKind == ViewerActionKind::RequestRestart && action->mIsHalfShroudEnabled,
        "Restart carries the current half shroud flag");
    state.mIsHalfShroudEnabled = false;
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::RestartPlayback, 1), state);
    Require(action && !action->mIsHalfShroudEnabled, "Restart ignores the command value");
}

void CheckHalfShroudChangeOnly()
{
    auto state = MakeState();
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHalfShroud, 0), state),
        "Disabling a disabled half shroud does nothing");
    auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHalfShroud, 5), state);
    Require(action && action->mKind == ViewerActionKind::SetHalfShroud && action->mIsHalfShroudEnabled,
        "Any non-zero value enables the half shroud");

    state.mIsHalfShroudEnabled = true;
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHalfShroud, 1), state),
        "Enabling an enabled half shroud does nothing");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHalfShroud, 0), state);
    Require(action && !action->mIsHalfShroudEnabled, "Zero disables the half shroud");
}

void CheckHudScale()
{
    auto state = MakeState();
    for (const int invalid : {-1, 1, 99, 101, 120, 250, 300, INT_MIN, INT_MAX})
        Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, invalid), state),
            "Undefined HUD scale values are ignored");
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, 100), state),
        "Same HUD scale is ignored");

    auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, 0), state);
    Require(action && action->mHudScale == HudScaleSetting::Auto, "Auto is a valid HUD scale");
    state.mHudScale = HudScaleSetting::Auto;
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, 0), state),
        "Auto again is ignored");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, 200), state);
    Require(action && action->mHudScale == HudScaleSetting::Percent200, "Switching from auto is routed");
}

void CheckRadarEventId()
{
    const auto state = MakeState();
    auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::CenterOnRadarEvent, 42), state);
    Require(action && action->mKind == ViewerActionKind::CenterOnRadarEvent && action->mRadarEventId == 42,
        "Radar event id is converted");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::CenterOnRadarEvent, 0), state);
    Require(action && action->mRadarEventId == 0, "Radar event zero is routed");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::CenterOnRadarEvent, INT_MAX), state);
    Require(action && action->mRadarEventId == static_cast<std::uint64_t>(INT_MAX), "Largest int id is kept");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::CenterOnRadarEvent, -1), state);
    Require(action && action->mRadarEventId == UINT64_MAX, "Negative values convert like static_cast");
}

void CheckBattlefieldZoom()
{
    const auto state = MakeState();
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 0), state),
        "A zero-step zoom command is dropped");
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::AdjustBattlefieldZoomAtCenter, 0), state),
        "A zero-step centered zoom command is dropped");
    struct StepCase
    {
        int mValue;
        int mExpected;
    };
    const StepCase cases[] = {{1, 1}, {-1, -1}, {3, 3}, {-7, -7}, {12, 12}, {-12, -12}, {13, 12}, {-13, -12},
        {INT_MAX, 12}, {INT_MIN, -12}};
    // 两种换档命令路由为同一动作，只有锚点不同：滚轮以鼠标为锚点，HUD 按钮以战术区中心为锚点。
    for (const auto kind : {ViewerCommandKind::AdjustBattlefieldZoom, ViewerCommandKind::AdjustBattlefieldZoomAtCenter})
    {
        const bool isPointer = kind == ViewerCommandKind::AdjustBattlefieldZoom;
        for (const StepCase& stepCase : cases)
        {
            const auto action = RouteViewerCommand(MakeCommand(kind, stepCase.mValue), state);
            Require(action && action->mKind == ViewerActionKind::AdjustBattlefieldZoom &&
                    action->mZoomSteps == stepCase.mExpected && action->mIsZoomAnchoredAtPointer == isPointer,
                std::string(isPointer ? "Pointer" : "Centered") + " zoom steps are clamped to [-12, 12] with the right anchor: " +
                    std::to_string(stepCase.mValue));
        }
    }
    for (const int value : {0, 1, -5, INT_MIN})
    {
        const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::ResetBattlefieldZoom, value), state);
        Require(action && action->mKind == ViewerActionKind::ResetBattlefieldZoom, "Zoom reset ignores the command value");
    }
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 2, SESSION_ID + 1), state) &&
                !RouteViewerCommand(MakeCommand(ViewerCommandKind::AdjustBattlefieldZoomAtCenter, 2, SESSION_ID + 1), state) &&
                !RouteViewerCommand(MakeCommand(ViewerCommandKind::ResetBattlefieldZoom, 0, SESSION_ID - 1), state),
        "Zoom commands from another session are dropped");
}

// 不在快进中：总帧数未知时拦下；目标须大于最后完成帧；超过最后一帧的按最后一帧。
void CheckSeekToFrame()
{
    auto unknown = MakeState();
    unknown.mLastCompletedFrame = 10;
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 500), unknown),
        "Seek is blocked while the total frame count is unknown");
    auto empty = MakeSeekState(0);
    empty.mTotalFrames = 0;
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 0), empty) &&
                !RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 5), empty),
        "Seek is blocked in an empty replay");

    const auto state = MakeSeekState(100);
    for (const int value : {-1, -500, INT_MIN, 100})
        Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, value), state),
            "Negative targets and the last completed frame are dropped: " + std::to_string(value));
    for (const int value : {0, 50, 99})
    {
        const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, value), state);
        Require(action && action->mKind == ViewerActionKind::SeekViaRestart &&
                    action->mSeekTargetFrame == static_cast<std::uint32_t>(value),
            "Targets before the last completed frame go through a restart: " + std::to_string(value));
    }
    struct TargetCase
    {
        int mValue;
        std::uint32_t mExpected;
    };
    const TargetCase cases[] = {{101, 101}, {500, 500}, {998, 998}, {999, 999}, {1'000, 999}, {5'000, 999},
        {INT_MAX, 999}};
    for (const TargetCase& targetCase : cases)
    {
        const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, targetCase.mValue), state);
        Require(action && action->mKind == ViewerActionKind::SeekToFrame && !action->mIsSeekRetarget &&
                    action->mSeekTargetFrame == targetCase.mExpected,
            "Seek starts towards the clamped target: " + std::to_string(targetCase.mValue));
    }

    // 已在最后一帧：任何目标夹到最后一帧后都不在当前位置之后，只剩向后跳转。
    const auto atEnd = MakeSeekState(TOTAL_FRAMES - 1);
    for (const int value : {999, 1'000, INT_MAX})
        Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, value), atEnd),
            "No forward seek from the last frame: " + std::to_string(value));
    for (const int value : {0, 998})
        Require(RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, value), atEnd)->mKind ==
                    ViewerActionKind::SeekViaRestart,
            "From the last frame earlier targets go back: " + std::to_string(value));
    // 只有一帧的回放没有可跳转的位置。
    auto single = MakeSeekState(0);
    single.mTotalFrames = 1;
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 3), single), "No seek in a one-frame replay");
}

// 快进中：比当前目标更靠后的目标改目标；当前位置之前的经重开向后跳转；当前位置到目标之间（含两端）丢弃。
void CheckSeekWhileSeeking()
{
    const auto state = MakeSeekState(200, 600);
    for (const int value : {-1, 200, 201, 400, 599, 600})
        Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, value), state),
            "Targets from the position to the current target are dropped while seeking: " + std::to_string(value));
    auto backward = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 100), state);
    Require(backward && backward->mKind == ViewerActionKind::SeekViaRestart && backward->mSeekTargetFrame == 100,
        "A target before the position goes back while seeking");
    auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 601), state);
    Require(action && action->mKind == ViewerActionKind::SeekToFrame && action->mIsSeekRetarget &&
                action->mSeekTargetFrame == 601,
        "A later target retargets the seek");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 50'000), state);
    Require(action && action->mIsSeekRetarget && action->mSeekTargetFrame == 999, "Retarget is clamped to the last frame");

    const auto towardsEnd = MakeSeekState(200, TOTAL_FRAMES - 1);
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 50'000), towardsEnd),
        "Already seeking to the last frame: a clamped target is not later");

    // 总帧数在快进中不会变为未知；即使出现也照样拦下。
    auto unknown = state;
    unknown.mTotalFrames.reset();
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 900), unknown),
        "Retarget also needs the total frame count");
}

// 进程内重开已请求、尚未执行；seekTarget 有值时请求是在快进中发出的（快进仍在进行，等执行点结束）。
ViewerRouterState MakePendingRestartState(std::uint32_t lastCompleted, std::optional<std::uint32_t> pendingTarget,
                                          std::optional<std::uint32_t> seekTarget = std::nullopt)
{
    ViewerRouterState state = MakeSeekState(lastCompleted, seekTarget);
    state.mIsRestartPending = true;
    state.mPendingRestartTargetFrame = pendingTarget;
    return state;
}

void CheckCancelSeek()
{
    for (const int value : {0, 1, -7})
    {
        Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, value), MakeSeekState(100)),
            "Cancel without a seek is dropped");
        Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, value), MakeState()),
            "Cancel with an unknown total is dropped");
        const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, value), MakeSeekState(100, 300));
        Require(action && action->mKind == ViewerActionKind::CancelSeek, "Cancel while seeking ignores the value");
        const auto pending = RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, value),
            MakePendingRestartState(500, 40));
        Require(pending && pending->mKind == ViewerActionKind::CancelSeek,
            "Cancel revokes a pending restart even without a seek");
    }
    auto exited = MakePendingRestartState(500, 40);
    exited.mHasExitRequested = true;
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::CancelSeek, 0), exited), "Cancel is blocked after exit");
}

// 向后跳转：目标在最后完成帧之前（含 T=0）经进程内重开；跳转前的暂停状态快进中取快进开始前保存的值。
void CheckSeekViaRestart()
{
    for (const bool isPaused : {false, true})
    {
        auto state = MakeSeekState(500);
        state.mIsPaused = isPaused;
        for (const int value : {0, 1, 250, 499})
        {
            const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, value), state);
            Require(action && action->mKind == ViewerActionKind::SeekViaRestart &&
                        action->mSeekTargetFrame == static_cast<std::uint32_t>(value) &&
                        action->mIsPausedAfterSeek == isPaused && !action->mIsSeekRetarget,
                "A backward target carries the current pause state: " + std::to_string(value));
        }
        Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 500), state),
            "The last completed frame itself is still dropped");
        const auto forward = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 600), state);
        Require(forward && forward->mKind == ViewerActionKind::SeekToFrame && forward->mSeekTargetFrame == 600,
            "Later targets still seek forward");
    }

    // 快进中会话是播放状态：暂停状态取快进开始前保存的，而不是当前的。
    for (const bool wasPaused : {false, true})
    {
        auto seeking = MakeSeekState(300, 800);
        seeking.mIsPaused = !wasPaused;
        seeking.mSeekWasPaused = wasPaused;
        const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 120), seeking);
        Require(action && action->mKind == ViewerActionKind::SeekViaRestart && action->mSeekTargetFrame == 120 &&
                    action->mIsPausedAfterSeek == wasPaused,
            "A backward seek during a seek keeps the pause state saved before the seek");
        const auto zero = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 0), seeking);
        Require(zero && zero->mKind == ViewerActionKind::SeekViaRestart && zero->mSeekTargetFrame == 0 &&
                    zero->mIsPausedAfterSeek == wasPaused,
            "T=0 during a seek is a backward seek too");
    }

    // 总帧数未知或已请求退出时与向前跳转一样拦下；负值丢弃。
    auto unknown = MakeState();
    unknown.mLastCompletedFrame = 500;
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 10), unknown),
        "A backward seek needs the total frame count");
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 10), MakeSeekState(500, std::nullopt, true)),
        "A backward seek is blocked after exit");
}

// 重开已请求、尚未执行：任何不同于待执行目标的新目标都改写它（包括当前位置之后的），相同的丢弃。
void CheckSeekWhileRestartPending()
{
    const auto pending = MakePendingRestartState(500, 40);
    for (const int value : {0, 39, 41, 499, 500, 501, 5'000})
    {
        const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, value), pending);
        const std::uint32_t expected = std::min<std::uint32_t>(static_cast<std::uint32_t>(value), TOTAL_FRAMES - 1);
        Require(action && action->mKind == ViewerActionKind::SeekViaRestart && action->mSeekTargetFrame == expected,
            "A new target rewrites the pending restart: " + std::to_string(value));
    }
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 40), pending),
        "The pending target itself is dropped");
    Require(!RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, -3), pending), "Negative values are dropped");

    // 待执行的是重新播放（没有目标）：第 0 帧也是新的目标（暂停状态可能不同）。
    const auto replay = MakePendingRestartState(500, std::nullopt);
    const auto zero = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 0), replay);
    Require(zero && zero->mKind == ViewerActionKind::SeekViaRestart && zero->mSeekTargetFrame == 0,
        "Frame 0 rewrites a pending replay");

    // 请求是在快进中发出的：暂停状态仍取快进开始前保存的。
    auto duringSeek = MakePendingRestartState(500, 40, 900);
    duringSeek.mSeekWasPaused = true;
    const auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SeekToFrame, 700), duringSeek);
    Require(action && action->mKind == ViewerActionKind::SeekViaRestart && action->mSeekTargetFrame == 700 &&
                action->mIsPausedAfterSeek,
        "A pending restart from a seek keeps the saved pause state");

    // 与快进中相同地拦下暂停、半黑幕、镜头跳转与缩放；调速、界面缩放、退出与重新播放照常。
    for (const auto kind : {ViewerCommandKind::SetPaused, ViewerCommandKind::SetHalfShroud,
             ViewerCommandKind::CenterOnRadarEvent, ViewerCommandKind::AdjustBattlefieldZoom,
             ViewerCommandKind::AdjustBattlefieldZoomAtCenter, ViewerCommandKind::ResetBattlefieldZoom})
        Require(!RouteViewerCommand(MakeCommand(kind, 1), pending),
            "Command " + std::to_string(static_cast<int>(kind)) + " is dropped while a restart is pending");
    Require(RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPlaybackSpeed, 2), pending)->mKind ==
                    ViewerActionKind::SetPlaybackSpeed &&
                RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, 150), pending)->mKind ==
                    ViewerActionKind::SetHudScale &&
                RouteViewerCommand(MakeCommand(ViewerCommandKind::ExitPlayback, 0), pending)->mKind ==
                    ViewerActionKind::ExitPlayback &&
                RouteViewerCommand(MakeCommand(ViewerCommandKind::RestartPlayback, 0), pending)->mKind ==
                    ViewerActionKind::RequestRestart,
        "Speed, HUD scale, exit and restart are routed while a restart is pending");
}

// 快进中丢弃暂停、半黑幕、镜头跳转与三种缩放命令；调速、界面缩放、退出与重新播放照常路由。
void CheckCommandsWhileSeeking()
{
    const auto seeking = MakeSeekState(100, 300);
    const auto idle = MakeSeekState(100);
    for (const auto kind : {ViewerCommandKind::SetPaused, ViewerCommandKind::SetHalfShroud,
             ViewerCommandKind::CenterOnRadarEvent, ViewerCommandKind::AdjustBattlefieldZoom,
             ViewerCommandKind::AdjustBattlefieldZoomAtCenter, ViewerCommandKind::ResetBattlefieldZoom})
    {
        for (const int value : {0, 1})
        {
            const auto command = MakeCommand(kind, kind == ViewerCommandKind::SetPaused ? value : 1);
            Require(!RouteViewerCommand(command, seeking),
                "Command " + std::to_string(static_cast<int>(kind)) + " is dropped while seeking");
            Require(RouteViewerCommand(command, idle).has_value(),
                "Command " + std::to_string(static_cast<int>(kind)) + " is routed when not seeking");
        }
    }
    auto action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetPlaybackSpeed, 2), seeking);
    Require(action && action->mKind == ViewerActionKind::SetPlaybackSpeed, "Speed is routed while seeking");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::SetHudScale, 150), seeking);
    Require(action && action->mKind == ViewerActionKind::SetHudScale, "HUD scale is routed while seeking");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::ExitPlayback, 0), seeking);
    Require(action && action->mKind == ViewerActionKind::ExitPlayback, "Exit is routed while seeking");
    action = RouteViewerCommand(MakeCommand(ViewerCommandKind::RestartPlayback, 0), seeking);
    Require(action && action->mKind == ViewerActionKind::RequestRestart, "Restart is routed while seeking");
}

void CheckSeekSequences()
{
    {
        // 开始、快进中的各种命令、改目标、取消，之后暂停恢复可用。
        SimulatedRuntime runtime;
        runtime.mState = MakeSeekState(100);
        runtime.Consume({MakeCommand(ViewerCommandKind::CancelSeek, 0), MakeCommand(ViewerCommandKind::SeekToFrame, 400),
            MakeCommand(ViewerCommandKind::SetPaused, 1), MakeCommand(ViewerCommandKind::SetHalfShroud, 1),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 1), MakeCommand(ViewerCommandKind::SetPlaybackSpeed, 2),
            MakeCommand(ViewerCommandKind::SeekToFrame, 300), MakeCommand(ViewerCommandKind::SeekToFrame, 400),
            MakeCommand(ViewerCommandKind::SeekToFrame, 700), MakeCommand(ViewerCommandKind::CancelSeek, 0),
            MakeCommand(ViewerCommandKind::CancelSeek, 0), MakeCommand(ViewerCommandKind::SetPaused, 1)});
        const std::vector<ViewerActionKind> expected{ViewerActionKind::SeekToFrame, ViewerActionKind::SetPlaybackSpeed,
            ViewerActionKind::SeekToFrame, ViewerActionKind::CancelSeek, ViewerActionKind::SetPaused};
        Require(runtime.mExecuted == expected, "Seek, retarget and cancel apply in order");
        Require(runtime.mSeekTargets == std::vector<std::uint32_t>({400, 700}) &&
                    runtime.mSeekRetargets == std::vector<bool>({false, true}),
            "The first seek starts and the later target retargets");
        Require(runtime.mSeekEnds == 1 && !runtime.mState.mIsSeeking, "One cancel ends the seek");
    }
    {
        // 快进中退出：执行方先结束快进；之后跳转与取消都被拦下。
        SimulatedRuntime runtime;
        runtime.mState = MakeSeekState(100);
        runtime.Consume({MakeCommand(ViewerCommandKind::SeekToFrame, 400), MakeCommand(ViewerCommandKind::ExitPlayback, 0),
            MakeCommand(ViewerCommandKind::CancelSeek, 0), MakeCommand(ViewerCommandKind::SeekToFrame, 900),
            MakeCommand(ViewerCommandKind::SetPaused, 1)});
        const std::vector<ViewerActionKind> expected{ViewerActionKind::SeekToFrame, ViewerActionKind::ExitPlayback,
            ViewerActionKind::SetPaused};
        Require(runtime.mExecuted == expected, "After exit during a seek only pause still applies");
        Require(runtime.mSeekEnds == 1 && runtime.mEndReplayCalls == 1, "Exit ends the seek and playback once");
    }
    {
        // 快进中重新播放成功：同样先结束快进。
        SimulatedRuntime runtime;
        runtime.mState = MakeSeekState(100);
        runtime.Consume({MakeCommand(ViewerCommandKind::SeekToFrame, 400),
            MakeCommand(ViewerCommandKind::RestartPlayback, 0), MakeCommand(ViewerCommandKind::SeekToFrame, 900)});
        Require(runtime.mRestartRequests == 1 && runtime.mSeekEnds == 1 && runtime.mSeekTargets.size() == 1,
            "A successful restart ends the seek and blocks later seeks");
    }
}

void CheckBackwardSeekSequences()
{
    {
        // 暂停时向后跳转，执行前再点一次：后一次改写目标，暂停状态都是跳转前的；期间暂停被拦下，取消撤销请求。
        SimulatedRuntime runtime;
        runtime.mState = MakeSeekState(600);
        runtime.mState.mIsPaused = true;
        runtime.Consume({MakeCommand(ViewerCommandKind::SeekToFrame, 200), MakeCommand(ViewerCommandKind::SetPaused, 0),
            MakeCommand(ViewerCommandKind::SeekToFrame, 200), MakeCommand(ViewerCommandKind::SeekToFrame, 50),
            MakeCommand(ViewerCommandKind::CancelSeek, 0), MakeCommand(ViewerCommandKind::CancelSeek, 0),
            MakeCommand(ViewerCommandKind::SetPaused, 0)});
        const std::vector<ViewerActionKind> expected{ViewerActionKind::SeekViaRestart, ViewerActionKind::SeekViaRestart,
            ViewerActionKind::CancelSeek, ViewerActionKind::SetPaused};
        Require(runtime.mExecuted == expected, "Backward seek, rewrite, cancel and then pause apply in order");
        Require(runtime.mRestartTargets == std::vector<std::uint32_t>({200, 50}) &&
                    runtime.mRestartPausedAfter == std::vector<bool>({true, true}),
            "The rewrite keeps the pause state from before the jump");
        Require(runtime.mRestartCancels == 1 && !runtime.mState.mIsRestartPending, "One cancel revokes the request");
    }
    {
        // 快进中再次向后：暂停状态取快进开始前保存的；之后退出撤销请求并结束快进。
        SimulatedRuntime runtime;
        runtime.mState = MakeSeekState(100);
        runtime.mState.mIsPaused = true;
        runtime.Consume({MakeCommand(ViewerCommandKind::SeekToFrame, 700)});
        // 执行方开始快进时解除暂停，并保存跳转前的暂停状态。
        runtime.mState.mIsPaused = false;
        runtime.mState.mSeekWasPaused = true;
        runtime.mState.mLastCompletedFrame = 400;
        runtime.Consume({MakeCommand(ViewerCommandKind::SeekToFrame, 300), MakeCommand(ViewerCommandKind::SeekToFrame, 900),
            MakeCommand(ViewerCommandKind::ExitPlayback, 0), MakeCommand(ViewerCommandKind::SeekToFrame, 10)});
        const std::vector<ViewerActionKind> expected{ViewerActionKind::SeekToFrame, ViewerActionKind::SeekViaRestart,
            ViewerActionKind::SeekViaRestart, ViewerActionKind::ExitPlayback};
        Require(runtime.mExecuted == expected, "Backward during a seek, a rewrite, then exit");
        Require(runtime.mRestartTargets == std::vector<std::uint32_t>({300, 900}) &&
                    runtime.mRestartPausedAfter == std::vector<bool>({true, true}),
            "Both requests carry the pause state saved before the seek");
        Require(runtime.mRestartCancels == 1 && runtime.mSeekEnds == 1 && runtime.mEndReplayCalls == 1,
            "Exit revokes the pending restart and ends the seek");
    }
    {
        // 前提不满足（原生入口未实现）：向后跳转被执行方拒绝，回放状态不变，向前跳转照常。
        SimulatedRuntime runtime;
        runtime.mCanRestartInProcess = false;
        runtime.mState = MakeSeekState(500);
        runtime.Consume({MakeCommand(ViewerCommandKind::SeekToFrame, 100), MakeCommand(ViewerCommandKind::SeekToFrame, 800)});
        Require(runtime.mRestartTargets.empty() && !runtime.mState.mIsRestartPending &&
                    runtime.mSeekTargets == std::vector<std::uint32_t>({800}),
            "A rejected backward seek leaves the replay as it was");
    }
}

void CheckSequences()
{
    {
        SimulatedRuntime runtime;
        runtime.Consume({MakeCommand(ViewerCommandKind::ExitPlayback, 0),
            MakeCommand(ViewerCommandKind::RestartPlayback, 0)});
        Require(runtime.mRestartRequests == 0 && runtime.mEndReplayCalls == 1,
            "Restart after exit in the same batch is blocked");
    }
    {
        SimulatedRuntime runtime;
        runtime.Consume({MakeCommand(ViewerCommandKind::RestartPlayback, 0),
            MakeCommand(ViewerCommandKind::SetHalfShroud, 1), MakeCommand(ViewerCommandKind::ExitPlayback, 0)});
        Require(runtime.mRestartRequests == 1 && runtime.mEndReplayCalls == 1,
            "Successful restart ends playback once");
        Require(runtime.mShroudInvalidations == 0 && !runtime.mState.mIsHalfShroudEnabled,
            "Half shroud after a successful restart is blocked");
        Require(runtime.mExecuted.size() == 1, "Exit after a successful restart is blocked");
    }
    {
        SimulatedRuntime runtime;
        runtime.mDoesRestartSucceed = false;
        runtime.Consume({MakeCommand(ViewerCommandKind::RestartPlayback, 0),
            MakeCommand(ViewerCommandKind::SetHalfShroud, 1), MakeCommand(ViewerCommandKind::RestartPlayback, 0),
            MakeCommand(ViewerCommandKind::ExitPlayback, 0)});
        Require(runtime.mRestartRequests == 2, "Failed restart can be retried");
        Require(runtime.mShroudInvalidations == 1 && runtime.mState.mIsHalfShroudEnabled,
            "Half shroud after a failed restart applies");
        Require(runtime.mEndReplayCalls == 1 && runtime.mState.mHasExitRequested,
            "Exit after a failed restart applies");
    }
    {
        SimulatedRuntime runtime;
        runtime.mDoesRestartSucceed = false;
        runtime.Consume({MakeCommand(ViewerCommandKind::SetHalfShroud, 1),
            MakeCommand(ViewerCommandKind::RestartPlayback, 0)});
        Require(runtime.mRestartRequests == 1, "Restart follows the half shroud change");
        Require(runtime.mExecuted.size() == 2 && runtime.mExecuted[1] == ViewerActionKind::RequestRestart,
            "Restart is routed after the half shroud change");
        Require(RouteViewerCommand(MakeCommand(ViewerCommandKind::RestartPlayback, 0), runtime.mState)
                    ->mIsHalfShroudEnabled,
            "Restart sees the half shroud enabled earlier in the batch");
    }
    {
        SimulatedRuntime runtime;
        runtime.Consume({MakeCommand(ViewerCommandKind::SetHalfShroud, 1),
            MakeCommand(ViewerCommandKind::SetHalfShroud, 1), MakeCommand(ViewerCommandKind::SetHudScale, 150),
            MakeCommand(ViewerCommandKind::SetHudScale, 150)});
        Require(runtime.mShroudInvalidations == 1, "Repeated half shroud value applies once");
        Require(runtime.mSettingsSaves == 1 && runtime.mState.mHudScale == HudScaleSetting::Percent150,
            "Repeated HUD scale applies once");
    }
    {
        SimulatedRuntime runtime;
        runtime.Consume({MakeCommand(ViewerCommandKind::ExitPlayback, 0),
            MakeCommand(ViewerCommandKind::SetPaused, 1), MakeCommand(ViewerCommandKind::SetPlaybackSpeed, 2),
            MakeCommand(ViewerCommandKind::SetHudScale, 125), MakeCommand(ViewerCommandKind::CenterOnRadarEvent, 3),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 1),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoomAtCenter, 1),
            MakeCommand(ViewerCommandKind::ResetBattlefieldZoom, 0),
            MakeCommand(ViewerCommandKind::SetHalfShroud, 1), MakeCommand(ViewerCommandKind::ExitPlayback, 0)});
        const std::vector<ViewerActionKind> expected{ViewerActionKind::ExitPlayback, ViewerActionKind::SetPaused,
            ViewerActionKind::SetPlaybackSpeed, ViewerActionKind::SetHudScale};
        Require(runtime.mExecuted == expected, "After exit only pause, speed and HUD scale still apply");
        Require(runtime.mEndReplayCalls == 1, "Exit ends playback once");
    }
    {
        SimulatedRuntime runtime;
        runtime.Consume({MakeCommand(ViewerCommandKind::ExitPlayback, 0, SESSION_ID + 1),
            MakeCommand(ViewerCommandKind::RestartPlayback, 0)});
        Require(runtime.mEndReplayCalls == 1 && runtime.mRestartRequests == 1,
            "A stale exit does not block the current session's restart");
    }
    {
        // Ctrl+滚轮与按钮交替：每条换档都按自己的档数执行，复原夹在中间也照常执行，档数为 0 的被丢弃。
        SimulatedRuntime runtime;
        runtime.Consume({MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 2),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 0), MakeCommand(ViewerCommandKind::ResetBattlefieldZoom, 0),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoomAtCenter, -40),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoomAtCenter, 0), MakeCommand(ViewerCommandKind::ExitPlayback, 0),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoom, 1),
            MakeCommand(ViewerCommandKind::AdjustBattlefieldZoomAtCenter, 1)});
        const std::vector<ViewerActionKind> expected{ViewerActionKind::AdjustBattlefieldZoom,
            ViewerActionKind::ResetBattlefieldZoom, ViewerActionKind::AdjustBattlefieldZoom, ViewerActionKind::ExitPlayback};
        Require(runtime.mExecuted == expected, "Zoom commands apply in order until exit");
        Require(runtime.mZoomSteps == std::vector<int>({2, -12}), "Zoom steps reach the runtime clamped");
        Require(runtime.mZoomAnchoredAtPointer == std::vector<bool>({true, false}),
            "The wheel command anchors at the pointer and the button command at the center");
    }
}
}

int main()
{
    try
    {
        CheckSessionFilter();
        CheckEachKindAroundExit();
        CheckHalfShroudChangeOnly();
        CheckHudScale();
        CheckRadarEventId();
        CheckBattlefieldZoom();
        CheckSeekToFrame();
        CheckSeekWhileSeeking();
        CheckCancelSeek();
        CheckSeekViaRestart();
        CheckSeekWhileRestartPending();
        CheckCommandsWhileSeeking();
        CheckSequences();
        CheckSeekSequences();
        CheckBackwardSeekSequences();
        std::cout << "Viewer command router tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
