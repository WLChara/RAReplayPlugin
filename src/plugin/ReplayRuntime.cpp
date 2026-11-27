#include "ReplayRuntime.h"

#include "Diagnostics.h"
#include "EventCaptureReporter.h"
#include "PlaybackRestart.h"
#include "PlaybackSeek.h"
#include "PlaybackSeekProfile.h"
#include "PlaybackTimelineScan.h"
#include "PlaybackViewingSession.h"
#include "RuntimeGates.h"
#include "ViewerCommandRouter.h"
#include "../viewer/ReplayViewExchange.h"
#include "../game_api/GameApi.h"
#include "../game_api/StateDigestReader.h"
#include "../replay/FrameCodec.h"
#include "../replay/ReplaySession.h"
#include "../stats/GameTimelineBuilder.h"
#include "../storage/FrameReader.h"
#include "../storage/FrameWriter.h"
#include "../storage/ReplayConfig.h"
#include "../target/TargetImage.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rareplay::plugin
{
namespace
{
const char* GetSeekEndReasonName(SeekEndReason reason) noexcept
{
    switch (reason)
    {
    case SeekEndReason::Reached: return "reached";
    case SeekEndReason::Cancelled: return "cancelled";
    case SeekEndReason::PlaybackEnded: return "ended";
    case SeekEndReason::Failed: return "failed";
    case SeekEndReason::Exited: return "exited";
    }
    return "unknown";
}

// 快进的兜底重绘节拍、输入降频与耗时都用单调时钟的毫秒数；HUD 呈现心跳取自同一时钟，按同一纪元换算。
std::int64_t ToSteadyMs(std::chrono::steady_clock::time_point time) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

std::int64_t GetSteadyNowMs() noexcept
{
    return ToSteadyMs(std::chrono::steady_clock::now());
}

const char* GetRestartSourceName(RestartSource source) noexcept
{
    return source == RestartSource::Replay ? "replay" : "backward-seek";
}

// 技术日志：目标帧为空时写 none。
std::string FormatRestartTarget(const std::optional<std::uint32_t>& targetFrame)
{
    return targetFrame ? std::to_string(*targetFrame) : std::string("none");
}

/**
 * 游戏线程上唯一的启动请求、领域会话及文件句柄所有者，负责录制与播放的生命周期、帧驱动和观看命令的执行。
 * 观看呈现由成员 mViewing 装配，回放状态在每次调用时传给它。
 * 向前跳转：持有快进状态 mSeek、是否已静音、输入降频节拍与快进诊断累计，以及时间表扫描 mTimelineScan
 * （插件唯一的工作线程，回放源打开后启动，会话结束、失败与退出时停止并 join）。
 * 进程内重开（同进程重新播放与向后跳转）：持有待重开状态 mRestart 与首次载入时的会话快照；观看命令只记下请求，
 * 在 GAME_OVER_CALL 执行点做插件收尾、调用原生重开、复位每场状态，本对象本身跨重开存活（不经 OnSessionEnd）。
 */
class ReplayRuntime
{
public:
    ReplayRuntime(storage::ReplayRequest request, const std::filesystem::path& gameDirectory)
        : mRequest(std::move(request)), mSession(mGame),
          mViewing(mGame, gameDirectory, mRequest.mRequestId) {}

    bool IsPlayback() const { return mRequest.mMode == storage::ReplayMode::Play; }
    bool IsStarted() const { return mIsStarted; }
    bool ShouldUseHalfShroud() const { return plugin::ShouldUseHalfShroud(GetGateState()); }
    bool IsRecordingActive() const { return plugin::IsRecordingActive(GetGateState()); }
    bool ShouldBlockPlayerCommands() const { return plugin::ShouldBlockPlayerCommands(GetGateState()); }
    bool TrySetPlaybackViewingSpeed(int setting)
    {
        return ShouldBlockPlayerCommands() && mGame.TrySetViewerPlaybackSpeed(setting);
    }
    bool TryGetPlaybackViewingSpeed(int& outSetting) const
    {
        return ShouldBlockPlayerCommands() && mGame.TryGetViewerPlaybackSpeed(outSetting);
    }
    bool ShouldHandlePlaybackViewingSpeed() const
    {
        return ShouldBlockPlayerCommands() && mGame.CanControlViewerPlaybackSpeed();
    }

    bool IsPlaybackViewActive() const noexcept { return plugin::IsPlaybackViewActive(GetGateState()); }
    bool ShouldSkipLoadBlitSleep() const noexcept { return plugin::ShouldSkipLoadBlitSleep(GetGateState()); }

    // 只读请求类型与当前 GameMode，不看会话状态；开局前即可求值，录制与播放两端从同一时刻开始中和。
    bool ShouldNeutralizeAircraftShadowSideEffects() noexcept
    {
        if (!plugin::ShouldNeutralizeAircraftShadowSideEffects(IsPlayback(), mGame.ReadSessionGameMode()))
            return false;
        if (!mHasLoggedAircraftShadowNeutralization)
        {
            mHasLoggedAircraftShadowNeutralization = true;
            Log(IsPlayback() ? "Aircraft shadow draw side effects neutralized (playback)" :
                "Aircraft shadow draw side effects neutralized (recording)");
        }
        return true;
    }

    NativeSidebarAction UpdateNativeSidebar() noexcept
    {
        return mViewing.UpdateNativeSidebar(IsPlaybackViewActive(), mSeek.IsActive());
    }

    // 不在快进中时不读时钟，节拍只忘掉上次的时刻，原生输入照常完整执行。
    bool ShouldSkipViewerInputUpdate() noexcept
    {
        const bool isSeeking = mSeek.IsActive();
        return mSeekInputThrottle.ShouldSkip(isSeeking, isSeeking ? GetSteadyNowMs() : 0);
    }

    // 雷达事件观察点：雷达会话未 Begin（录制或回放尚未开始）时这些调用立即返回。
    void RadarSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
        const void* targetCell) noexcept
    {
        mViewing.OnSuperWeaponFireBegin(house, super, prerequisiteSuper, targetCell);
    }
    void RadarSuperWeaponFireEnd(const void* super) noexcept { mViewing.OnSuperWeaponFireEnd(super); }
    void RadarBuildingInfiltrated(const void* spy, const void* building) noexcept
    {
        mViewing.OnBuildingInfiltrated(spy, building);
    }

    bool OpenPlaybackSource()
    {
        if (!IsPlayback() || mHasStartFailed)
            return false;
        if (mIsPlaybackSourceOpen)
            return true;
        std::string error;
        if (!OpenPlaybackReader(mReader, &mRecordedInfo, error))
            return Fail(error);
        const auto containerMessage = "Replay source container v" + std::to_string(mReader.GetContainerVersion()) +
            ": logicalStreamBytes=" + std::to_string(mReader.GetLogicalStreamBytes()) +
            " storedStreamBytes=" + std::to_string(mReader.GetStoredStreamBytes());
        Log(containerMessage.c_str());
        if (mReader.IsUnfinalized())
        {
            // 遗留的未完成录像没有内容哈希；只在这里记录一次，播放仍走正常 EOF 路径。
            const auto message = "Replay source is unfinalized; recoveredFrames=" +
                std::to_string(mReader.GetRecoveredFrameCount());
            Log(message.c_str());
        }
        mIsPlaybackSourceOpen = true;
        // 时间表在工作线程上扫描，用独立的读取器，输入与上面的 Open 相同；失败只让总帧数保持未知。
        mTimelineScan.Start(mRequest.mReplayPath.wstring(), target::TARGET_SHA256_BYTES);
        return true;
    }

    /**
     * 打开回放文件并核对启动请求的清单，读出帧流头；OpenPlaybackSource 与进程内重开（重新打开到第 0 帧）共用。
     * outInfo 非空时写入帧流头的会话信息。失败时 reader 处于关闭状态并写 error。
     */
    bool OpenPlaybackReader(storage::FrameReader& reader, replay::ReplaySessionInfo* outInfo, std::string& error)
    {
        std::vector<std::uint8_t> metadata;
        if (!reader.Open(mRequest.mReplayPath.wstring(), target::TARGET_SHA256_BYTES, metadata, error))
            return false;
        replay::ReplaySessionInfo info;
        if (!storage::MatchesArchiveManifest(mRequest, metadata))
            error = "Replay request manifest differs from archive";
        else if (reader.GetSessionInfo(info, error))
        {
            if (outInfo != nullptr)
                *outInfo = info;
            return true;
        }
        reader.Close();
        return false;
    }

    /**
     * 开局载入的预世界等待点（只对播放请求）。第一次：读取会话快照，只读这一次，失败也不重试；战役（GameMode 0）的快照
     * 不能用于重开，记日志后进程内重开保持不可用。之后每次（进程内重开的载入）：不覆盖快照，只再读一次逐项比对并写一行日志，
     * 用来核对还原是否正确（载入期间的变换对快照值幂等，正确时应一致）。
     */
    void OnScenarioPreworldWait() noexcept
    {
        if (!IsPlayback())
            return;
        try
        {
            if (mHasReadSessionSnapshot)
            {
                CheckSessionSnapshotAfterRestart();
                return;
            }
            mHasReadSessionSnapshot = true;
            mSessionSnapshot = mGame.ReadScenarioSessionSnapshot();
            if (!mSessionSnapshot)
            {
                Log("Replay session snapshot unavailable; in-process restart disabled");
                return;
            }
            const auto& snapshot = *mSessionSnapshot;
            mCanRestartFromSnapshot = game_api::native::CanRestartFromSnapshot(snapshot);
            const auto message = "Replay session snapshot recorded: gameMode=" + std::to_string(snapshot.mGameMode) +
                " maxAhead=" + std::to_string(snapshot.mMaxAhead) + " frameSendRate=" +
                std::to_string(snapshot.mFrameSendRate) + " maxMaxAhead=" + std::to_string(snapshot.mMaxMaxAhead) +
                " desiredFrameRate=" + std::to_string(snapshot.mDesiredFrameRate) + " specialFlags=" +
                std::to_string(snapshot.mSpecialRulesFlags) + " multiplayerObserver=" +
                std::to_string(snapshot.mMultiplayerObserver) + " scenario=" + snapshot.mScenarioFileName;
            Log(message.c_str());
            if (!mCanRestartFromSnapshot)
                Log("Replay in-process restart unavailable: campaign mode");
        }
        catch (...) { Log("Replay session snapshot handling failed"); }
    }

    void CheckSessionSnapshotAfterRestart()
    {
        if (!mSessionSnapshot)
            return;
        const auto current = mGame.ReadScenarioSessionSnapshot();
        if (!current)
        {
            Log("Replay session snapshot check after restart: unreadable");
            return;
        }
        const auto differences = game_api::native::DescribeSnapshotDifferences(*mSessionSnapshot, *current);
        Log(differences.empty() ? "Replay session snapshot check after restart: matches"
                                : ("Replay session snapshot check after restart: differs " + differences).c_str());
    }

    bool TryGetPlaybackSeed(std::uint32_t& outSeed)
    {
        if (!OpenPlaybackSource())
            return false;
        outSeed = mRecordedInfo.mRandomSeed;
        return true;
    }

    void BeginFrame()
    {
        MarkSeekPhase(SeekPhase::Input);
        mGame.BeginFrame();
        if (!mGame.IsWorldReady())
            return;
        if (!mIsStarted && !Start())
            return;
        ConsumeViewerCommands();
        if (mHasExitRequested)
            return;
        ReportFailure(mSession.OnFrameBegin());
        SyncSeekWithSession();
    }

    ViewerLoopAction ViewerLoopGate()
    {
        if (!IsPlayback())
            return ViewerLoopAction::Continue;
        MarkSeekPhase(SeekPhase::Simulation);
        if (mIsStarted)
            ConsumeViewerCommands();
        if (mHasExitRequested || mSession.GetState() == replay::ReplayState::Failed ||
            mGame.IsReplayExitRequested())
            return ViewerLoopAction::Exit;
        if (!mSession.IsPlaybackHeld())
        {
            // 继续命令也可能在本轮输入后到达；在进入模拟前补齐同一个未执行帧。
            if (mIsStarted)
                ReportFailure(mSession.OnFrameBegin());
            SyncSeekWithSession();
            if (mSession.GetState() == replay::ReplayState::Failed)
                return ViewerLoopAction::Exit;
            if (mSession.IsPlaybackHeld())
                return ViewerLoopAction::Hold;
            return GetSeekDrawAction();
        }
        mGame.DiscardLocalCommands();
        mGame.UpdatePausedCamera();
        UpdateViewer();
        return ViewerLoopAction::Hold;
    }

    AfterFrameLoopAction AfterFrameLoop()
    {
        if (!mIsStarted || !IsPlayback())
            return AfterFrameLoopAction::NativeGameOver;
        // 进程内重开只在这里执行：此时已在 MainLoop 之外，主循环的下一次迭代即新场景的第 0 帧。
        if (mRestart.IsRequested())
        {
            if (const auto action = ExecutePendingRestart())
                return *action;
        }
        ReportFailure(mSession.OnAfterFrameLoop());
        // 预取到 EOF 时会话转为结束：快进以“回放结束”结束，停在结束状态。
        SyncSeekWithSession();
        const auto token = PushSeekPhase(SeekPhase::Plugin);
        UpdateViewer();
        PopSeekPhase(token);
        const bool shouldSkipGameOver = mSession.IsPlaybackHeld() || mHasExitRequested ||
            mSession.GetState() == replay::ReplayState::Failed;
        return shouldSkipGameOver ? AfterFrameLoopAction::SkipGameOver : AfterFrameLoopAction::NativeGameOver;
    }

    void BeforeNativeEvents()
    {
        if (!IsRecordingStarted())
            return;
        mGame.BeforeNativeEventExecution();
        ReportFailure(mSession.OnBeforeEvents());
    }

    void CaptureDispatchedEvent(std::optional<std::uint32_t> ringIndex)
    {
        if (!IsRecordingActive())
            return;
        std::string error;
        if (!ringIndex)
        {
            // 标记未打开时（联机或本帧外）不采集，寄存器异常也不影响录制。
            if (!mGame.IsCapturingDispatchedEvents())
                return;
            error = "Native event dispatch register is not a DoList ring index";
        }
        else
        {
            replay::RecordedEvent event{};
            bool isCaptured = false;
            if (mGame.CopyDispatchedEvent(*ringIndex, event, isCaptured, error))
            {
                if (isCaptured)
                    ReportFailure(mSession.OnEventDispatched(event));
                return;
            }
        }
        mSession.Stop();
        Fail(error);
    }

    void AfterNativeFrameParser()
    {
        if (IsRecordingStarted())
            mGame.AfterNativeFrameParser();
    }

    void BeforeSimulation()
    {
        // 播放：写回录制速度并重演输入效果；录制：只采样游戏速度。会话按自身状态分支。
        if (!plugin::ShouldRunBeforeSimulation(GetGateState()))
            return;
        const bool success = mSession.OnBeforeSimulation();
        if (const auto difference = mSession.TakeInitialGameSpeedDifference())
        {
            const auto message = "Replay game speed differs at frame 0: recorded=" +
                std::to_string(difference->mRecorded) + " current=" + std::to_string(difference->mCurrent) +
                "; using recorded";
            Log(message.c_str());
        }
        ReportFailure(success);
    }

    void CapturePlayerInputEffect(const replay::RecordedInputEffect& effect)
    {
        if (!IsRecordingActive())
            return;
        std::string error;
        if (!mGame.RecordPlayerInputEffect(effect, error))
            FailPlayerInputCapture(error);
    }

    void FailPlayerInputCapture(const std::string& error)
    {
        if (!IsRecordingActive())
            return;
        mSession.Stop();
        Fail(error);
    }

    void CaptureControlGroupUpdate()
    {
        if (!IsRecordingActive())
            return;
        std::string error;
        if (!mGame.RecordControlGroupUpdate(error))
        {
            mSession.Stop();
            Fail(error);
        }
    }

    void CompleteFrame()
    {
        // 快进中本帧不限速：FRAME_COMPLETE 在 RegulateFrameRate 之前，两个预算下一帧由原生重算。
        if (mSeek.IsActive())
        {
            MarkSeekPhase(SeekPhase::Other);
            mGame.ClearFrameBudgets();
        }
        if (mIsStarted)
            ReportFailure(mSession.OnFrameEnd());
        if (IsRecordingActive())
        {
            if (const auto stats = mGame.CompleteEventCaptureFrame())
                mEventCaptureReporter.ReportFrame(*stats);
        }
        // 快进中逐帧清理积压（本帧记录处理之后）：脏矩形清零，电弧、光束与已解绑的尾迹超过阈值时整体删除。
        if (mSeek.IsActive())
            CleanSeekBacklog(SeekCleanupPoint::FrameCompleted);
        if (IsPlaybackPlaying())
        {
            CaptureRuntimeGameSpeed();
            const std::uint32_t completedFrame = mSession.GetLastCompletedFrame();
            auto token = PushSeekPhase(SeekPhase::Plugin);
            mViewing.OnPlaybackFrameCompleted(completedFrame);
            PopSeekPhase(token);
            // 停点：第“目标”帧完成后结束快进；跳转前在暂停时恢复暂停，下一帧的门控即保持在这里。
            // 停点的状态摘要在结束快进之前记录，与间隔摘要一样计入本次快进的插件段。
            const bool isSeekReached = mSeek.OnFrameCompleted(completedFrame);
            RecordStateDigest(completedFrame, isSeekReached);
            if (isSeekReached)
                EndSeek(SeekEndReason::Reached, true);
            token = PushSeekPhase(SeekPhase::Plugin);
            UpdateViewer();
            PopSeekPhase(token);
        }
    }

    bool ProcessPlaybackFrame()
    {
        if (!IsPlayback())
            return false;
        // 快进计时：本帧录制事件的执行单独成段，返回后到帧完成为帧尾。
        MarkSeekPhase(SeekPhase::Events);
        if (!mIsStarted)
        {
            if (!mGame.IsWorldReady() || !Start())
            {
                mGame.EndReplay();
                return true;
            }
        }
        mGame.DiscardLocalCommands();
        mSession.ProcessPlaybackFrame();
        ReportFailure(mSession.GetState() != replay::ReplayState::Failed);
        MarkSeekPhase(SeekPhase::Tail);
        return true;
    }

    void Finish()
    {
        // 对局在快进中结束（例如胜负已分）时仍要恢复声音；世界随即拆除，不请求重绘。
        EndSeek(SeekEndReason::Exited, false);
        mTimelineScan.Stop();
        if (IsRecordingStarted())
        {
            const auto stats = mGame.GetControlGroupCaptureStats();
            const auto message = "Replay groups capture: nativeUpdates=" +
                std::to_string(stats.mNativeUpdateCount) + " snapshotUpdates=" +
                std::to_string(stats.mSnapshotUpdateCount);
            Log(message.c_str());
            if (mGame.UsesDispatchEventCapture())
                mEventCaptureReporter.ReportSummary(mGame.GetEventCaptureTotals());
        }
        if (IsPlayback())
            mViewing.ReportFinalOverlayDiagnostics();
        mViewing.Stop();
        if (mIsStarted && mSession.GetState() == replay::ReplayState::Recording)
        {
            const bool success = mSession.Finish();
            ReportFailure(success);
            if (success)
            {
                // 实机验收用 storedBytes/logicalBytes 核对帧流压缩率（不含清单段）。
                const auto message = "Replay recording finalized: frames=" + std::to_string(mWriter.GetFrameCount()) +
                    " logicalBytes=" + std::to_string(mWriter.GetLogicalStreamBytes()) +
                    " storedBytes=" + std::to_string(mWriter.GetStoredStreamBytes());
                Log(message.c_str());
            }
        }
        else
            mSession.Stop();
        mReader.Close();
    }

    void FailUnexpected()
    {
        mRestart.Abandon();
        EndSeek(SeekEndReason::Failed, false);
        mTimelineScan.Stop();
        mViewing.Stop();
        mSession.Stop();
        if (IsPlayback())
            mGame.EndReplay();
        Log("Unexpected exception in replay hook");
    }

private:
    RuntimeGateState GetGateState() const noexcept
    {
        RuntimeGateState state;
        state.mIsStarted = mIsStarted;
        state.mIsPlayback = IsPlayback();
        state.mReplayState = mSession.GetState();
        state.mIsPlaybackSourceOpen = mIsPlaybackSourceOpen;
        state.mHasStartFailed = mHasStartFailed;
        state.mHasExitRequested = mHasExitRequested;
        state.mHasViewFailed = mViewing.HasViewFailed();
        state.mIsHalfShroudEnabled = mViewing.IsHalfShroudEnabled();
        state.mHasTotalFrames = mTotalFrames.has_value();
        state.mCanRestartFromSnapshot = mCanRestartFromSnapshot;
        return state;
    }
    bool IsRecordingStarted() const noexcept { return plugin::IsRecordingStarted(GetGateState()); }
    bool IsPlaybackPlaying() const noexcept { return plugin::IsPlaybackPlaying(GetGateState()); }

    // 一次取出全部命令后逐条路由并执行；每条都按前一条执行后的状态路由。
    // 某条执行抛出异常时，本批剩余命令一并丢弃。
    void ConsumeViewerCommands() noexcept
    {
        if (!IsPlayback())
            return;
        try
        {
            for (const auto& command : viewer::GetReplayViewExchange().Drain())
            {
                ViewerRouterState state;
                state.mSessionId = mViewing.GetSessionId();
                state.mHasExitRequested = mHasExitRequested;
                state.mIsHalfShroudEnabled = mViewing.IsHalfShroudEnabled();
                state.mHudScale = mViewing.GetHudScale();
                state.mLastCompletedFrame = mSession.GetLastCompletedFrame();
                // 扫描未结束或失败时总帧数为空，跳转命令被路由拦下；状态是否允许开始快进由执行方再按 CanSeekPlayback 判定。
                state.mTotalFrames = mTotalFrames;
                state.mIsSeeking = mSeek.IsActive();
                state.mSeekTargetFrame = mSeek.GetTargetFrame();
                state.mIsPaused = mSession.GetState() == replay::ReplayState::Paused;
                state.mSeekWasPaused = mSeek.WasPausedBeforeSeek();
                const auto pendingRestart = mRestart.GetPendingRequest();
                state.mIsRestartPending = pendingRestart.has_value();
                if (pendingRestart)
                    state.mPendingRestartTargetFrame = pendingRestart->mTargetFrame;
                if (const auto action = RouteViewerCommand(command, state))
                    ExecuteViewerAction(*action);
            }
        }
        catch (...) { Log("Replay HUD command was ignored after an exception"); }
    }

    void ExecuteViewerAction(const ViewerAction& action)
    {
        switch (action.mKind)
        {
        case ViewerActionKind::SetPlaybackSpeed:
            TrySetPlaybackViewingSpeed(action.mPlaybackSpeedSetting);
            break;
        case ViewerActionKind::SetPaused:
            mSession.SetPaused(action.mIsPaused);
            break;
        case ViewerActionKind::RequestRestart:
            RequestRestart({RestartSource::Replay, std::nullopt, false}, action.mIsHalfShroudEnabled);
            break;
        case ViewerActionKind::SetHalfShroud:
            mViewing.SetHalfShroudEnabled(action.mIsHalfShroudEnabled);
            break;
        case ViewerActionKind::ExitPlayback:
            CancelRestartRequest();
            EndSeek(SeekEndReason::Exited, true);
            mHasExitRequested = true;
            mTimelineScan.Stop();
            mGame.EndReplay();
            break;
        case ViewerActionKind::SetHudScale:
            mViewing.ApplyHudScale(action.mHudScale);
            break;
        case ViewerActionKind::CenterOnRadarEvent:
            mViewing.CenterOnRadarEvent(action.mRadarEventId);
            break;
        case ViewerActionKind::AdjustBattlefieldZoom:
            mViewing.GetBattlefieldZoom().StepZoom(action.mZoomSteps, action.mIsZoomAnchoredAtPointer
                ? game_api::zoom::ZoomStepAnchor::Pointer : game_api::zoom::ZoomStepAnchor::Center);
            break;
        case ViewerActionKind::ResetBattlefieldZoom:
            mViewing.GetBattlefieldZoom().ResetZoom();
            break;
        case ViewerActionKind::SeekToFrame:
            if (action.mIsSeekRetarget)
                RetargetSeek(action.mSeekTargetFrame);
            else
                BeginSeek(action.mSeekTargetFrame, mSession.GetState() == replay::ReplayState::Paused);
            break;
        case ViewerActionKind::SeekViaRestart:
            RequestRestart({RestartSource::BackwardSeek, action.mSeekTargetFrame, action.mIsPausedAfterSeek},
                mViewing.IsHalfShroudEnabled());
            break;
        case ViewerActionKind::CancelSeek:
            // 重开尚未执行时撤销请求；正在进行的快进（含重开前就在进行的）停在当前位置。
            CancelRestartRequest();
            EndSeek(SeekEndReason::Cancelled, true);
            break;
        }
    }

    // ---- 进程内重开 ----

    /**
     * 观看者请求重开（重新播放或向后跳转）：按 DecideRestart 记下请求（在下一次 GAME_OVER_CALL 执行），
     * 或者重新播放退回启动器，或者拒绝向后跳转。记下请求时不改变会话与快进：执行点会先结束快进。
     */
    void RequestRestart(const RestartRequest& request, bool isHalfShroudEnabled)
    {
        switch (DecideRestart(request.mSource, plugin::CanRestartInProcess(GetGateState())))
        {
        case RestartDecision::Execute:
        {
            // 重开已执行、等待开始播放时不消费观看命令，这里不会被拒绝；仍按返回值处理以免记错日志。
            if (!mRestart.Request(request))
                break;
            const auto message = std::string("Replay in-process restart requested: source=") +
                GetRestartSourceName(request.mSource) + " from=" +
                std::to_string(mSession.GetLastCompletedFrame()) + " target=" +
                FormatRestartTarget(request.mTargetFrame) + " pausedAfter=" +
                (request.mIsPausedAfterRestart ? "1" : "0");
            Log(message.c_str());
            break;
        }
        case RestartDecision::FallBackToLauncher:
            RestartThroughLauncher(isHalfShroudEnabled);
            break;
        case RestartDecision::Reject:
            Log("Replay backward seek ignored: in-process restart is unavailable");
            break;
        }
    }

    // 原有的重新播放：先结束快进（恢复声音），再请求启动器重新播放；请求失败时回放停在当前位置继续播放。
    void RestartThroughLauncher(bool isHalfShroudEnabled)
    {
        CancelRestartRequest();
        EndSeek(SeekEndReason::Exited, true);
        if (mViewing.RequestRestart(isHalfShroudEnabled))
        {
            mHasExitRequested = true;
            mTimelineScan.Stop();
            mGame.EndReplay();
        }
    }

    void CancelRestartRequest() noexcept
    {
        if (mRestart.Cancel())
            Log("Replay in-process restart request cancelled");
    }

    /**
     * 开始拆除之前放弃本次重开：记日志，回到空闲；重新播放退回启动器重启（已请求退出时不再退回），向后跳转只记日志。
     * 世界还完整，回放照常继续。
     */
    void AbandonRestartBeforeTeardown(const RestartRequest& request, const std::string& reason)
    {
        mRestart.Abandon();
        Log(("Replay in-process restart abandoned before teardown: " + reason).c_str());
        if (request.mSource == RestartSource::Replay && !mHasExitRequested)
            RestartThroughLauncher(mViewing.IsHalfShroudEnabled());
    }

    /**
     * GAME_OVER_CALL 执行点（主循环两次迭代之间）执行尚未执行的重开请求；没有请求时返回空。
     * 开始拆除之前依次确认：插件侧前提仍满足（CanRestartInProcess）、原生侧可以重开（CheckScenarioRestartReady：g_GameActive
     * 等）、新的帧读取器能打开；任何一项不满足都经 AbandonRestartBeforeTeardown 放弃并返回空，回放照常继续。
     * 开始拆除之后不再有可放弃的失败：任何一步失败都按播放失败处理并退出回放，返回 SkipGameOver。
     * 成功返回 ScenarioRestarted：新场景停在第 0 帧之前，运行时“尚未开始”，下一帧 OnFrameBegin 再 Start()。
     */
    std::optional<AfterFrameLoopAction> ExecutePendingRestart()
    {
        const auto request = mRestart.BeginExecution();
        if (!request)
            return std::nullopt;
        if (!plugin::CanRestartInProcess(GetGateState()))
        {
            AbandonRestartBeforeTeardown(*request, "preconditions are no longer met");
            return std::nullopt;
        }
        std::string error;
        if (!mGame.CheckScenarioRestartReady(*mSessionSnapshot, error))
        {
            AbandonRestartBeforeTeardown(*request, "the game cannot restart now: " + error);
            return std::nullopt;
        }
        // 帧读取器重新打开到第 0 帧；时间表扫描不重做，总帧数与时间轴沿用。
        storage::FrameReader reader;
        if (!OpenPlaybackReader(reader, nullptr, error))
        {
            AbandonRestartBeforeTeardown(*request, error);
            return std::nullopt;
        }
        LogRestartBegin(*request);
        mRestartStartedAtMs = GetSteadyNowMs();

        // 插件收尾：先结束快进（恢复声音；世界随即拆除，不清理积压、不请求重绘），再停观看会话：先停战局缩放回到物理状态，
        // 再结束发布（撤回 HUD 快照）、统计与雷达，清掉半黑幕缓存。侧栏门控不必显式停：开始播放前回放视图不处于激活，
        // 新会话的 HUD 也还没呈现，下一次侧栏绘制时门控自行转回原生并请求完整重画。
        EndSeek(SeekEndReason::Exited, false);
        mViewing.Stop();
        // 回到“尚未开始”：重开的载入期间各 Hook 的门控与首次载入相同（载入提速 L1 因此照样生效），
        // 回放会话回到 Inactive；下一帧 OnFrameBegin 在世界就绪后重新 Start()。不得调用 Stop：它会请求退出回放。
        // 以下失败都经 Fail：丢弃重开状态，因“尚未开始”记为启动失败，请求退出回放。
        mIsStarted = false;
        if (!mSession.ResetForRestart())
        {
            Fail("Replay in-process restart failed: the playback session cannot be reset");
            return AfterFrameLoopAction::SkipGameOver;
        }
        game_api::native::ScenarioRestartReport report;
        const bool isRestarted = mGame.RestartScenarioInProcess(*mSessionSnapshot, report);
        LogRestartReport(report, isRestarted);
        if (!isRestarted)
        {
            Fail("Replay in-process restart failed: " + report.mError);
            return AfterFrameLoopAction::SkipGameOver;
        }
        // 每场状态：换上位于第 0 帧的读取器，状态摘要从第 0 帧重新记录（读取异常后的停用也只限上一场）。
        mReader = std::move(reader);
        mLastStateDigestFrame.reset();
        mIsStateDigestDisabled = false;
        return AfterFrameLoopAction::ScenarioRestarted;
    }

    // 原生重开的诊断：耗时、删除的光束、载入前的 ScenarioInit、连接的删除与重建；失败时在 Fail 的日志之前写。
    void LogRestartReport(const game_api::native::ScenarioRestartReport& report, bool isRestarted) noexcept
    {
        try
        {
            const std::int64_t elapsedMs = mRestartStartedAtMs ? GetSteadyNowMs() - *mRestartStartedAtMs : 0;
            const auto message = std::string(isRestarted ? "Replay in-process restart loaded the scenario"
                                                         : "Replay in-process restart sequence stopped") +
                ": elapsedMs=" + std::to_string(elapsedMs) + " radBeams=" + std::to_string(report.mDeletedRadBeams) +
                " scenarioInitBefore=" + std::to_string(report.mScenarioInitBeforeStart) + " connections=" +
                (report.mHasRebuiltConnections ? std::to_string(report.mDeletedConnections) + "/" +
                        std::to_string(report.mRemainingConnections)
                                               : std::string("untouched"));
            Log(message.c_str());
        }
        catch (...) { Log("Replay in-process restart report unavailable"); }
    }

    // 重开开始拆除前记一行：请求内容与沿用到重开之后的观看设置（拆除后失败时也能据此核对）。
    void LogRestartBegin(const RestartRequest& request) noexcept
    {
        try
        {
            int speed = 0;
            const bool hasSpeed = mGame.TryGetViewerPlaybackSpeed(speed);
            const auto zoom = mViewing.GetBattlefieldZoom().GetStatus();
            const auto message = std::string("Replay in-process restart begins: source=") +
                GetRestartSourceName(request.mSource) + " from=" + std::to_string(mSession.GetLastCompletedFrame()) +
                " target=" + FormatRestartTarget(request.mTargetFrame) + " pausedAfter=" +
                (request.mIsPausedAfterRestart ? "1" : "0") + "; keeps viewerSpeed=" +
                (hasSpeed ? std::to_string(speed) : std::string("native")) + " zoomIndex=" +
                std::to_string(zoom.mRequestedIndex) + " halfShroud=" + (mViewing.IsHalfShroudEnabled() ? "1" : "0") +
                " hudScale=" + std::to_string(static_cast<int>(mViewing.GetHudScale()));
            Log(message.c_str());
        }
        catch (...) { Log("Replay in-process restart begins"); }
    }

    /**
     * 重开后开始播放（Start 成功）时调用：取出后续动作。目标帧大于 0 时以重开前保存的暂停状态开始向前跳转，
     * 快进到达或取消后恢复该状态；不快进（重新播放、目标为 0）或无法开始快进时，按该状态立即暂停或继续。
     */
    void ApplyRestartFollowUp()
    {
        const auto followUp = mRestart.OnPlaybackStarted();
        if (!followUp)
            return;
        if (mRestartStartedAtMs)
        {
            const auto message = "Replay in-process restart ready at frame 0: elapsedMs=" +
                std::to_string(GetSteadyNowMs() - *mRestartStartedAtMs);
            Log(message.c_str());
            mRestartStartedAtMs.reset();
        }
        if (followUp->mSeekTargetFrame && BeginSeek(*followUp->mSeekTargetFrame, followUp->mIsPaused))
            return;
        if (followUp->mSeekTargetFrame)
            Log("Replay seek after in-process restart could not start");
        if (followUp->mIsPaused)
            mSession.SetPaused(true);
    }

    // ---- 向前跳转 ----

    /**
     * 开始快进：只在可以跳转（CanSeekPlayback：播放或暂停、总帧数已知、观看界面可用）时开始，返回是否开始了。
     * wasPaused 是到达或取消后要恢复的暂停状态：观看命令取当前会话状态，重开后的快进取重开前保存的状态。
     * 当前在暂停时先解除暂停（暂停的唯一所有者仍是 ReplaySession::SetPaused），然后静音。
     * 目标已由路由夹取并保证在当前位置之后（重开后的目标大于 0，当前位置为 0）。
     */
    bool BeginSeek(std::uint32_t targetFrame, bool wasPaused)
    {
        if (!plugin::CanSeekPlayback(GetGateState()))
            return false;
        const std::uint32_t fromFrame = mSession.GetLastCompletedFrame();
        if (!mSeek.Begin(fromFrame, targetFrame, wasPaused, GetSteadyNowMs(), ReadHudPresentedAtMs()))
            return false;
        // 诊断从这里清零；阶段计时在下一个主循环阶段边界开始。
        mSeekProfile.Reset();
        if (mSession.GetState() == replay::ReplayState::Paused)
            mSession.SetPaused(false);
        MuteAudioForSeek();
        char message[96]{};
        std::snprintf(message, sizeof(message), "Replay seek started: from=%u target=%u", fromFrame, targetFrame);
        Log(message);
        return true;
    }

    void RetargetSeek(std::uint32_t targetFrame) noexcept
    {
        if (!mSeek.Retarget(targetFrame))
            return;
        char message[64]{};
        std::snprintf(message, sizeof(message), "Replay seek retargeted: target=%u", targetFrame);
        Log(message);
    }

    /**
     * 结束快进的唯一入口；不在快进中时什么也不做。canRequestRedraw 为真时先清理不画期间的积压（含仍挂在物体上的尾迹、
     * 雷达事件与屏震），再请求一次完整重绘（半黑幕开启时另登记整视口脏矩形）；然后恢复声音，按结果恢复跳转前的暂停状态，
     * 写结束日志与分阶段计时日志。canRequestRedraw 为假用于失败与会话收尾：回放随即结束，世界可能正在拆除，
     * 不清理、不请求重绘，只恢复声音。
     */
    void EndSeek(SeekEndReason reason, bool canRequestRedraw) noexcept
    {
        if (!mSeek.IsActive())
            return;
        if (canRequestRedraw)
        {
            CleanSeekBacklog(SeekCleanupPoint::SeekEnd);
            RequestSeekRedraw();
        }
        const auto now = std::chrono::steady_clock::now();
        mSeekProfile.GetClock().Stop(now);
        const auto outcome = mSeek.End(reason, ToSteadyMs(now));
        if (!outcome)
            return;
        RestoreAudioAfterSeek();
        if (outcome->mPausedToRestore.value_or(false))
            mSession.SetPaused(true);
        const std::uint32_t frames = outcome->mLastCompletedFrame > outcome->mStartFrame
            ? outcome->mLastCompletedFrame - outcome->mStartFrame : 0;
        const double fps = outcome->mElapsedMs > 0
            ? static_cast<double>(frames) * 1000.0 / static_cast<double>(outcome->mElapsedMs) : 0.0;
        char message[192]{};
        std::snprintf(message, sizeof(message),
            "Replay seek finished: reason=%s from=%u target=%u reached=%u frames=%u wallMs=%lld fps=%.1f",
            GetSeekEndReasonName(outcome->mReason), outcome->mStartFrame, outcome->mTargetFrame,
            outcome->mLastCompletedFrame, frames, static_cast<long long>(outcome->mElapsedMs), fps);
        Log(message);
        try { Log(mSeekProfile.FormatTimingLog(frames).c_str()); }
        catch (...) { Log("Replay seek timing unavailable"); }
    }

    /** 会话已转为结束或失败而快进仍在：以对应原因结束快进（失败的回放随即结束，不请求重绘）。 */
    void SyncSeekWithSession() noexcept
    {
        if (!mSeek.IsActive())
            return;
        const auto state = mSession.GetState();
        if (state == replay::ReplayState::Ended)
            EndSeek(SeekEndReason::PlaybackEnded, true);
        else if (state == replay::ReplayState::Failed)
            EndSeek(SeekEndReason::Failed, false);
    }

    /**
     * 快进中门控：默认一律不绘制，画面由 HUD 自己的呈现刷新进度。只有当前会话的 HUD 呈现心跳超时或不存在时，
     * 按兜底节拍先清理积压、请求完整重绘，再照常绘制一次。不在快进中时照常绘制。
     */
    ViewerLoopAction GetSeekDrawAction() noexcept
    {
        if (!mSeek.IsActive())
            return ViewerLoopAction::Continue;
        const std::int64_t nowMs = GetSteadyNowMs();
        if (!mSeek.ShouldDrawFallback(nowMs, ReadHudPresentedAtMs()))
            return ViewerLoopAction::ContinueWithoutDraw;
        CleanSeekBacklog(SeekCleanupPoint::BeforeFallbackDraw);
        RequestSeekRedraw();
        mSeek.MarkFallbackDrawn(nowMs);
        mSeekProfile.RecordFallbackDraw();
        return ViewerLoopAction::Continue;
    }

    // 当前会话的 HUD 呈现心跳（单调时钟毫秒）；F10 隐藏、界面未呈现或换会话时为空。
    std::optional<std::int64_t> ReadHudPresentedAtMs() const noexcept
    {
        const auto presentedAt = mViewing.ReadHudPresentedAt();
        if (!presentedAt)
            return std::nullopt;
        return ToSteadyMs(*presentedAt);
    }

    /**
     * 快进中的积压清理，只在快进期间、绘制之外调用（帧完成点、兜底重绘前的门控、结束快进时）：先记下各条数的峰值，
     * 再按时机与阈值清理，耗时计入清理阶段。
     */
    void CleanSeekBacklog(SeekCleanupPoint point) noexcept
    {
        const auto token = PushSeekPhase(SeekPhase::Cleanup);
        const auto counts = mGame.ReadSeekBacklogCounts();
        mSeekProfile.ObserveBacklog(counts);
        mSeekProfile.RecordCleanup(mGame.CleanSeekBacklog(PlanSeekBacklogCleanup(point, counts)));
        PopSeekPhase(token);
    }

    // 清理之后请求完整重绘；脏矩形已清零，半黑幕开启时补登记整视口脏矩形，遮罩随完整重绘重新生成。
    void RequestSeekRedraw() noexcept
    {
        mGame.RequestFullRedraw();
        if (ShouldUseHalfShroud())
            mGame.RegisterViewerShroudFullRedraw();
    }

    // ---- 快进分阶段计时：只在快进中读时钟；未开始计时（快进刚开始、尚未到阶段边界）时 Push/Pop 不起作用。----

    void MarkSeekPhase(SeekPhase phase) noexcept
    {
        if (mSeek.IsActive())
            mSeekProfile.GetClock().Enter(phase, std::chrono::steady_clock::now());
    }

    SeekPhaseClock::Token PushSeekPhase(SeekPhase phase) noexcept
    {
        if (!mSeek.IsActive())
            return std::nullopt;
        return mSeekProfile.GetClock().Push(phase, std::chrono::steady_clock::now());
    }

    void PopSeekPhase(SeekPhaseClock::Token token) noexcept
    {
        if (token)
            mSeekProfile.GetClock().Pop(token, std::chrono::steady_clock::now());
    }

    // 静音状态由这里持有；game_api 的入口本身不记状态。
    void MuteAudioForSeek() noexcept
    {
        if (mIsAudioMuted)
            return;
        mGame.MuteAudioForSeek();
        mIsAudioMuted = true;
    }

    void RestoreAudioAfterSeek() noexcept
    {
        if (!mIsAudioMuted)
            return;
        mGame.RestoreAudioAfterSeek();
        mIsAudioMuted = false;
    }

    // ---- 只读状态摘要 ----

    /**
     * 帧完成点按 ShouldRecordStateDigest 的时机只读计算一份世界摘要并写一行技术日志，供比对两次播放到同一帧的世界。
     * 耗时在快进中计入插件段。读取或格式化抛出异常时只停用本场的摘要并记一行日志，不影响回放。
     */
    void RecordStateDigest(std::uint32_t completedFrame, bool isSeekReached) noexcept
    {
        if (mIsStateDigestDisabled ||
            !plugin::ShouldRecordStateDigest(GetGateState(), completedFrame, isSeekReached, mLastStateDigestFrame))
            return;
        mLastStateDigestFrame = completedFrame;
        const auto token = PushSeekPhase(SeekPhase::Plugin);
        try
        {
            Log(game_api::FormatStateDigestLog(game_api::ReadStateDigest()).c_str());
        }
        catch (const std::exception& exception)
        {
            mIsStateDigestDisabled = true;
            char message[160]{};
            std::snprintf(message, sizeof(message), "Replay state digest disabled: %s", exception.what());
            Log(message);
        }
        catch (...)
        {
            mIsStateDigestDisabled = true;
            Log("Replay state digest disabled after an exception");
        }
        PopSeekPhase(token);
    }

    // ---- 时间表 ----

    /** 录像不带速度（v1–v3 单机）时，时间表按第 0 帧的运行时速度整段换算；在第一个完成帧读取一次，读取失败下一帧再试。 */
    void CaptureRuntimeGameSpeed()
    {
        if (mRuntimeGameSpeed)
            return;
        std::int32_t speed = 0;
        std::string error;
        if (mGame.ReadGameSpeed(speed, error))
            mRuntimeGameSpeed = speed;
    }

    /**
     * 游戏线程轮询扫描结果（每次 UpdateViewer）。成功时转成共享的不可变时间轴与总帧数；RuntimeGameSpeed 口径要等运行时速度。
     * 失败只记一行日志，总帧数保持未知（不能跳转），不影响回放。
     */
    void PollTimelineScan()
    {
        if (mTimeline || mHasTimelineFailed)
            return;
        if (!mPendingTimeline)
        {
            auto result = mTimelineScan.TakeResult();
            if (!result)
                return;
            if (!result->mBuilder)
            {
                ReportTimelineUnavailable(result->mError);
                return;
            }
            mPendingTimeline = std::move(result->mBuilder);
        }
        std::optional<int> runtimeGameSpeed;
        if (mPendingTimeline->NeedsRuntimeGameSpeed())
        {
            if (!mRuntimeGameSpeed)
                return;
            runtimeGameSpeed = mRuntimeGameSpeed;
        }
        const stats::TimelineTiming timing = mPendingTimeline->GetTiming();
        auto timeline = mPendingTimeline->TakeTimeline(runtimeGameSpeed);
        mPendingTimeline.reset();
        if (!timeline)
        {
            ReportTimelineUnavailable("the scanned timeline is empty");
            return;
        }
        mTotalFrames = timeline->GetFrameCount();
        mTimeline = std::make_shared<const viewer::GameTimeline>(std::move(*timeline));
        Log(FormatTimelineReadyLog(*mTimeline, timing).c_str());
    }

    void ReportTimelineUnavailable(const std::string& reason) noexcept
    {
        mHasTimelineFailed = true;
        mPendingTimeline.reset();
        try
        {
            const auto message = "Replay timeline unavailable: " + reason;
            Log(message.c_str());
        }
        catch (...) { Log("Replay timeline unavailable"); }
    }

    // 先轮询时间表扫描；回放状态全部取自 noexcept 的只读查询，在交给观看会话之前一次求出。
    void UpdateViewer() noexcept
    {
        try
        {
            PollTimelineScan();
        }
        catch (...) { ReportTimelineUnavailable("exception while taking the scan result"); }
        // 重开已请求、尚未执行时显示为快进中（可以取消），进度从 0 起；否则显示快进本身的状态。
        const auto gates = GetGateState();
        const PlaybackViewInput input{IsPlaybackViewActive(), mSession.GetState(), mSession.IsPlaybackHeld(),
            mSession.GetLastCompletedFrame(), mSession.GetRecordedControlGroups(),
            mRestart.GetRequestedView().value_or(mSeek.GetView()), plugin::CanSeekPlayback(gates),
            plugin::CanRestartInProcess(gates), mTotalFrames, mTimeline};
        mViewing.UpdateViewer(input);
    }

    bool Start()
    {
        if (mHasStartFailed)
            return false;
        replay::ReplaySessionInfo info;
        std::string error;
        if (!mGame.ReadSessionInfo(info, error))
            return Fail(error);
        if (IsPlayback())
        {
            if (!OpenPlaybackSource())
                return false;
            if (!mSession.StartPlayback(mReader))
            {
                return Fail(mSession.GetLastError());
            }
        }
        else if (!mWriter.Open(mRequest.mReplayPath.wstring(), target::TARGET_SHA256_BYTES,
                mRequest.mManifest, info, error) || !mSession.StartRecording(mWriter))
        {
            return Fail(error.empty() ? mSession.GetLastError() : error);
        }
        mIsStarted = true;
        // 进程内重开之后的开始：观看会话沿用重开前的观看设置，然后执行重开请求的后续动作（快进到目标或按原状态暂停）。
        const bool isAfterRestart = mRestart.IsAwaitingStart();
        Log(!IsPlayback() ? "Replay recording started" :
            isAfterRestart ? "Replay playback started after in-process restart" : "Replay playback started");
        if (!IsPlayback())
            mEventCaptureReporter.ReportContext(mGame.ReadEventCaptureContext());
        else if (isAfterRestart)
        {
            mViewing.BeginAfterRestart(mRequest.mReplayPath, static_cast<int>(mRecordedInfo.mLocalHouseIndex),
                mSession);
            ApplyRestartFollowUp();
        }
        else
        {
            ReportUnneutralizedOfflineRecording();
            mViewing.Begin(mRequest.mReplayPath, static_cast<int>(mRecordedInfo.mLocalHouseIndex), mSession);
        }
        return true;
    }

    // 回放端一律中和飞机影子绘制的副作用，而 v5 之前的单机录像在录制时没有中和，录制者镜头带来的副作用无法重现。
    void ReportUnneutralizedOfflineRecording()
    {
        const auto version = mRecordedInfo.mStreamVersion;
        const auto gameMode = mRecordedInfo.mGameMode;
        if (!replay::FrameCodec::IsOfflineGameMode(gameMode) ||
            replay::FrameCodec::IsRecordedWithAircraftDrawNeutralized(version, gameMode))
            return;
        const auto message = "Replay stream v" + std::to_string(version) + " (gameMode=" +
            std::to_string(gameMode) + ") was recorded without aircraft shadow draw neutralization (R1); "
            "matches with aircraft may not replay exactly";
        Log(message.c_str());
    }

    bool Fail(const std::string& error)
    {
        // 回放随即结束：丢弃待执行的进程内重开。快进中失败（含 FailPlayback）：恢复声音，不请求重绘。
        mRestart.Abandon();
        EndSeek(SeekEndReason::Failed, false);
        mTimelineScan.Stop();
        mViewing.Stop();
        if (!mIsStarted)
            mHasStartFailed = true;
        if (!mHasLoggedFailure)
        {
            Log(error.c_str());
            mHasLoggedFailure = true;
        }
        if (IsPlayback())
            mGame.EndReplay();
        else
            mWriter.Abort();
        return false;
    }

    void ReportFailure(bool success)
    {
        if (!success)
            Fail(mSession.GetLastError());
    }

    storage::ReplayRequest mRequest;
    game_api::GameApi mGame;
    storage::FrameReader mReader;
    storage::FrameWriter mWriter;
    replay::ReplaySession mSession;
    replay::ReplaySessionInfo mRecordedInfo;
    // 录制时也存在但不 Begin；构造与销毁时机和它接管的各成员原先相同。
    PlaybackViewingSession mViewing;
    EventCaptureReporter mEventCaptureReporter;
    // 快进状态（纯逻辑）与是否已静音；静音状态只在这里，game_api 的入口不记状态。
    PlaybackSeek mSeek;
    bool mIsAudioMuted = false;
    // 快进中原生输入更新的降频节拍，以及本次快进的分阶段耗时与积压诊断（每次开始快进时清零）。
    SeekInputThrottle mSeekInputThrottle;
    SeekProfile mSeekProfile;
    // 时间表：扫描线程只在回放时启动；结果取出后转成共享的不可变时间轴。
    PlaybackTimelineScan mTimelineScan;
    // 已取出、正等运行时速度的扫描结果（只在 RuntimeGameSpeed 口径下停留）。
    std::optional<stats::GameTimelineBuilder> mPendingTimeline;
    std::optional<std::uint32_t> mTotalFrames;
    std::shared_ptr<const viewer::GameTimeline> mTimeline;
    // 第一个完成帧读到的 GameOptions.GameSpeed，供不带速度的录像换算时间表。
    std::optional<int> mRuntimeGameSpeed;
    // 只读状态摘要：上次记录的帧号（同一帧不重复记录），以及读取异常后本场停用。
    std::optional<std::uint32_t> mLastStateDigestFrame;
    bool mIsStateDigestDisabled = false;
    // 进程内重开：待执行的请求与重开后的后续动作；首次载入时读到的会话快照（播放请求只读一次，读取失败也不重试，
    // 重开的载入不覆盖）及它能否用于重开（战役不能）；本次重开开始执行的时刻（单调时钟毫秒，开始播放时记录耗时后清空）。
    PlaybackRestart mRestart;
    std::optional<game_api::native::ScenarioSessionSnapshot> mSessionSnapshot;
    bool mHasReadSessionSnapshot = false;
    bool mCanRestartFromSnapshot = false;
    std::optional<std::int64_t> mRestartStartedAtMs;
    bool mHasTimelineFailed = false;
    bool mIsStarted = false;
    bool mIsPlaybackSourceOpen = false;
    bool mHasStartFailed = false;
    bool mHasLoggedFailure = false;
    bool mHasExitRequested = false;
    bool mHasLoggedAircraftShadowNeutralization = false;
};

std::unique_ptr<ReplayRuntime> g_Runtime;
bool g_StartupAttempted = false;
// 进程级：载入提速第一次生效时记一行日志，之后整个进程不再记（会话结束不复位）。
bool g_HasLoggedLoadBlitSleepSkip = false;
}

