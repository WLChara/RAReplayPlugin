#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <optional>

namespace rareplay::plugin
{
/** 路由一条观看命令时所需的当前状态；只含值，由运行时在执行每条命令前重新填写。 */
struct ViewerRouterState
{
    /** 当前发布会话；与命令携带的会话 ID 不同的命令丢弃。 */
    std::uint64_t mSessionId = 0;
    bool mHasExitRequested = false;
    bool mIsHalfShroudEnabled = false;
    viewer::HudScaleSetting mHudScale = viewer::HudScaleSetting::Percent100;
    /** 最后完成的帧号，与快照 mFrameNumber 同一单位。 */
    std::uint32_t mLastCompletedFrame = 0;
    /** 回放总帧数；未知时为空，跳转命令一律拦下。 */
    std::optional<std::uint32_t> mTotalFrames;
    /** 正在快进（向前跳转）。 */
    bool mIsSeeking = false;
    /** 正在进行的快进的目标帧；只在 mIsSeeking 时有效，用来判断新目标是否更靠后。 */
    std::uint32_t mSeekTargetFrame = 0;
    /** 回放会话处于暂停（结束不算暂停）。快进中会话为播放，此值为假。 */
    bool mIsPaused = false;
    /** 快进开始前保存的暂停状态（PlaybackSeek::WasPausedBeforeSeek）；只在 mIsSeeking 时有效。 */
    bool mSeekWasPaused = false;
    /** 进程内重开已请求、尚未执行。 */
    bool mIsRestartPending = false;
    /** 尚未执行的重开的目标帧（重新播放为空）；只在 mIsRestartPending 时有效。 */
    std::optional<std::uint32_t> mPendingRestartTargetFrame;
};

enum class ViewerActionKind
{
    SetPlaybackSpeed,
    SetPaused,
    /**
     * 重新播放：进程内重开的前提满足时记下重开请求（重开到开头并开始播放）；否则请求启动器重新播放，
     * 只有请求成功时执行方才置退出标志并结束回放。
     */
    RequestRestart,
    SetHalfShroud,
    ExitPlayback,
    SetHudScale,
    CenterOnRadarEvent,
    /** 战局缩放换档（AdjustBattlefieldZoom 与 AdjustBattlefieldZoomAtCenter 两种命令）；档位范围由游戏线程上的缩放适配器夹取。 */
    AdjustBattlefieldZoom,
    /** 战局缩放复原到 1×。 */
    ResetBattlefieldZoom,
    /** 向前跳转到 mSeekTargetFrame：不在快进中时开始快进，快进中改目标（mIsSeekRetarget）。 */
    SeekToFrame,
    /**
     * 经进程内重开跳转到 mSeekTargetFrame（可为 0），到达后按 mIsPausedAfterSeek 暂停或继续：
     * 向后跳转（目标在最后完成帧之前），以及重开已请求、尚未执行时的任何新目标（改写待执行的请求）。
     * 前提不满足时执行方不执行。
     */
    SeekViaRestart,
    /** 结束正在进行的快进，停在当前位置；进程内重开尚未执行时撤销该请求。 */
    CancelSeek
};

/**
 * 单条换档命令的档数上限（两个方向）。档位表共 12 档，更大的档数与它等效；
 * 夹取只是不把任意 int 原样交给游戏线程。
 */
constexpr int MAX_BATTLEFIELD_ZOOM_STEPS = 12;

/** 一条命令路由后的待执行动作；只有与 mKind 对应的字段有意义。 */
struct ViewerAction
{
    ViewerActionKind mKind = ViewerActionKind::SetPlaybackSpeed;
    /** SetPlaybackSpeed：命令值原样转交，由游戏适配层校验。 */
    int mPlaybackSpeedSetting = 0;
    /** SetPaused：命令值非零即暂停。 */
    bool mIsPaused = false;
    /** RequestRestart：重新播放时沿用的当前半黑幕开关；SetHalfShroud：新的开关值（必与当前不同）。 */
    bool mIsHalfShroudEnabled = false;
    /** SetHudScale：已校验、且与当前不同的档位。 */
    viewer::HudScaleSetting mHudScale = viewer::HudScaleSetting::Percent100;
    /** CenterOnRadarEvent：命令值按 std::uint64_t 转换后的事件编号。 */
    std::uint64_t mRadarEventId = 0;
    /** AdjustBattlefieldZoom：非零档数，已夹到 [-MAX_BATTLEFIELD_ZOOM_STEPS, MAX_BATTLEFIELD_ZOOM_STEPS]，正数放大。 */
    int mZoomSteps = 0;
    /**
     * AdjustBattlefieldZoom：为真时以应用时鼠标指着的地面点为锚点（Ctrl+滚轮的 AdjustBattlefieldZoom 命令），
     * 为假时以战术区中心为锚点（HUD 按钮的 AdjustBattlefieldZoomAtCenter 命令）。
     */
    bool mIsZoomAnchoredAtPointer = false;
    /**
     * SeekToFrame：目标帧，已夹到最后一帧（总帧数 − 1），且大于最后完成帧；快进中还大于当前目标。
     * SeekViaRestart：目标帧，已夹到最后一帧；不在待执行重开中时小于最后完成帧，在其中时与待执行的目标不同。
     */
    std::uint32_t mSeekTargetFrame = 0;
    /** SeekToFrame：为真时改正在进行的快进的目标，为假时开始新的快进。 */
    bool mIsSeekRetarget = false;
    /** SeekViaRestart：跳转前的暂停状态，快进中取快进开始前保存的状态，否则取当前会话是否暂停。 */
    bool mIsPausedAfterSeek = false;
};

/**
 * 按当前状态把单条观看命令转换为待执行动作；为空表示本条命令不执行任何操作。
 * 后一条命令依赖前一条执行后的状态（例如退出后的重新播放被拦下），所以调用方必须逐条路由、
 * 执行并更新状态后再路由下一条，不能一次路由整批。
 * “已请求退出”之后：调速、暂停与界面缩放照常路由；重新播放、半黑幕、退出、镜头跳转、战局缩放（两种换档、复原）
 * 与两种跳转命令都被拦下，其中镜头跳转与战局缩放会移动镜头并创建或释放缩放资源。
 * 两种换档命令都路由为 AdjustBattlefieldZoom，只是锚点不同；档数为 0 的丢弃。
 * 跳转：总帧数未知或为 0 时拦下 SeekToFrame；负值丢弃，超过最后一帧的按最后一帧。之后：
 * - 进程内重开已请求、尚未执行：与待执行目标相同的丢弃，其余一律路由为 SeekViaRestart（改写待执行的请求）；
 * - 否则小于最后完成帧的路由为 SeekViaRestart（向后跳转，快进中同样如此），等于最后完成帧的丢弃；
 * - 大于最后完成帧的向前跳转：快进中只有比当前目标更靠后的目标路由为“改目标”，其余丢弃。
 * SeekViaRestart 的暂停状态：快进中取快进开始前保存的状态（mSeekWasPaused），否则取 mIsPaused。
 * 路由不判定进程内重开的前提，由执行方判定。CancelSeek 只在快进中或重开已请求时有效。
 * 快进中或重开已请求时再丢弃暂停、半黑幕、镜头跳转与战局缩放（两种换档、复原）；调速、界面缩放、退出与重新播放照常路由，
 * 退出与重新播放由执行方先结束快进（退出还撤销待执行的重开）。
 */
std::optional<ViewerAction> RouteViewerCommand(const viewer::ViewerCommand& command,
    const ViewerRouterState& state) noexcept;
}
