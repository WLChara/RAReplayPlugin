#include "RuntimeGates.h"

#include "../replay/FrameCodec.h"

namespace rareplay::plugin
{
namespace
{
bool IsViewableState(replay::ReplayState state) noexcept
{
    return state == replay::ReplayState::Playing || state == replay::ReplayState::Paused ||
        state == replay::ReplayState::Ended;
}
}

bool IsPlaybackViewActive(const RuntimeGateState& state) noexcept
{
    return state.mIsStarted && state.mIsPlayback && !state.mHasExitRequested && !state.mHasViewFailed &&
        IsViewableState(state.mReplayState);
}

bool ShouldUseHalfShroud(const RuntimeGateState& state) noexcept
{
    // 既有语义：半黑幕不随界面失败关闭，只由退出请求与会话状态决定；与 IsPlaybackViewActive 不同。
    return state.mIsStarted && state.mIsPlayback && state.mIsHalfShroudEnabled && !state.mHasExitRequested &&
        IsViewableState(state.mReplayState);
}

bool IsRecordingActive(const RuntimeGateState& state) noexcept
{
    return IsRecordingStarted(state) && state.mReplayState == replay::ReplayState::Recording;
}

bool IsRecordingStarted(const RuntimeGateState& state) noexcept
{
    return state.mIsStarted && !state.mIsPlayback;
}

bool ShouldBlockPlayerCommands(const RuntimeGateState& state) noexcept
{
    return state.mIsPlayback && state.mIsPlaybackSourceOpen && !state.mHasStartFailed;
}

bool IsPlaybackPlaying(const RuntimeGateState& state) noexcept
{
    return state.mIsStarted && state.mIsPlayback && state.mReplayState == replay::ReplayState::Playing;
}

bool CanSeekPlayback(const RuntimeGateState& state) noexcept
{
    return IsPlaybackViewActive(state) && state.mHasTotalFrames &&
        (state.mReplayState == replay::ReplayState::Playing || state.mReplayState == replay::ReplayState::Paused);
}

bool ShouldRunBeforeSimulation(const RuntimeGateState& state) noexcept
{
    return IsPlaybackPlaying(state) || IsRecordingActive(state);
}

bool ShouldNeutralizeAircraftShadowSideEffects(bool isPlaybackRequest, std::uint32_t gameMode) noexcept
{
    return isPlaybackRequest || replay::FrameCodec::IsOfflineGameMode(gameMode);
}
}