void OnStartup() noexcept
{
    if (g_StartupAttempted)
        return;
    g_StartupAttempted = true;
    try
    {
        target::TargetImage image;
        std::string error;
        const bool isTargetValid = image.LoadCurrentProcess(error);
        const auto gameDirectory = image.GetPath().empty()
            ? std::filesystem::current_path() : image.GetPath().parent_path();
        std::string diagnosticError;
        if (!OpenBootstrapDiagnostics(gameDirectory, diagnosticError))
            Log(diagnosticError.c_str());
        Log("Replay bootstrap entered");
        if (!isTargetValid)
        {
            Log("Target validation failed");
            Log(error.c_str());
            return;
        }
        const auto markerPath = gameDirectory / L"rareplay-runtime.bin";
        storage::ReplayRequest request;
        std::vector<std::uint8_t> rawBytes;
        if (!storage::ReadReplayRequest(markerPath, request, rawBytes, error))
        {
            Log("Replay request read failed");
            Log(error.c_str());
            return;
        }
        if (!storage::ConsumeReplayRequest(markerPath, rawBytes, error))
        {
            Log("Replay request consumption failed");
            Log(error.c_str());
            return;
        }
        g_Runtime = std::make_unique<ReplayRuntime>(std::move(request), gameDirectory);
        Log("Replay request consumed");
        if (g_Runtime->IsPlayback())
            g_Runtime->OpenPlaybackSource();
    }
    catch (...)
    {
        Log("Replay startup failed");
    }
}

