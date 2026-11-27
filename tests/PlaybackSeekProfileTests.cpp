#include "../src/plugin/PlaybackSeekProfile.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::game_api::native::SeekBacklogCleanup;
using rareplay::game_api::native::SeekBacklogCleanupResult;
using rareplay::game_api::native::SeekBacklogCounts;
using rareplay::plugin::PlanSeekBacklogCleanup;
using rareplay::plugin::SEEK_EBOLT_LIMIT;
using rareplay::plugin::SEEK_LINE_TRAIL_LIMIT;
using rareplay::plugin::SEEK_RADBEAM_LIMIT;
using rareplay::plugin::SeekCleanupPoint;
using rareplay::plugin::SeekPhase;
using rareplay::plugin::SeekPhaseClock;
using rareplay::plugin::SeekProfile;
using Clock = SeekPhaseClock::Clock;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Clock::time_point At(std::int64_t microseconds)
{
    return Clock::time_point{} + std::chrono::microseconds(microseconds);
}

std::int64_t Microseconds(const SeekPhaseClock& clock, SeekPhase phase)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(clock.GetTotal(phase)).count();
}

SeekBacklogCounts MakeCounts(std::int32_t dirty, std::int32_t bolts, std::int32_t beams, std::int32_t trails,
                             std::int32_t radar)
{
    SeekBacklogCounts counts;
    counts.mDirtyAreas = dirty;
    counts.mEBolts = bolts;
    counts.mRadBeams = beams;
    counts.mLineTrails = trails;
    counts.mRadarEvents = radar;
    return counts;
}

bool IsOnlyDirtyAreas(const SeekBacklogCleanup& cleanup)
{
    return cleanup.mClearDirtyAreas && !cleanup.mDeleteEBolts && !cleanup.mDeleteRadBeams &&
           !cleanup.mDeleteDetachedLineTrails && !cleanup.mHideAttachedLineTrails && !cleanup.mClearRadarEvents &&
           !cleanup.mClearScreenShake;
}

