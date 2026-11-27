#pragma once

#include "../game_api/native/NativeSeekBacklog.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

/**
 * 快进（不重绘战场）期间的纯逻辑：积压清理的时机与阈值，以及各阶段耗时、积压峰值与清理次数的累计，
 * 结束时汇成一行技术日志，用来判断下一步该优化哪一段。不读游戏与时钟，时刻由调用方传入；只在游戏线程使用。
 */
namespace rareplay::plugin
{
/** 超过即在帧完成点整体删除的条数：电弧与光束各 32，尾迹（只删已解绑的）64。 */
constexpr std::int32_t SEEK_EBOLT_LIMIT = 32;
constexpr std::int32_t SEEK_RADBEAM_LIMIT = 32;
constexpr std::int32_t SEEK_LINE_TRAIL_LIMIT = 64;

enum class SeekCleanupPoint
{
    /** 快进中每帧的帧完成点（本帧记录处理之后）：脏矩形清零，三种特效按阈值清理。 */
    FrameCompleted,
    /** 兜底重绘之前：脏矩形、电弧、光束全部清理，尾迹删除已解绑的记录。 */
    BeforeFallbackDraw,
    /** 结束快进、完整重绘之前：同兜底，另把仍挂在物体上的尾迹隐去，并清空雷达事件与屏震。 */
    SeekEnd
};

/** 某个时机按当前条数要做的清理。 */
game_api::native::SeekBacklogCleanup PlanSeekBacklogCleanup(SeekCleanupPoint point,
    const game_api::native::SeekBacklogCounts& counts) noexcept;

/**
 * 快进中一帧按主循环的回调点分成的阶段：
 * Input 为帧开始（0x55D360）到绘制门控（0x55D8F2），含原生输入与插件的帧开始处理；
 * Simulation 为门控到帧解析（0x55DE40），经过模拟前（0x55DC99）与 Logic，兜底重绘的 DrawOnTop 也在其中；
 * Events 为回放帧解析的处理内（执行本帧录制的事件）；Tail 为帧解析返回到帧完成（0x55DE7E）；
 * Plugin 为插件的逐帧观看工作（统计与雷达会话的帧完成采样、快照与落点发布）；Cleanup 为积压清理；
 * Other 为帧完成之后到下一帧开始的其余时间（限速判断、MainLoop 返回后的处理）。Plugin 与 Cleanup 嵌套在其他阶段内，
 * 计入它们自己而不计入外层，所以各阶段互不重叠，合计约等于快进的真实时间。
 */
enum class SeekPhase : std::size_t
{
    Input,
    Simulation,
    Events,
    Tail,
    Plugin,
    Cleanup,
    Other,
    Count
};

/**
 * 分阶段的挂钟累计器：每次切换阶段时，把上一个边界到此刻的时间记到当前阶段。空闲（未开始或已停止）时
 * Enter 开始计时，Push 与 Pop 不起作用；时钟回退的一段按 0 计。
 */
class SeekPhaseClock final
{
public:
    using Clock = std::chrono::steady_clock;
    /** Push 的返回值：嵌套前的阶段；空闲时为空，Pop 时据此回到原阶段。 */
    using Token = std::optional<SeekPhase>;

    /** 切换到 phase（主循环的阶段边界）；空闲时从 now 开始计时。 */
    void Enter(SeekPhase phase, Clock::time_point now) noexcept;
    /** 进入嵌套阶段；空闲时什么也不做并返回空。 */
    Token Push(SeekPhase phase, Clock::time_point now) noexcept;
    /** 回到 Push 前的阶段；token 为空或其间已 Stop 时什么也不做。 */
    void Pop(Token token, Clock::time_point now) noexcept;
    /** 记下当前阶段并回到空闲。 */
    void Stop(Clock::time_point now) noexcept;
    void Reset() noexcept;

    bool IsRunning() const noexcept { return mCurrent.has_value(); }
    Clock::duration GetTotal(SeekPhase phase) const noexcept;

private:
    void Accumulate(Clock::time_point now) noexcept;

    std::array<Clock::duration, static_cast<std::size_t>(SeekPhase::Count)> mTotals{};
    std::optional<SeekPhase> mCurrent;
    Clock::time_point mSince{};
};

/** 一次快进的诊断累计：阶段耗时、兜底重绘次数、各积压的峰值与清理次数（只计实际删掉了记录的那几次）。 */
class SeekProfile final
{
public:
    void Reset() noexcept;
    SeekPhaseClock& GetClock() noexcept { return mClock; }
    const SeekPhaseClock& GetClock() const noexcept { return mClock; }

    /** 清理前读到的条数：更新各峰值。 */
    void ObserveBacklog(const game_api::native::SeekBacklogCounts& counts) noexcept;
    void RecordCleanup(const game_api::native::SeekBacklogCleanupResult& result) noexcept;
    void RecordFallbackDraw() noexcept { ++mFallbackDraws; }

    std::uint32_t GetFallbackDraws() const noexcept { return mFallbackDraws; }
    const game_api::native::SeekBacklogCounts& GetPeaks() const noexcept { return mPeaks; }

    /**
     * 结束时的一行英文技术日志：
     * "Replay seek timing: frames=N inputMs=.. simMs=.. eventsMs=.. tailMs=.. pluginMs=.. cleanupMs=.. otherMs=.. draws=K
     * dirtyPeak=.. eboltPeak=.. eboltClears=.. radBeamPeak=.. radBeamClears=.. lineTrailPeak=.. lineTrailClears=..
     * radarEventPeak=.."；毫秒保留一位小数，draws 是兜底重绘的次数。
     */
    std::string FormatTimingLog(std::uint32_t frames) const;

private:
    SeekPhaseClock mClock;
    game_api::native::SeekBacklogCounts mPeaks;
    std::uint32_t mFallbackDraws = 0;
    std::uint32_t mEBoltClears = 0;
    std::uint32_t mRadBeamClears = 0;
    std::uint32_t mLineTrailClears = 0;
};
}