bool TryGetPlaybackSeed(std::uint32_t& outSeed) noexcept
{
    OnStartup();
    try { return g_Runtime && g_Runtime->TryGetPlaybackSeed(outSeed); }
    catch (...) { OnHookFailure(); return false; }
}

void OnFrameBegin() noexcept
{
    OnStartup();
    try { if (g_Runtime) g_Runtime->BeginFrame(); }
    catch (...) { OnHookFailure(); }
}

void OnBeforeSimulation() noexcept
{
    try { if (g_Runtime) g_Runtime->BeforeSimulation(); }
    catch (...) { OnHookFailure(); }
}

ViewerLoopAction OnViewerLoopGate() noexcept
{
    try { return g_Runtime ? g_Runtime->ViewerLoopGate() : ViewerLoopAction::Continue; }
    catch (...) { OnHookFailure(); return ViewerLoopAction::Exit; }
}

AfterFrameLoopAction OnAfterFrameLoop() noexcept
{
    try { return g_Runtime ? g_Runtime->AfterFrameLoop() : AfterFrameLoopAction::NativeGameOver; }
    catch (...) { OnHookFailure(); return AfterFrameLoopAction::SkipGameOver; }
}

bool ShouldUseHalfShroud() noexcept
{
    return g_Runtime && g_Runtime->ShouldUseHalfShroud();
}

