#pragma once

#include "BattlefieldZoomSession.h"
#include "NativeSidebarGate.h"
#include "NativeTooltipGate.h"
#include "OverlayDiagnosticsReporter.h"
#include "ReplayRadarSession.h"
#include "ReplayStatsSession.h"
#include "ReplayViewPublisher.h"
#include "../replay/RecordedControlGroups.h"
#include "../replay/ReplaySession.h"
#include "../storage/ViewerControlChannel.h"
#include "../viewer/ReplayViewExchange.h"
#include "../viewer/ReplayViewModel.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace rareplay::game_api
{
class GameApi;
}

namespace rareplay::plugin
{
/**
 * 每次更新观看呈现时由运行时传入的回放状态，只在本次调用内有效。
 * mRecordedGroups 引用回放会话自己的编队快照，不复制。
 */
struct PlaybackViewInput
{
    /** 见 RuntimeGates 的 IsPlaybackViewActive；由运行时在调用前按当前状态求出。 */
    bool mIsPlaybackViewActive;
    replay::ReplayState mReplayState;
    bool mIsPlaybackHeld;
    std::uint32_t mLastCompletedFrame;
    const std::optional<replay::RecordedControlGroupSnapshot>& mRecordedGroups;
    /** 运行时持有的快进状态（只读视图）。快进中不投影落点，原生提示与侧栏门控按 HUD 可见处理。 */
    viewer::PlaybackSeekView mSeek;
    /** 见 RuntimeGates 的 CanSeekPlayback；写进快照的 mCanSeek。 */
    bool mCanSeek = false;
    /** 时间表扫描得出的总帧数与时间轴；扫描未结束或失败时为空。 */
    std::optional<std::uint32_t> mTotalFrames;
    std::shared_ptr<const viewer::GameTimeline> mTimeline;
};

/**
 * 一次回放的观看呈现装配：快照发布、统计与雷达会话、战局缩放会话、原生提示与侧栏门控、HUD 设置、
 * 呈现诊断、半黑幕开关与重新播放通道。由 ReplayRuntime 作为普通成员持有，与它同时构造和销毁；
 * 录制时同样存在但不调用 Begin。只在游戏线程使用；不持有运行时，回放状态由运行时每次调用时传入。
 * 界面失败只停用界面（mHasViewFailed），不把回放会话转为失败。
 */
class PlaybackViewingSession final
{
public:
    /** game 必须比本对象活得长；构造时只创建观看通道对象并确定设置文件路径，不读写文件。 */
    PlaybackViewingSession(game_api::GameApi& game, const std::filesystem::path& gameDirectory,
        std::array<std::uint8_t, 16> requestId);

    /**
     * 回放开始时调用一次：初始化观看通道并按其初始标志设置半黑幕，读取 HUD 设置，
     * 启用战局缩放，再开始发布、统计（并登记为 session 的帧观察者）与雷达。
     * 观看通道初始化与设置半黑幕时的异常向调用方抛出；读取设置的异常只回退为 100%；
     * 发布、统计与雷达开始时的异常只停用界面。
     */
    void Begin(const std::filesystem::path& replayPath, int recordedLocalHouseIndex,
        replay::ReplaySession& session);

    /**
     * 回放结束、失败或意外异常时的观看收尾，录制时也调用：交还原生提示，停用战局缩放（写回物理视口、释放逻辑资源），
     * 结束发布、统计与雷达，清掉观看者黑幕。发布器结束时的异常向调用方抛出。
     */
    void Stop();

    /** 回放结束时强制输出一次呈现计数。 */
    void ReportFinalOverlayDiagnostics() noexcept;

    /** 回放帧完成点（状态为播放中）调用：先采样统计，再以统计的游戏时间登记本帧的雷达事件。 */
    void OnPlaybackFrameCompleted(std::uint32_t frameNumber) noexcept;

