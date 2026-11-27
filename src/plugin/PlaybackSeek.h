#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <optional>

namespace rareplay::plugin
{
/**
 * 快进中不重绘战场，靠 HUD 自己的呈现刷新进度。当前会话的 HUD 呈现心跳超过这么久没有更新（或没有心跳）时，
 * 认为画面没有在刷新（显示驱动只在游戏绘制时才呈现、HUD 被 F10 隐藏或界面失败），改为兜底重绘。
 */
constexpr std::int64_t SEEK_HUD_STALE_MS = 1000;
/** 兜底重绘的间隔：两次兜底重绘之间至少相隔的真实时间。 */
constexpr std::int64_t SEEK_FALLBACK_DRAW_INTERVAL_MS = 1000;
/** 快进中原生输入更新（GetInputAndUpdate）完整执行的最小间隔，约等于 60 帧界面的一帧；其余帧跳过。 */
constexpr std::int64_t SEEK_INPUT_TICK_MS = 16;

enum class SeekEndReason
{
    /** 第目标帧已完成。 */
    Reached,
    /** 观看者取消，停在当前位置。 */
    Cancelled,
    /** 快进中回放读到结尾，停在结束状态。 */
    PlaybackEnded,
    /** 快进中回放失败。 */
    Failed,
    /** 观看者退出或重新播放，执行方先结束快进再走原有流程。 */
    Exited
};

/** 一次快进结束后交给执行方的结果。 */
struct SeekOutcome
{
    SeekEndReason mReason = SeekEndReason::Reached;
    std::uint32_t mStartFrame = 0;
    std::uint32_t mTargetFrame = 0;
    /** 结束时最后登记的完成帧。 */
    std::uint32_t mLastCompletedFrame = 0;
    /**
     * 应恢复的暂停状态：到达与取消时沿用跳转前的暂停状态；
     * 回放结束、失败与退出时为空，回放不再继续，没有可恢复的状态。
     */
    std::optional<bool> mPausedToRestore;
    /** 开始到结束的真实时间（毫秒），供跳转结束的日志使用；时钟回退时为 0。 */
    std::int64_t mElapsedMs = 0;
};

/**
 * 一次向前跳转（快进）的纯逻辑状态：目标、起点、跳转前的暂停状态、兜底重绘节拍、进度与停点。
 * 帧号与快照 mFrameNumber 同一单位（最后完成的帧号），快进在第“目标”帧完成后停下。
 * 只含值，不读游戏与时钟：当前时间与 HUD 呈现心跳的时刻由调用方以毫秒传入，两者应取自同一个单调时钟。
 * 由运行时在游戏线程持有并驱动，不做线程同步。
 */
class PlaybackSeek final
{
public:
    /**
     * 开始快进。已在快进中，或目标不大于最后完成帧时返回 false 且不改变状态（快进中改目标用 Retarget）。
     * hudPresentedAtMs 是开始时当前会话的 HUD 呈现心跳（没有时为空）。心跳新鲜时开始时刻算作“刚画过”，
     * 第一次兜底重绘最早在开始后满 SEEK_FALLBACK_DRAW_INTERVAL_MS 时进行；心跳本来就不存在或已超时，第一次兜底不必等待。
     */
    bool Begin(std::uint32_t lastCompletedFrame, std::uint32_t targetFrame, bool wasPaused, std::int64_t nowMs,
        std::optional<std::int64_t> hudPresentedAtMs) noexcept;

    /** 改目标：只在快进中、且新目标比当前目标更靠后时生效，返回是否改变。起点、跳转前的状态与兜底节拍不变。 */
    bool Retarget(std::uint32_t targetFrame) noexcept;

    /**
     * 帧完成点调用：登记最后完成帧（不回退），返回是否已到达目标（最后完成帧不小于目标）。
     * 到达后由调用方调用 End(SeekEndReason::Reached)；不在快进中时返回 false。
     */
    bool OnFrameCompleted(std::uint32_t lastCompletedFrame) noexcept;

    /**
     * 快进中的绘制门控：只有 HUD 心跳为空或已超过 SEEK_HUD_STALE_MS，且距上次兜底重绘（或开始时的“刚画过”）
     * 满 SEEK_FALLBACK_DRAW_INTERVAL_MS 时为真；其余一律为假（不绘制）。不在快进中时为假（照常绘制由调用方决定）。
     * 时钟回退（now 早于心跳或上次绘制）时按“未超时、未满间隔”处理。
     */
    bool ShouldDrawFallback(std::int64_t nowMs, std::optional<std::int64_t> hudPresentedAtMs) const noexcept;
    /** 本次兜底重绘已进行，下一次至少再等 SEEK_FALLBACK_DRAW_INTERVAL_MS。不在快进中时忽略。 */
    void MarkFallbackDrawn(std::int64_t nowMs) noexcept;

    /** 快进进度（0 到 1），算法同 viewer::GetSeekProgress；不在快进中时为 0。 */
    float GetProgress() const noexcept;

    /** 结束快进并回到空闲；不在快进中时返回空。 */
    std::optional<SeekOutcome> End(SeekEndReason reason, std::int64_t nowMs) noexcept;

    bool IsActive() const noexcept { return mIsActive; }
    std::uint32_t GetTargetFrame() const noexcept { return mTargetFrame; }
    /** 写入快照的快进状态；不在快进中时为默认值。 */
    viewer::PlaybackSeekView GetView() const noexcept;

private:
    bool mIsActive = false;
    std::uint32_t mStartFrame = 0;
    std::uint32_t mTargetFrame = 0;
    std::uint32_t mLastCompletedFrame = 0;
    bool mWasPaused = false;
    std::int64_t mBeginMs = 0;
    // 上次兜底重绘（或开始时心跳新鲜而算作的“刚画过”）的时刻；为空表示第一次兜底不必等待。
    std::optional<std::int64_t> mLastFallbackDrawMs;
};

/**
 * 快进中原生输入更新的降频节拍（纯逻辑，不读时钟）：快进中距上次完整执行不到 SEEK_INPUT_TICK_MS 时跳过，
 * 否则完整执行并记下这次的时刻。不在快进中时一律完整执行，并忘掉上次的时刻，所以快进开始后的第一次总是完整执行。
 * 只在游戏线程使用。
 */
class SeekInputThrottle final
{
public:
    /** 返回这次是否跳过；isSeeking 为假时不使用 nowMs。时钟回退时完整执行并以新时刻重新计时。 */
    bool ShouldSkip(bool isSeeking, std::int64_t nowMs) noexcept;

private:
    std::optional<std::int64_t> mLastFullRunMs;
};
}