bool ShouldSkipViewerInputUpdate() noexcept
{
    return g_Runtime && g_Runtime->ShouldSkipViewerInputUpdate();
}

bool ShouldCapturePlayerInputEffect() noexcept
{
    return g_Runtime && g_Runtime->IsRecordingActive();
}

void OnPlayerInputEffectCaptured(const replay::RecordedInputEffect& effect) noexcept
{
    try { if (g_Runtime) g_Runtime->CapturePlayerInputEffect(effect); }
    catch (...) { OnHookFailure(); }
}

void OnPlayerInputEffectCaptureFailed() noexcept
{
    try
    {
        if (g_Runtime)
            g_Runtime->FailPlayerInputCapture("Unsupported movement feedback capture parameters");
    }
    catch (...) { OnHookFailure(); }
}

void OnControlGroupUpdated() noexcept
{
    try { if (g_Runtime) g_Runtime->CaptureControlGroupUpdate(); }
    catch (...) { OnHookFailure(); }
}

bool ShouldBlockPlayerCommands() noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->ShouldBlockPlayerCommands();
}

bool ShouldNeutralizeAircraftShadowSideEffects() noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->ShouldNeutralizeAircraftShadowSideEffects();
}

NativeSidebarAction OnNativeSidebarDraw() noexcept
{
    // 侧栏绘制只影响画面；任何异常都退回原生绘制，不把回放会话转为失败。
    try { return g_Runtime ? g_Runtime->UpdateNativeSidebar() : NativeSidebarAction::DrawNative; }
    catch (...) { return NativeSidebarAction::DrawNative; }
}

void OnSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
    const void* targetCell) noexcept
{
    // 观察只影响界面信息，不触发 OnStartup，也不把回放会话转为失败。
    if (g_Runtime)
        g_Runtime->RadarSuperWeaponFireBegin(house, super, prerequisiteSuper, targetCell);
}

void OnSuperWeaponFireEnd(const void* super) noexcept
{
    if (g_Runtime)
        g_Runtime->RadarSuperWeaponFireEnd(super);
}

void OnBuildingInfiltrated(const void* spy, const void* building) noexcept
{
    if (g_Runtime)
        g_Runtime->RadarBuildingInfiltrated(spy, building);
}

bool TrySetPlaybackViewingSpeed(int setting) noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->TrySetPlaybackViewingSpeed(setting);
}

bool TryGetPlaybackViewingSpeed(int& outSetting) noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->TryGetPlaybackViewingSpeed(outSetting);
}

bool ShouldHandlePlaybackViewingSpeed() noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->ShouldHandlePlaybackViewingSpeed();
}

void OnBeforeNativeEvents() noexcept
{
    try { if (g_Runtime) g_Runtime->BeforeNativeEvents(); }
    catch (...) { OnHookFailure(); }
}

void OnNativeEventDispatched(std::optional<std::uint32_t> ringIndex) noexcept
{
    try { if (g_Runtime) g_Runtime->CaptureDispatchedEvent(ringIndex); }
    catch (...) { OnHookFailure(); }
}