    /**
     * 每个主循环迭代调用：先写出战局缩放日志、更新原生提示，界面未失败时再发布落点（快进中改为撤回）与快照；
     * 异常只停用界面。
     */
    void UpdateViewer(const PlaybackViewInput& input) noexcept;

    /**
     * SidebarClass::Draw 跳过点每次调用一次；返回 RedrawNative 时已置原生重画标志。
     * 快进中（isSeeking）画面不绘制或只按兜底节拍绘制，HUD 呈现间隔可能超过心跳超时：只要本会话的面板心跳仍在
     * （未因 F10 或位置不确定撤回），就按刚呈现处理，不让侧栏在跳过与原生之间来回切换。
     */
    NativeSidebarAction UpdateNativeSidebar(bool isPlaybackViewActive, bool isSeeking) noexcept;

    // 雷达事件观察点：雷达会话未 Begin（录制或回放尚未开始）时这些调用立即返回。
    void OnSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
        const void* targetCell) noexcept;
    void OnSuperWeaponFireEnd(const void* super) noexcept;
    void OnBuildingInfiltrated(const void* spy, const void* building) noexcept;

    std::uint64_t GetSessionId() const noexcept { return mViewPublisher.GetSessionId(); }
    viewer::HudScaleSetting GetHudScale() const noexcept { return mViewPublisher.GetHudScale(); }
    bool IsHalfShroudEnabled() const noexcept { return mIsHalfShroudEnabled; }
    bool HasViewFailed() const noexcept { return mHasViewFailed; }
    /** 战局缩放：观看命令（HUD 按钮与 Ctrl+滚轮）经此换档与复原；状态随每次发布写进快照。 */
    BattlefieldZoomSession& GetBattlefieldZoom() noexcept { return mBattlefieldZoom; }

    /**
     * 请求启动器重新播放，isHalfShroudEnabled 写入请求标志。成功时返回 true，调用方负责结束回放；
     * 失败的原因保存在下一次发布的播放控制中。
     */
    bool RequestRestart(bool isHalfShroudEnabled);
    /** 调用方保证 isEnabled 与当前不同；设置后令观看者黑幕重新计算。 */
    void SetHalfShroudEnabled(bool isEnabled);
    /**
     * setting 已由路由校验且与当前档位不同。设置立即生效；写文件失败只记日志，
     * 不影响回放与本次已生效的档位。调用方负责捕获异常。
     */
    void ApplyHudScale(viewer::HudScaleSetting setting);
    void CenterOnRadarEvent(std::uint64_t eventId) noexcept;

    /**
     * 本会话的 HUD 最近一次实际呈现的时刻（渲染线程的心跳，取自 steady_clock）；F10 隐藏、界面未呈现或换会话时为空。
     * 快进的兜底重绘据此判断画面是否仍在刷新。
     */
    std::optional<std::chrono::steady_clock::time_point> ReadHudPresentedAt() const noexcept;

private:
    std::optional<viewer::HudPresence> ReadCurrentHudPresence() const noexcept;
    /** isSeeking 的处理同 UpdateNativeSidebar：心跳仍在即视为 HUD 可见，不看呈现间隔。 */
    void UpdateNativeTooltips(bool isPlaybackViewActive, bool isSeeking) noexcept;
    void ReleaseNativeTooltips() noexcept;
    void LoadHudSettings() noexcept;

    game_api::GameApi& mGame;
    ReplayViewPublisher mViewPublisher;
    ReplayStatsSession mStatsSession;
    ReplayRadarSession mRadarSession;
    BattlefieldZoomSession mBattlefieldZoom;
    storage::ViewerControlChannel mViewerChannel;
    std::filesystem::path mSettingsPath;
    std::string mRestartError;
    NativeSidebarGate mSidebarGate;
    NativeTooltipGate mTooltipGate;
    std::uint32_t mSidebarLogCount = 0;
    bool mHasViewFailed = false;
    OverlayDiagnosticsReporter mOverlayDiagnostics;
    bool mIsHalfShroudEnabled = false;
};
}
