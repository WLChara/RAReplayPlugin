#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <optional>

namespace rareplay::plugin
{
/** 进程内重开的来源。 */
enum class RestartSource
{
    /** 底栏“重新播放”：重开到开头后开始播放（不暂停）。前提不满足时退回启动器重启。 */
    Replay,
    /** 向后跳转：重开后快进到目标帧，到达后沿用跳转前的暂停状态。前提不满足时不执行。 */
    BackwardSeek
};

/** 一次进程内重开请求，只含值。 */
struct RestartRequest
{
    RestartSource mSource = RestartSource::Replay;
    /**
     * 重开后快进到的帧，与快照 mFrameNumber 同一单位（第 T 帧完成后停下）。为空或为 0 时不快进：
     * 开始播放后直接按 mIsPausedAfterRestart 暂停或继续（停在第 0 帧之前，即开头）。重新播放恒为空。
     */
    std::optional<std::uint32_t> mTargetFrame;
    /** 到达目标后（不快进时为开始播放后）是否暂停：重新播放为假，向后跳转为跳转前的暂停状态。 */
    bool mIsPausedAfterRestart = false;
};

/** 重开后开始播放时要接着做的事。 */
struct RestartFollowUp
{
    /** 开始播放后立即快进到的帧，必大于 0；为空时不快进。 */
    std::optional<std::uint32_t> mSeekTargetFrame;
    /** 快进时作为“跳转前的暂停状态”，到达后恢复；不快进时开始播放后立即按它暂停或继续。 */
    bool mIsPaused = false;
};

/** 收到重开请求时的处理。 */
enum class RestartDecision
{
    /** 记下请求，在下一次 GAME_OVER_CALL 执行点（主循环两次迭代之间）进程内重开。 */
    Execute,
    /** 只用于重新播放：前提不满足，走原有的“关闭游戏、由启动器重启”流程。 */
    FallBackToLauncher,
    /** 只用于向后跳转：前提不满足，不执行（此时界面也不提供向后跳转）。 */
    Reject
};

/**
 * canRestartInProcess 为 RuntimeGates 的 CanRestartInProcess。满足时一律 Execute；
 * 不满足时重新播放退回启动器，向后跳转拒绝。
 */
RestartDecision DecideRestart(RestartSource source, bool canRestartInProcess) noexcept;

/**
 * 进程内重开（同进程重新播放与向后跳转）的纯逻辑状态：
 * 空闲 →（观看命令记下请求）已请求 →（执行点取出并完成重开）等待开始播放 →（下一帧开始播放，交出后续动作）空闲。
 * 已请求期间的新请求整体改写旧请求，取消则撤销；执行点放弃或重开失败时回到空闲。
 * 只含值，不读游戏与时钟；由运行时在游戏线程持有并驱动，不做线程同步。
 */
class PlaybackRestart final
{
public:
    /**
     * 记下请求。已有尚未执行的请求时以这一次整体改写（目标帧覆盖；请求期间暂停命令被路由拦下，
     * 所以暂停状态与上一次相同）。已执行、等待开始播放时不接受（此时不消费观看命令），返回 false 且不变。
     */
    bool Request(const RestartRequest& request) noexcept;

    /** 撤销尚未执行的请求，返回是否撤销了；没有请求或已执行时不变。 */
    bool Cancel() noexcept;

    /** 尚未执行的请求；没有时为空。 */
    std::optional<RestartRequest> GetPendingRequest() const noexcept;

    /**
     * 执行点开始执行：取出尚未执行的请求并转入“等待开始播放”；没有请求时返回空且不变。
     * 取出后放弃执行（前提不再满足或准备失败）或原生重开失败时，调用方须调用 Abandon。
     */
    std::optional<RestartRequest> BeginExecution() noexcept;

    /** 回到空闲：丢弃请求或等待中的后续动作。任何阶段都可调用。 */
    void Abandon() noexcept;

    /**
     * 重开后开始播放成功时调用：交出后续动作并回到空闲。目标帧为空或为 0 时后续动作不快进。
     * 不在等待开始播放时返回空且不变。
     */
    std::optional<RestartFollowUp> OnPlaybackStarted() noexcept;

    bool IsRequested() const noexcept { return mPhase == Phase::Requested; }
    bool IsAwaitingStart() const noexcept { return mPhase == Phase::AwaitingStart; }

    /**
     * 请求尚未执行时写入快照的快进状态：快进中、mIsReloading 为真、起点 0、目标为请求的目标帧（重新播放为 0）。
     * 没有尚未执行的请求时为空，调用方改用 PlaybackSeek 的状态。
     */
    std::optional<viewer::PlaybackSeekView> GetRequestedView() const noexcept;

private:
    enum class Phase
    {
        Idle,
        Requested,
        AwaitingStart
    };

    Phase mPhase = Phase::Idle;
    RestartRequest mRequest;
};
}