void OnNativeFrameParserReturned() noexcept
{
    try { if (g_Runtime) g_Runtime->AfterNativeFrameParser(); }
    catch (...) { OnHookFailure(); }
}

void OnFrameComplete() noexcept
{
    try { if (g_Runtime) g_Runtime->CompleteFrame(); }
    catch (...) { OnHookFailure(); }
}

bool OnPlaybackParser() noexcept
{
    try { return g_Runtime && g_Runtime->ProcessPlaybackFrame(); }
    catch (...) { OnHookFailure(); return g_Runtime && g_Runtime->IsPlayback(); }
}

void OnScenarioPreworldWait() noexcept
{
    OnStartup();
    // 运行时每进程只有一个（启动请求只消费一次），且跨进程内重开存活，所以它的第一次载入就是本进程的第一次载入。
    if (g_Runtime)
        g_Runtime->OnScenarioPreworldWait();
}

bool ShouldBypassPeerProgressWait() noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->IsPlayback();
}

bool ShouldSkipLoadBlitSleep() noexcept
{
    // 不调用 OnStartup：bootstrap 之前没有已接受的请求，WinMain 阶段的 DoBlit 保持原生等待。
    if (!g_Runtime || !g_Runtime->ShouldSkipLoadBlitSleep())
        return false;
    if (!g_HasLoggedLoadBlitSleepSkip)
    {
        g_HasLoggedLoadBlitSleepSkip = true;
        Log("Replay load blit sleep skipped");
    }
    return true;
}

void OnSessionEnd() noexcept
{
    try
    {
        if (g_Runtime)
        {
            g_Runtime->Finish();
            g_Runtime.reset();
        }
        CloseDiagnostics();
        g_StartupAttempted = false;
    }
    catch (...) { OnHookFailure(); }
}

void OnHookFailure() noexcept
{
    if (g_Runtime)
        g_Runtime->FailUnexpected();
}
}
