#include "ReplayRuntime.h"

#include "Diagnostics.h"
#include "ReplayConfig.h"
#include "ReplayViewPublisher.h"
#include "OverlayDiagnosticsReporter.h"
#include "../hooks/graphics/ReplayOverlayHook.h"
#include "../viewer/ReplayViewExchange.h"
#include "../game_api/GameApi.h"
#include "../replay/ReplaySession.h"
#include "../storage/FrameReader.h"
#include "../storage/FrameWriter.h"
#include "../storage/ViewerControlChannel.h"
#include "../target/TargetImage.h"

#include <filesystem>
#include <memory>
#include <utility>

namespace rareplay::plugin
{
namespace
{
/** 游戏线程上唯一的启动请求、领域会话及文件句柄所有者。 */
class ReplayRuntime
{
public:
    ReplayRuntime(ReplayRequest request, const std::filesystem::path& gameDirectory)
        : mRequest(std::move(request)), mSession(mGame),
          mViewerChannel(gameDirectory, mRequest.mRequestId) {}

    bool IsPlayback() const { return mRequest.mMode == ReplayMode::Play; }
    bool IsStarted() const { return mIsStarted; }
    bool ShouldUseHalfShroud() const
    {
        const auto state = mSession.GetState();
        return mIsStarted && IsPlayback() && mIsHalfShroudEnabled && !mHasExitRequested &&
            (state == replay::ReplayState::Playing || state == replay::ReplayState::Paused ||
                state == replay::ReplayState::Ended);
    }
    bool IsRecordingActive() const
    {
        return mIsStarted && mRequest.mMode == ReplayMode::Record &&
            mSession.GetState() == replay::ReplayState::Recording;
    }
    bool ShouldBlockPlayerCommands() const
    {
        return IsPlayback() && mIsPlaybackSourceOpen && !mHasStartFailed;
    }
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

    bool OpenPlaybackSource()
    {
        if (!IsPlayback() || mHasStartFailed)
            return false;
        if (mIsPlaybackSourceOpen)
            return true;
        std::vector<std::uint8_t> metadata;
        std::string error;
        if (!mReader.Open(mRequest.mReplayPath.wstring(), target::TARGET_SHA256_BYTES,
                metadata, error))
            return Fail(error);
        if (!MatchesArchiveManifest(mRequest, metadata))
            return Fail("Replay request manifest differs from archive");
        if (!mReader.GetSessionInfo(mRecordedInfo, error))
            return Fail(error);
        mIsPlaybackSourceOpen = true;
        return true;
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
        mGame.BeginFrame();
        if (!mGame.IsWorldReady())
            return;
        if (!mIsStarted && !Start())
            return;
        ConsumeViewerCommands();
        if (mHasExitRequested)
            return;
        ReportFailure(mSession.OnFrameBegin());
    }

    ViewerLoopAction ViewerLoopGate()
    {
        if (!IsPlayback())
            return ViewerLoopAction::Continue;
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
            if (mSession.GetState() == replay::ReplayState::Failed)
                return ViewerLoopAction::Exit;
            if (mSession.IsPlaybackHeld())
                return ViewerLoopAction::Hold;
            return ViewerLoopAction::Continue;
        }
        mGame.DiscardLocalCommands();
        mGame.UpdatePausedCamera();
        UpdateViewer();
        return ViewerLoopAction::Hold;
    }

    bool AfterFrameLoop()
    {
        if (!mIsStarted || !IsPlayback())
            return false;
        ReportFailure(mSession.OnAfterFrameLoop());
        UpdateViewer();
        return mSession.IsPlaybackHeld() || mHasExitRequested ||
            mSession.GetState() == replay::ReplayState::Failed;
    }

    void BeforeNativeEvents()
    {
        if (!mIsStarted || mRequest.mMode != ReplayMode::Record)
            return;
        mGame.BeforeNativeEventExecution();
        ReportFailure(mSession.OnBeforeEvents());
    }

