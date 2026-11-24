#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <optional>

namespace rareplay::plugin
{
/** 快进期间两次绘制之间至少间隔的真实时间（约每秒刷新 10 次）。 */
constexpr std::int64_t SEEK_DRAW_INTERVAL_MS = 100;

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
 * 一次向前跳转（快进）的纯逻辑状态：目标、起点、跳转前的暂停状态、绘制节拍、进度与停点。
 * 帧号与快照 mFrameNumber 同一单位（最后完成的帧号），快进在第“目标”帧完成后停下。
 * 只含值，不读游戏与时钟：当前时间由调用方以毫秒传入，应取自单调时钟。由运行时在游戏线程持有并驱动，不做线程同步。
 */
class PlaybackSeek final
{
public:
    /**
     * 开始快进。已在快进中，或目标不大于最后完成帧时返回 false 且不改变状态（快进中改目标用 Retarget）。
     * 绘制节拍：开始时刻算作“刚画过”。开始前一帧刚按正常流程画过画面，带快进状态的快照也要等下一次发布，
     * 所以第一次快进中的绘制在开始后满 SEEK_DRAW_INTERVAL_MS 时进行。
     */
    bool Begin(std::uint32_t lastCompletedFrame, std::uint32_t targetFrame, bool wasPaused, std::int64_t nowMs) noexcept;

    /** 改目标：只在快进中、且新目标比当前目标更靠后时生效，返回是否改变。起点与跳转前的状态不变。 */
    bool Retarget(std::uint32_t targetFrame) noexcept;

    /**
     * 帧完成点调用：登记最后完成帧（不回退），返回是否已到达目标（最后完成帧不小于目标）。
     * 到达后由调用方调用 End(SeekEndReason::Reached)；不在快进中时返回 false。
     */
    bool OnFrameCompleted(std::uint32_t lastCompletedFrame) noexcept;

    /** 绘制节拍：快进中且距上次绘制满 SEEK_DRAW_INTERVAL_MS 时为真；不在快进中时为假（照常绘制由调用方决定）。 */
    bool ShouldDraw(std::int64_t nowMs) const noexcept;
    /** 本次绘制已进行，下一次要再等 SEEK_DRAW_INTERVAL_MS。不在快进中时忽略。 */
    void MarkDrawn(std::int64_t nowMs) noexcept;

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
    std::int64_t mLastDrawMs = 0;
};
}
