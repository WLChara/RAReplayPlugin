#include "PlaybackViewingSession.h"

#include "Diagnostics.h"
#include "../game_api/GameApi.h"
#include "../game_api/NativeSidebar.h"
#include "../game_api/NativeTooltips.h"
#include "../hooks/graphics/ReplayOverlayHook.h"
#include "../storage/PluginSettings.h"
#include "../storage/ViewerControlProtocol.h"

#include <chrono>

namespace rareplay::plugin
{
PlaybackViewingSession::PlaybackViewingSession(game_api::GameApi& game,
    const std::filesystem::path& gameDirectory, std::array<std::uint8_t, 16> requestId)
    : mGame(game), mViewerChannel(gameDirectory, requestId),
      mSettingsPath(gameDirectory / storage::PLUGIN_SETTINGS_FILE_NAME) {}

void PlaybackViewingSession::Begin(const std::filesystem::path& replayPath, int recordedLocalHouseIndex,
    replay::ReplaySession& session)
{
    mViewerChannel.Initialize();
    mIsHalfShroudEnabled = (mViewerChannel.GetInitialFlags() & storage::VIEWER_SHROUD_HALF) != 0;
    if (mIsHalfShroudEnabled)
        mGame.InvalidateViewerShroud();
    Log("Replay viewer command filtering active; native map input remains available");
    LoadHudSettings();
    // 缩放只在回放观看时启用；资源在首次离开 1× 时惰性创建，失败只停用缩放。
    mBattlefieldZoom.Begin();
    try
    {
        const auto name = replayPath.filename().u8string();
        mViewPublisher.Begin(std::string(name.begin(), name.end()), recordedLocalHouseIndex);
        // 统计只在回放时采样；经会话的帧观察者统计注入的操作事件。
        mStatsSession.Begin();
        session.SetFrameObserver(&mStatsSession);
        // 雷达事件与落点图标同样只在回放时创建，时间轴取统计的游戏时钟。
        mRadarSession.Begin();
    }
    catch (...) { mHasViewFailed = true; Log("Replay HUD initialization failed; playback continues"); }
}

void PlaybackViewingSession::Stop()
{
    ReleaseNativeTooltips();
    mBattlefieldZoom.Stop();
    mViewPublisher.End();
    mStatsSession.End();
    mRadarSession.End();
    mGame.ClearViewerShroud();
}

void PlaybackViewingSession::ReportFinalOverlayDiagnostics() noexcept
{
    mOverlayDiagnostics.Report(true);
}

void PlaybackViewingSession::OnPlaybackFrameCompleted(std::uint32_t frameNumber) noexcept
{
    // 统计只在回放播放中、帧刚完成时采样一次；暂停时帧号不推进，因此不会累加。
    mStatsSession.OnFrameCompleted(frameNumber);
    // 雷达事件以统计的游戏时钟为时间轴：本帧内观察到的超武与渗透在此打上帧完成后的游戏时间。
    mRadarSession.OnFrameCompleted(mStatsSession.GetGameTimeMs());
}

void PlaybackViewingSession::UpdateViewer(const PlaybackViewInput& input) noexcept
{
    mBattlefieldZoom.Update();
    const bool isSeeking = input.mSeek.mIsSeeking;
    // 先于界面失败判断：界面失败、退出或 HUD 隐藏时也要把原生提示交还。
    UpdateNativeTooltips(input.mIsPlaybackViewActive, isSeeking);
    // 界面失败只能停止界面，不能把已验证的回放会话转换为 Failed。
    if (mHasViewFailed)
        return;
    try
    {
        hooks::EnsureReplayOverlayStarted();
        mOverlayDiagnostics.Report();
        // 落点图标的屏幕位置每个主循环迭代都发布（不受 100 ms 快照节流约束），观看者滚动地图时才贴得住地面。
        // 快进中不投影，并撤回快进前的位置；快进结束后照常发布。
        if (isSeeking)
            mRadarSession.WithdrawMarks();
        else
            mRadarSession.PublishMarks(mViewPublisher.GetSessionId(), mStatsSession.GetGameTimeMs());
        int speed = 0;
        const std::optional<int> override = mGame.TryGetViewerPlaybackSpeed(speed)
            ? std::optional<int>(speed) : std::nullopt;
        viewer::PlaybackControls controls;
        controls.mStatus = input.mReplayState == replay::ReplayState::Ended
            ? viewer::PlaybackStatus::Ended : input.mIsPlaybackHeld
                ? viewer::PlaybackStatus::Paused : viewer::PlaybackStatus::Playing;
        controls.mLastCompletedFrame = input.mLastCompletedFrame;
        controls.mCanRestart = mViewerChannel.CanRestart();
        controls.mRestartError = mRestartError;
        controls.mIsHalfShroudEnabled = mIsHalfShroudEnabled;
        controls.mCanSeek = input.mCanSeek;
        controls.mSeek = input.mSeek;
        mViewPublisher.Publish(override, mGame.CanControlViewerPlaybackSpeed(), controls,
            input.mRecordedGroups, input.mTotalFrames, input.mTimeline, mStatsSession, mRadarSession,
            mBattlefieldZoom);
    }
    catch (...)
    {
        mHasViewFailed = true;
        mViewPublisher.End();
        Log("Replay HUD disabled after a snapshot exception; playback continues");
    }
}

NativeSidebarAction PlaybackViewingSession::UpdateNativeSidebar(bool isPlaybackViewActive, bool isSeeking) noexcept
{
    NativeSidebarGateInput input;
    input.mIsPlaybackActive = isPlaybackViewActive;
    const auto presence = ReadCurrentHudPresence();
    // 面板位置不唯一（例如 D3D9 留边无法判定）时 HUD 仍可见，但不能停止原生侧栏绘制。
    input.mIsHudVisible = presence && presence->mIsPanelCovering;
    input.mNow = std::chrono::steady_clock::now();
    // 快进中只改门控输入、不改阈值：心跳仍在就按刚呈现处理（呈现时刻取当前时刻）。
    if (input.mIsHudVisible)
        input.mPanelPresentedAt = isSeeking ? input.mNow : presence->mPresentedAt;
    const auto sidebar = game_api::ReadNativeSidebarState();
    input.mIsSidebarOnRight = sidebar.mIsOnRight;
    input.mIsArmageddonMode = sidebar.mIsArmageddonMode;
    const bool wasSkipping = mSidebarGate.IsSkipping();
    const auto action = mSidebarGate.Update(input);
    if (action == NativeSidebarAction::RedrawNative)
        game_api::RequestNativeSidebarRedraw();
    // 只记录前若干次切换，供实机核对门控，不在每帧写日志。
    if (wasSkipping != mSidebarGate.IsSkipping() && mSidebarLogCount < 16)
    {
        ++mSidebarLogCount;
        Log(mSidebarGate.IsSkipping() ? "Replay sidebar: native chrome skipped behind panel"
                                      : "Replay sidebar: native chrome restored with full redraw");
    }
    return action;
}

void PlaybackViewingSession::OnSuperWeaponFireBegin(const void* house, const void* super,
    const void* prerequisiteSuper, const void* targetCell) noexcept
{
    mRadarSession.OnSuperWeaponFireBegin(house, super, prerequisiteSuper, targetCell);
}

void PlaybackViewingSession::OnSuperWeaponFireEnd(const void* super) noexcept
{
    mRadarSession.OnSuperWeaponFireEnd(super);
}

void PlaybackViewingSession::OnBuildingInfiltrated(const void* spy, const void* building) noexcept
{
    mRadarSession.OnBuildingInfiltrated(spy, building);
}

bool PlaybackViewingSession::RequestRestart(bool isHalfShroudEnabled)
{
    const auto flags = isHalfShroudEnabled ? storage::VIEWER_SHROUD_HALF : 0;
    return mViewerChannel.RequestRestart(flags, mRestartError);
}

void PlaybackViewingSession::SetHalfShroudEnabled(bool isEnabled)
{
    mIsHalfShroudEnabled = isEnabled;
    mGame.InvalidateViewerShroud();
}

void PlaybackViewingSession::ApplyHudScale(viewer::HudScaleSetting setting)
{
    mViewPublisher.SetHudScale(setting);
    std::string error;
    if (!storage::SavePluginSettings(mSettingsPath, {static_cast<int>(setting)}, error))
    {
        const auto line = "Replay HUD settings save failed: " + error;
        Log(line.c_str());
    }
}

void PlaybackViewingSession::CenterOnRadarEvent(std::uint64_t eventId) noexcept
{
    mRadarSession.CenterOnEvent(eventId);
}

// 本会话的 HUD 呈现心跳；F10 隐藏或换会话时为空。
std::optional<viewer::HudPresence> PlaybackViewingSession::ReadCurrentHudPresence() const noexcept
{
    const std::uint64_t sessionId = mViewPublisher.GetSessionId();
    const auto presence = viewer::GetReplayViewExchange().ReadHudPresence();
    if (sessionId == 0 || !presence || presence->mSessionId != sessionId)
        return std::nullopt;
    return presence;
}

// 每帧在游戏线程检查一次：回放播放中且 HUD 可见（心跳未超时；快进中只要心跳仍在）时关闭原生提示，否则恢复原值。
void PlaybackViewingSession::UpdateNativeTooltips(bool isPlaybackViewActive, bool isSeeking) noexcept
{
    const auto current = game_api::ReadNativeTooltipState();
    if (!current)
        return;
    const auto presence = ReadCurrentHudPresence();
    const bool isHudVisible = presence &&
        (isSeeking || std::chrono::steady_clock::now() - presence->mPresentedAt <= PANEL_PRESENCE_TIMEOUT);
    if (const auto state = mTooltipGate.Update(isPlaybackViewActive && isHudVisible, *current))
        game_api::SetNativeTooltipState(*state);
}

// 会话结束或失败时交还原生提示；不再每帧检查之后由原生自行维护。
void PlaybackViewingSession::ReleaseNativeTooltips() noexcept
{
    const auto current = game_api::ReadNativeTooltipState();
    if (!current)
        return;
    if (const auto state = mTooltipGate.Update(false, *current))
        game_api::SetNativeTooltipState(*state);
}

// 只在播放开始时读取一次；缺文件为 100%，读取失败或值非法退回 100% 并记一行日志。
void PlaybackViewingSession::LoadHudSettings() noexcept
{
    auto hudScale = viewer::HudScaleSetting::Percent100;
    try
    {
        storage::PluginSettings settings;
        std::string message;
        const auto result = storage::LoadPluginSettings(mSettingsPath, settings, message);
        if (result == storage::PluginSettingsLoadResult::Invalid ||
            result == storage::PluginSettingsLoadResult::ReadFailed)
        {
            const auto line = "Replay HUD settings fallback to 100%: " + message;
            Log(line.c_str());
        }
        hudScale = viewer::ToHudScaleSetting(settings.mHudScalePercent).value_or(viewer::HudScaleSetting::Percent100);
    }
    catch (...) { Log("Replay HUD settings fallback to 100% after an exception"); }
    mViewPublisher.SetHudScale(hudScale);
}
}