    void BeforeSimulation()
    {
        if (mIsStarted && IsPlayback() &&
            mSession.GetState() == replay::ReplayState::Playing)
            ReportFailure(mSession.OnBeforeSimulation());
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

    void CompleteFrame()
    {
        if (mIsStarted)
            ReportFailure(mSession.OnFrameEnd());
        if (mIsStarted && IsPlayback() && mSession.GetState() == replay::ReplayState::Playing)
            UpdateViewer();
    }

    bool ProcessPlaybackFrame()
    {
        if (!IsPlayback())
            return false;
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
        return true;
    }

    void Finish()
    {
        if (IsPlayback())
            mOverlayDiagnostics.Report(true);
        mViewPublisher.End();
        mGame.ClearViewerShroud();
        if (mIsStarted && mSession.GetState() == replay::ReplayState::Recording)
        {
            const bool success = mSession.Finish();
            ReportFailure(success);
            if (success)
                Log("Replay recording finalized");
        }
        else
            mSession.Stop();
        mReader.Close();
    }

    void FailUnexpected()
    {
        mViewPublisher.End();
        mGame.ClearViewerShroud();
        mSession.Stop();
        if (IsPlayback())
            mGame.EndReplay();
        Log("Unexpected exception in replay hook");
    }

private:
    void ConsumeViewerCommands() noexcept
    {
        if (!IsPlayback())
            return;
        try
        {
            for (const auto& command : viewer::GetReplayViewExchange().Drain())
            {
                if (command.mSessionId != mViewPublisher.GetSessionId())
                    continue;
                if (command.mKind == viewer::ViewerCommandKind::SetPlaybackSpeed)
                    TrySetPlaybackViewingSpeed(command.mValue);
                else if (command.mKind == viewer::ViewerCommandKind::SetPaused)
                    mSession.SetPaused(command.mValue != 0);
                else if (command.mKind == viewer::ViewerCommandKind::RestartPlayback && !mHasExitRequested)
                {
                    const auto flags = mIsHalfShroudEnabled ? storage::VIEWER_SHROUD_HALF : 0;
                    if (mViewerChannel.RequestRestart(flags, mRestartError))
                    {
                        mHasExitRequested = true;
                        mGame.EndReplay();
                    }
                }
                else if (command.mKind == viewer::ViewerCommandKind::SetHalfShroud && !mHasExitRequested)
                {
                    const bool enabled = command.mValue != 0;
                    if (enabled != mIsHalfShroudEnabled)
                    {
                        mIsHalfShroudEnabled = enabled;
                        mGame.InvalidateViewerShroud();
                    }
                }
                else if (command.mKind == viewer::ViewerCommandKind::ExitPlayback && !mHasExitRequested)
                {
                    mHasExitRequested = true;
                    mGame.EndReplay();
                }
            }
        }
        catch (...) { Log("Replay HUD command was ignored after an exception"); }
    }

    void UpdateViewer() noexcept
    {
        // 界面失败只能停止界面，不能把已验证的回放会话转换为 Failed。
        if (mHasViewFailed)
            return;
        try
        {
            hooks::EnsureReplayOverlayStarted();
            mOverlayDiagnostics.Report();
            int speed = 0;
            const std::optional<int> override = mGame.TryGetViewerPlaybackSpeed(speed)
                ? std::optional<int>(speed) : std::nullopt;
            viewer::PlaybackControls controls;
            controls.mStatus = mSession.GetState() == replay::ReplayState::Ended
                ? viewer::PlaybackStatus::Ended : mSession.IsPlaybackHeld()
                    ? viewer::PlaybackStatus::Paused : viewer::PlaybackStatus::Playing;
            controls.mLastCompletedFrame = mSession.GetLastCompletedFrame();
            controls.mCanRestart = mViewerChannel.CanRestart();
            controls.mRestartError = mRestartError;
            controls.mIsHalfShroudEnabled = mIsHalfShroudEnabled;
            mViewPublisher.Publish(override, mGame.CanControlViewerPlaybackSpeed(), controls);
        }
        catch (...)
        {
            mHasViewFailed = true;
            mViewPublisher.End();
            Log("Replay HUD disabled after a snapshot exception; playback continues");
        }
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
        Log(IsPlayback() ? "Replay playback started" : "Replay recording started");
        if (IsPlayback())
        {
            mViewerChannel.Initialize();
            mIsHalfShroudEnabled = (mViewerChannel.GetInitialFlags() & storage::VIEWER_SHROUD_HALF) != 0;
            if (mIsHalfShroudEnabled)
                mGame.InvalidateViewerShroud();
            Log("Replay viewer command filtering active; native map input remains available");
            try
            {
                const auto name = mRequest.mReplayPath.filename().u8string();
                mViewPublisher.Begin(std::string(name.begin(), name.end()),
                    static_cast<int>(mRecordedInfo.mLocalHouseIndex));
            }
            catch (...) { mHasViewFailed = true; Log("Replay HUD initialization failed; playback continues"); }
        }
        return true;
    }

    bool Fail(const std::string& error)
    {
        mViewPublisher.End();
        mGame.ClearViewerShroud();
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

    ReplayRequest mRequest;
    game_api::GameApi mGame;
    storage::FrameReader mReader;
    storage::FrameWriter mWriter;
    replay::ReplaySession mSession;
    replay::ReplaySessionInfo mRecordedInfo;
    ReplayViewPublisher mViewPublisher;
    storage::ViewerControlChannel mViewerChannel;
    std::string mRestartError;
    bool mHasViewFailed = false;
    OverlayDiagnosticsReporter mOverlayDiagnostics;
    bool mIsStarted = false;
    bool mIsPlaybackSourceOpen = false;
    bool mHasStartFailed = false;
    bool mHasLoggedFailure = false;
    bool mHasExitRequested = false;
    bool mIsHalfShroudEnabled = false;
};

std::unique_ptr<ReplayRuntime> g_Runtime;
bool g_StartupAttempted = false;
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
        ReplayRequest request;
        std::vector<std::uint8_t> rawBytes;
        if (!ReadReplayRequest(markerPath, request, rawBytes, error))
        {
            Log("Replay request read failed");
            Log(error.c_str());
            return;
        }
        if (!ConsumeReplayRequest(markerPath, rawBytes, error))
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

bool OnAfterFrameLoop() noexcept
{
    try { return g_Runtime && g_Runtime->AfterFrameLoop(); }
    catch (...) { OnHookFailure(); return true; }
}

bool ShouldUseHalfShroud() noexcept
{
    return g_Runtime && g_Runtime->ShouldUseHalfShroud();
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

bool ShouldBlockPlayerCommands() noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->ShouldBlockPlayerCommands();
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

bool ShouldBypassPeerProgressWait() noexcept
{
    OnStartup();
    return g_Runtime && g_Runtime->IsPlayback();
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