// 帧完成点：脏矩形每帧清零；三种特效只在超过阈值时清理，等于阈值不清理。
void CheckFrameThresholds()
{
    Require(IsOnlyDirtyAreas(PlanSeekBacklogCleanup(SeekCleanupPoint::FrameCompleted, MakeCounts(0, 0, 0, 0, 0))),
            "an empty frame only clears the dirty areas");
    Require(IsOnlyDirtyAreas(PlanSeekBacklogCleanup(SeekCleanupPoint::FrameCompleted,
                MakeCounts(5'000, SEEK_EBOLT_LIMIT, SEEK_RADBEAM_LIMIT, SEEK_LINE_TRAIL_LIMIT, 200))),
            "counts at the limits leave the effects alone");
    auto cleanup = PlanSeekBacklogCleanup(SeekCleanupPoint::FrameCompleted, MakeCounts(0, SEEK_EBOLT_LIMIT + 1, 0, 0, 0));
    Require(cleanup.mClearDirtyAreas && cleanup.mDeleteEBolts && !cleanup.mDeleteRadBeams &&
                !cleanup.mDeleteDetachedLineTrails,
            "bolts above the limit are deleted");
    cleanup = PlanSeekBacklogCleanup(SeekCleanupPoint::FrameCompleted, MakeCounts(0, 0, SEEK_RADBEAM_LIMIT + 1, 0, 0));
    Require(!cleanup.mDeleteEBolts && cleanup.mDeleteRadBeams && !cleanup.mDeleteDetachedLineTrails,
            "beams above the limit are deleted");
    cleanup = PlanSeekBacklogCleanup(SeekCleanupPoint::FrameCompleted, MakeCounts(0, 0, 0, SEEK_LINE_TRAIL_LIMIT + 1, 0));
    Require(!cleanup.mDeleteEBolts && !cleanup.mDeleteRadBeams && cleanup.mDeleteDetachedLineTrails &&
                !cleanup.mHideAttachedLineTrails,
            "trails above the limit lose only the detached ones");
    cleanup = PlanSeekBacklogCleanup(SeekCleanupPoint::FrameCompleted, MakeCounts(0, 100, 100, 100, 100));
    Require(cleanup.mDeleteEBolts && cleanup.mDeleteRadBeams && cleanup.mDeleteDetachedLineTrails &&
                !cleanup.mClearRadarEvents && !cleanup.mClearScreenShake && !cleanup.mHideAttachedLineTrails,
            "radar events and screen shake are never touched per frame");
}

// 兜底重绘前全部清理（尾迹只删已解绑的）；结束时另隐去挂着的尾迹、清雷达事件与屏震。与条数无关。
void CheckRedrawPoints()
{
    for (const auto& counts : {MakeCounts(0, 0, 0, 0, 0), MakeCounts(9, 1, 1, 1, 1)})
    {
        const auto fallback = PlanSeekBacklogCleanup(SeekCleanupPoint::BeforeFallbackDraw, counts);
        Require(fallback.mClearDirtyAreas && fallback.mDeleteEBolts && fallback.mDeleteRadBeams &&
                    fallback.mDeleteDetachedLineTrails,
                "a fallback draw clears everything first");
        Require(!fallback.mHideAttachedLineTrails && !fallback.mClearRadarEvents && !fallback.mClearScreenShake,
                "a fallback draw keeps attached trails, radar events and screen shake");
        const auto end = PlanSeekBacklogCleanup(SeekCleanupPoint::SeekEnd, counts);
        Require(end.mClearDirtyAreas && end.mDeleteEBolts && end.mDeleteRadBeams && end.mDeleteDetachedLineTrails &&
                    end.mHideAttachedLineTrails && end.mClearRadarEvents && end.mClearScreenShake,
                "the end of a seek clears everything");
    }
}

// 一帧按回调点切换阶段；嵌套的插件与清理阶段计入自己，不计入外层。
void CheckPhaseClock()
{
    SeekPhaseClock clock;
    Require(!clock.IsRunning(), "a fresh clock is idle");
    // 空闲时 Push 与 Pop 不起作用。
    const auto idleToken = clock.Push(SeekPhase::Plugin, At(0));
    Require(!idleToken && !clock.IsRunning(), "pushing on an idle clock does nothing");
    clock.Pop(idleToken, At(10));
    Require(!clock.IsRunning() && Microseconds(clock, SeekPhase::Plugin) == 0, "popping an idle token does nothing");

    // 第 1 帧：输入 100、门控到帧解析 400（其中兜底清理 50）、事件 300、帧尾 50、帧完成后到下一帧开始 200（其中插件 80、清理 20）。
    clock.Enter(SeekPhase::Input, At(1'000));
    Require(clock.IsRunning(), "the first boundary starts the clock");
    clock.Enter(SeekPhase::Simulation, At(1'100));
    auto token = clock.Push(SeekPhase::Cleanup, At(1'200));
    clock.Pop(token, At(1'250));
    clock.Enter(SeekPhase::Events, At(1'500));
    clock.Enter(SeekPhase::Tail, At(1'800));
    clock.Enter(SeekPhase::Other, At(1'850));
    token = clock.Push(SeekPhase::Cleanup, At(1'860));
    clock.Pop(token, At(1'880));
    token = clock.Push(SeekPhase::Plugin, At(1'900));
    clock.Pop(token, At(1'980));
    // 第 2 帧开始，输入 70 后结束。
    clock.Enter(SeekPhase::Input, At(2'050));
    clock.Stop(At(2'120));
    Require(!clock.IsRunning(), "stop makes the clock idle");

    Require(Microseconds(clock, SeekPhase::Input) == 170, "input: 100 + 70");
    Require(Microseconds(clock, SeekPhase::Simulation) == 350, "simulation excludes the nested cleanup");
    Require(Microseconds(clock, SeekPhase::Events) == 300, "events");
    Require(Microseconds(clock, SeekPhase::Tail) == 50, "tail");
    Require(Microseconds(clock, SeekPhase::Cleanup) == 70, "cleanup: 50 + 20");
    Require(Microseconds(clock, SeekPhase::Plugin) == 80, "plugin");
    Require(Microseconds(clock, SeekPhase::Other) == 100, "other excludes the nested plugin and cleanup");
    std::int64_t total = 0;
    for (std::size_t phase = 0; phase < static_cast<std::size_t>(SeekPhase::Count); ++phase)
        total += Microseconds(clock, static_cast<SeekPhase>(phase));
    Require(total == 1'120, "the phases do not overlap and add up to the wall time");

    // 嵌套阶段中途停止（快进在其中结束）：之后的 Pop 不重新开始计时。
    SeekPhaseClock nested;
    nested.Enter(SeekPhase::Other, At(0));
    token = nested.Push(SeekPhase::Plugin, At(10));
    nested.Stop(At(30));
    nested.Pop(token, At(40));
    Require(!nested.IsRunning() && Microseconds(nested, SeekPhase::Plugin) == 20 &&
                Microseconds(nested, SeekPhase::Other) == 10,
            "a pop after stop leaves the clock idle");
    // 时钟回退的一段按 0 计。
    nested.Enter(SeekPhase::Input, At(100));
    nested.Enter(SeekPhase::Simulation, At(90));
    nested.Stop(At(95));
    Require(Microseconds(nested, SeekPhase::Input) == 0 && Microseconds(nested, SeekPhase::Simulation) == 5,
            "a backward step counts as zero");
    nested.Reset();
    Require(!nested.IsRunning() && Microseconds(nested, SeekPhase::Plugin) == 0, "reset clears the totals");
}

// 峰值取各次观察的最大值；清理次数只计实际删掉了记录的那几次；日志带齐各字段。
void CheckProfileAndLog()
{
    SeekProfile profile;
    profile.ObserveBacklog(MakeCounts(120, 3, 40, 70, 2));
    profile.ObserveBacklog(MakeCounts(80, 35, 2, 90, 5));
    profile.ObserveBacklog(MakeCounts(0, 0, 0, 0, 0));
    const auto& peaks = profile.GetPeaks();
    Require(peaks.mDirtyAreas == 120 && peaks.mEBolts == 35 && peaks.mRadBeams == 40 && peaks.mLineTrails == 90 &&
                peaks.mRadarEvents == 5,
            "peaks are the per-field maxima");

    SeekBacklogCleanupResult result;
    result.mEBoltsDeleted = 35;
    profile.RecordCleanup(result);
    result = SeekBacklogCleanupResult();
    result.mRadBeamsDeleted = 40;
    result.mLineTrailsDeleted = 12;
    profile.RecordCleanup(result);
    profile.RecordCleanup(SeekBacklogCleanupResult());
    profile.RecordFallbackDraw();
    profile.RecordFallbackDraw();
    Require(profile.GetFallbackDraws() == 2, "fallback draws are counted");

    auto& clock = profile.GetClock();
    clock.Enter(SeekPhase::Input, At(0));
    clock.Enter(SeekPhase::Simulation, At(1'260));
    clock.Enter(SeekPhase::Events, At(4'000));
    clock.Stop(At(4'500));
    const std::string line = profile.FormatTimingLog(600);
    const std::string expected =
        "Replay seek timing: frames=600 inputMs=1.3 simMs=2.7 eventsMs=0.5 tailMs=0.0 pluginMs=0.0 cleanupMs=0.0 "
        "otherMs=0.0 draws=2 dirtyPeak=120 eboltPeak=35 eboltClears=1 radBeamPeak=40 radBeamClears=1 "
        "lineTrailPeak=90 lineTrailClears=1 radarEventPeak=5";
    Require(line == expected, "timing log: " + line);

    profile.Reset();
    Require(profile.GetFallbackDraws() == 0 && profile.GetPeaks().mDirtyAreas == 0 && !profile.GetClock().IsRunning(),
            "reset starts a new seek from zero");
    Require(profile.FormatTimingLog(0).find("eboltClears=0") != std::string::npos, "reset clears the clear counts");
}
}

int main()
{
    try
    {
        CheckFrameThresholds();
        CheckRedrawPoints();
        CheckPhaseClock();
        CheckProfileAndLog();
        std::cout << "Playback seek profile tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
