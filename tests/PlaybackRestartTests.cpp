#include "../src/plugin/PlaybackRestart.h"
#include "../src/plugin/RuntimeGates.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::plugin::CanRestartInProcess;
using rareplay::plugin::DecideRestart;
using rareplay::plugin::PlaybackRestart;
using rareplay::plugin::RestartDecision;
using rareplay::plugin::RestartRequest;
using rareplay::plugin::RestartSource;
using rareplay::plugin::RuntimeGateState;
using rareplay::replay::ReplayState;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

RestartRequest MakeReplayRequest()
{
    return {RestartSource::Replay, std::nullopt, false};
}

RestartRequest MakeBackwardRequest(std::uint32_t target, bool isPaused)
{
    return {RestartSource::BackwardSeek, target, isPaused};
}

bool SameRequest(const RestartRequest& left, const RestartRequest& right)
{
    return left.mSource == right.mSource && left.mTargetFrame == right.mTargetFrame &&
        left.mIsPausedAfterRestart == right.mIsPausedAfterRestart;
}

// 已开始、正在播放、快照已取得：进程内重开的前提全部满足。
RuntimeGateState MakeRestartableState()
{
    RuntimeGateState state;
    state.mIsStarted = true;
    state.mIsPlayback = true;
    state.mIsPlaybackSourceOpen = true;
    state.mReplayState = ReplayState::Playing;
    state.mHasTotalFrames = true;
    state.mCanRestartFromSnapshot = true;
    return state;
}

void CheckDecision()
{
    Require(DecideRestart(RestartSource::Replay, true) == RestartDecision::Execute, "replay executes in process");
    Require(DecideRestart(RestartSource::BackwardSeek, true) == RestartDecision::Execute,
        "backward seek executes in process");
    Require(DecideRestart(RestartSource::Replay, false) == RestartDecision::FallBackToLauncher,
        "replay falls back to the launcher");
    Require(DecideRestart(RestartSource::BackwardSeek, false) == RestartDecision::Reject,
        "backward seek is rejected");

    // 与运行时门控一起：前提逐项不满足时，重新播放退回启动器，向后跳转拒绝。
    struct GateCase
    {
        const char* mName;
        void (*mChange)(RuntimeGateState&);
        bool mIsRestartable;
    };
    const GateCase cases[] = {
        {"playing", [](RuntimeGateState&) {}, true},
        {"paused", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Paused; }, true},
        {"ended", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Ended; }, true},
        {"view failed", [](RuntimeGateState& s) { s.mHasViewFailed = true; }, true},
        {"unknown total", [](RuntimeGateState& s) { s.mHasTotalFrames = false; }, true},
        {"no restartable snapshot (unreadable or campaign)", [](RuntimeGateState& s) { s.mCanRestartFromSnapshot = false; }, false},
        {"failed", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Failed; }, false},
        {"finished", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Finished; }, false},
        {"inactive", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Inactive; }, false},
        {"not started", [](RuntimeGateState& s) { s.mIsStarted = false; }, false},
        {"recording", [](RuntimeGateState& s) { s.mIsPlayback = false; s.mReplayState = ReplayState::Recording; }, false},
        {"source closed", [](RuntimeGateState& s) { s.mIsPlaybackSourceOpen = false; }, false},
        {"start failed", [](RuntimeGateState& s) { s.mHasStartFailed = true; }, false},
        {"exit requested", [](RuntimeGateState& s) { s.mHasExitRequested = true; }, false}};
    for (const GateCase& gateCase : cases)
    {
        auto state = MakeRestartableState();
        gateCase.mChange(state);
        const bool canRestart = CanRestartInProcess(state);
        Require(canRestart == gateCase.mIsRestartable, std::string("precondition: ") + gateCase.mName);
        Require(DecideRestart(RestartSource::Replay, canRestart) ==
                    (canRestart ? RestartDecision::Execute : RestartDecision::FallBackToLauncher),
            std::string("replay decision: ") + gateCase.mName);
        Require(DecideRestart(RestartSource::BackwardSeek, canRestart) ==
                    (canRestart ? RestartDecision::Execute : RestartDecision::Reject),
            std::string("backward seek decision: ") + gateCase.mName);
    }
}

void CheckIdle()
{
    PlaybackRestart restart;
    Require(!restart.IsRequested() && !restart.IsAwaitingStart() && !restart.GetPendingRequest() &&
                !restart.GetRequestedView(),
        "a fresh restart is idle");
    Require(!restart.Cancel(), "nothing to cancel");
    Require(!restart.BeginExecution() && !restart.IsAwaitingStart(), "nothing to execute");
    Require(!restart.OnPlaybackStarted(), "no follow-up without a restart");
    restart.Abandon();
    Require(!restart.IsRequested() && !restart.IsAwaitingStart(), "abandon keeps it idle");
}

