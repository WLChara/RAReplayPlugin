#include "PlaybackRestart.h"

namespace rareplay::plugin
{
RestartDecision DecideRestart(RestartSource source, bool canRestartInProcess) noexcept
{
    if (canRestartInProcess)
        return RestartDecision::Execute;
    return source == RestartSource::Replay ? RestartDecision::FallBackToLauncher : RestartDecision::Reject;
}

bool PlaybackRestart::Request(const RestartRequest& request) noexcept
{
    if (mPhase == Phase::AwaitingStart)
        return false;
    mRequest = request;
    mPhase = Phase::Requested;
    return true;
}

bool PlaybackRestart::Cancel() noexcept
{
    if (mPhase != Phase::Requested)
        return false;
    *this = PlaybackRestart();
    return true;
}

std::optional<RestartRequest> PlaybackRestart::GetPendingRequest() const noexcept
{
    if (mPhase != Phase::Requested)
        return std::nullopt;
    return mRequest;
}

std::optional<RestartRequest> PlaybackRestart::BeginExecution() noexcept
{
    if (mPhase != Phase::Requested)
        return std::nullopt;
    mPhase = Phase::AwaitingStart;
    return mRequest;
}

void PlaybackRestart::Abandon() noexcept
{
    *this = PlaybackRestart();
}

std::optional<RestartFollowUp> PlaybackRestart::OnPlaybackStarted() noexcept
{
    if (mPhase != Phase::AwaitingStart)
        return std::nullopt;
    RestartFollowUp followUp;
    // 目标为 0 时不快进：开始播放时最后完成帧也记为 0，快进没有可走的帧，开头即目标。
    if (mRequest.mTargetFrame && *mRequest.mTargetFrame > 0)
        followUp.mSeekTargetFrame = mRequest.mTargetFrame;
    followUp.mIsPaused = mRequest.mIsPausedAfterRestart;
    *this = PlaybackRestart();
    return followUp;
}

std::optional<viewer::PlaybackSeekView> PlaybackRestart::GetRequestedView() const noexcept
{
    if (mPhase != Phase::Requested)
        return std::nullopt;
    viewer::PlaybackSeekView view;
    view.mIsSeeking = true;
    view.mIsReloading = true;
    view.mStartFrame = 0;
    view.mTargetFrame = mRequest.mTargetFrame.value_or(0);
    return view;
}
}