void CheckRequestAndView()
{
    PlaybackRestart restart;
    Require(restart.Request(MakeBackwardRequest(1'200, true)), "a request is accepted");
    Require(restart.IsRequested() && !restart.IsAwaitingStart(), "the request waits for the execution point");
    Require(SameRequest(*restart.GetPendingRequest(), MakeBackwardRequest(1'200, true)), "the pending request is kept");
    auto view = restart.GetRequestedView();
    Require(view && view->mIsSeeking && view->mIsReloading && view->mStartFrame == 0 && view->mTargetFrame == 1'200,
        "a pending backward seek shows as reloading towards its target");

    // 重开期间再来的请求整体改写：目标覆盖，来源与暂停状态取最后一次。
    Require(restart.Request(MakeBackwardRequest(300, true)), "a second request is accepted");
    Require(SameRequest(*restart.GetPendingRequest(), MakeBackwardRequest(300, true)), "the later target wins");
    Require(restart.Request(MakeReplayRequest()), "replay overrides a pending seek");
    Require(SameRequest(*restart.GetPendingRequest(), MakeReplayRequest()), "the replay request wins");
    view = restart.GetRequestedView();
    Require(view && view->mIsReloading && view->mTargetFrame == 0, "a pending replay shows frame 0 as its target");
    Require(restart.Request(MakeBackwardRequest(0, false)), "a seek to frame 0 overrides a pending replay");
    Require(SameRequest(*restart.GetPendingRequest(), MakeBackwardRequest(0, false)), "frame 0 is a real target");

    // 取消：撤销尚未执行的请求，只撤销一次。
    Require(restart.Cancel() && !restart.IsRequested() && !restart.GetRequestedView(), "cancel revokes the request");
    Require(!restart.Cancel(), "a second cancel does nothing");
    Require(!restart.BeginExecution(), "a cancelled request is not executed");
}

void CheckExecutionAndFollowUp()
{
    struct FollowUpCase
    {
        const char* mName;
        RestartRequest mRequest;
        std::optional<std::uint32_t> mSeekTarget;
        bool mIsPaused;
    };
    const FollowUpCase cases[] = {
        {"backward seek while playing", MakeBackwardRequest(2'400, false), 2'400u, false},
        {"backward seek while paused", MakeBackwardRequest(2'400, true), 2'400u, true},
        {"backward seek to frame 1", MakeBackwardRequest(1, true), 1u, true},
        {"backward seek to frame 0 while paused", MakeBackwardRequest(0, true), std::nullopt, true},
        {"backward seek to frame 0 while playing", MakeBackwardRequest(0, false), std::nullopt, false},
        {"replay", MakeReplayRequest(), std::nullopt, false}};
    for (const FollowUpCase& followUpCase : cases)
    {
        const std::string name = followUpCase.mName;
        PlaybackRestart restart;
        Require(restart.Request(followUpCase.mRequest), name + ": requested");
        const auto executed = restart.BeginExecution();
        Require(executed && SameRequest(*executed, followUpCase.mRequest), name + ": execution takes the request");
        Require(restart.IsAwaitingStart() && !restart.IsRequested() && !restart.GetPendingRequest() &&
                    !restart.GetRequestedView(),
            name + ": waiting for playback to start, no longer pending");
        // 等待开始播放时不消费观看命令；即使有请求或取消到达也不改变。
        Require(!restart.Request(MakeBackwardRequest(99, true)) && !restart.Cancel() && restart.IsAwaitingStart(),
            name + ": requests and cancels are refused while waiting");
        Require(!restart.BeginExecution(), name + ": nothing more to execute");
        const auto followUp = restart.OnPlaybackStarted();
        Require(followUp && followUp->mSeekTargetFrame == followUpCase.mSeekTarget &&
                    followUp->mIsPaused == followUpCase.mIsPaused,
            name + ": follow-up after playback starts");
        Require(!restart.IsAwaitingStart() && !restart.OnPlaybackStarted(), name + ": the follow-up is handed out once");
        // 重开后的快进中再次向后：状态机已空闲，接受新的请求。
        Require(restart.Request(MakeBackwardRequest(10, true)) && restart.IsRequested(),
            name + ": a new backward seek during the follow-up seek is accepted");
    }
}

void CheckAbandon()
{
    // 执行点放弃（前提不再满足、读取器打不开）或原生重开失败：回到空闲，没有后续动作。
    PlaybackRestart restart;
    Require(restart.Request(MakeBackwardRequest(500, false)) && restart.BeginExecution(), "execution started");
    restart.Abandon();
    Require(!restart.IsAwaitingStart() && !restart.IsRequested() && !restart.OnPlaybackStarted(),
        "an abandoned restart has no follow-up");
    Require(restart.Request(MakeReplayRequest()), "a new request after abandoning is accepted");
    restart.Abandon();
    Require(!restart.IsRequested() && !restart.BeginExecution(), "abandon also drops a pending request");
}
}

int main()
{
    try
    {
        CheckDecision();
        CheckIdle();
        CheckRequestAndView();
        CheckExecutionAndFollowUp();
        CheckAbandon();
        std::cout << "Playback restart tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
