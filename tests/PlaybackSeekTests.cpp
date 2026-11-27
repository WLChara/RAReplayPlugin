#include "../src/plugin/PlaybackSeek.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::plugin::PlaybackSeek;
using rareplay::plugin::SEEK_FALLBACK_DRAW_INTERVAL_MS;
using rareplay::plugin::SEEK_HUD_STALE_MS;
using rareplay::plugin::SEEK_INPUT_TICK_MS;
using rareplay::plugin::SeekEndReason;
using rareplay::plugin::SeekInputThrottle;

// 没有 HUD 心跳（F10 隐藏、界面未呈现或换会话）。
constexpr std::optional<std::int64_t> NO_HUD = std::nullopt;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < 1e-5f;
}

void CheckBegin()
{
    PlaybackSeek seek;
    Require(!seek.IsActive() && !seek.GetView().mIsSeeking, "a fresh seek is idle");
    Require(!seek.Begin(100, 100, false, 0, NO_HUD), "a target equal to the current frame is rejected");
    Require(!seek.Begin(100, 40, false, 0, NO_HUD), "a backward target is rejected");
    Require(!seek.IsActive(), "rejected targets leave the seek idle");

    Require(seek.Begin(100, 500, true, 1'000, 990), "a forward target starts the seek");
    Require(seek.IsActive() && seek.GetTargetFrame() == 500, "the target is recorded");
    const auto view = seek.GetView();
    Require(view.mIsSeeking && view.mStartFrame == 100 && view.mTargetFrame == 500, "the view carries start and target");
    Require(Near(seek.GetProgress(), 0.0f), "progress starts at 0");
    Require(!seek.Begin(200, 900, false, 2'000, NO_HUD), "a second begin while seeking is rejected");
    Require(seek.GetView().mStartFrame == 100 && seek.GetTargetFrame() == 500,
            "a rejected second begin changes nothing");
}

void CheckRetarget()
{
    PlaybackSeek idle;
    Require(!idle.Retarget(900), "retargeting needs an active seek");

    PlaybackSeek seek;
    seek.Begin(100, 500, false, 0, NO_HUD);
    seek.OnFrameCompleted(300);
    Require(!seek.Retarget(500), "the same target is not a change");
    Require(!seek.Retarget(400), "an earlier target is rejected");
    Require(seek.GetTargetFrame() == 500, "the target stays");
    Require(seek.Retarget(900), "a later target is accepted");
    Require(seek.GetTargetFrame() == 900 && seek.GetView().mStartFrame == 100, "retarget keeps the start frame");
    Require(Near(seek.GetProgress(), 200.0f / 800.0f), "progress is measured against the new target");
}

void CheckReachAndProgress()
{
    PlaybackSeek seek;
    seek.Begin(1'000, 1'004, false, 0, NO_HUD);
    Require(!seek.OnFrameCompleted(1'001), "frame 1001 is before the target");
    Require(Near(seek.GetProgress(), 0.25f), "one of four frames done");
    Require(!seek.OnFrameCompleted(1'003), "frame 1003 is the frame before the target");
    Require(Near(seek.GetProgress(), 0.75f), "three of four frames done");
    // 暂停或菜单帧反复进入同一完成点，帧号不推进；回退的帧号不改变进度。
    Require(!seek.OnFrameCompleted(1'003) && !seek.OnFrameCompleted(998), "repeated or stale frames do not reach");
    Require(Near(seek.GetProgress(), 0.75f), "stale frames do not move progress back");
    Require(seek.OnFrameCompleted(1'004), "completing the target frame reaches it");
    Require(Near(seek.GetProgress(), 1.0f), "progress is 1 at the target");
    Require(seek.IsActive(), "reaching does not end by itself");

    // 门控被跳过时可能多跑一帧：越过目标同样算到达。
    PlaybackSeek overshoot;
    overshoot.Begin(10, 20, false, 0, NO_HUD);
    Require(overshoot.OnFrameCompleted(21) && Near(overshoot.GetProgress(), 1.0f), "overshooting counts as reached");

    PlaybackSeek idle;
    Require(!idle.OnFrameCompleted(5) && Near(idle.GetProgress(), 0.0f), "an idle seek never reaches");
}

// 心跳新鲜时一律不画，不论过了多久。
void CheckNoDrawWhileHudIsFresh()
{
    PlaybackSeek idle;
    Require(!idle.ShouldDrawFallback(1'000'000, NO_HUD), "an idle seek leaves drawing to the caller");

    PlaybackSeek seek;
    seek.Begin(0, 10'000, false, 5'000, 4'990);
    for (std::int64_t nowMs = 5'000; nowMs <= 65'000; nowMs += 7)
    {
        // HUD 每 16 ms 左右呈现一次，心跳最多落后一个呈现间隔。
        const std::int64_t presentedAt = nowMs - (nowMs % 16);
        Require(!seek.ShouldDrawFallback(nowMs, presentedAt), "a fresh heartbeat never draws at " + std::to_string(nowMs));
    }
    // 恰好 SEEK_HUD_STALE_MS 仍算新鲜，超过才算超时。
    Require(!seek.ShouldDrawFallback(70'000, 70'000 - SEEK_HUD_STALE_MS), "exactly the stale limit is still fresh");
    Require(seek.ShouldDrawFallback(70'000, 70'000 - SEEK_HUD_STALE_MS - 1), "beyond the stale limit draws");
    // 另一线程刚写入、晚于 now 的心跳算新鲜。
    Require(!seek.ShouldDrawFallback(70'000, 70'005), "a heartbeat later than now is fresh");
}

// 开始时心跳新鲜：第一次兜底最早在开始后满 1000 ms，即使心跳更早超时。
void CheckGraceAtBegin()
{
    PlaybackSeek seek;
    // 开始时心跳已有 900 ms，仍算新鲜；100 ms 后它就超时了。
    seek.Begin(0, 10'000, false, 10'000, 9'100);
    Require(!seek.ShouldDrawFallback(10'101, 9'100), "a heartbeat going stale soon after begin does not draw yet");
    Require(!seek.ShouldDrawFallback(10'000 + SEEK_FALLBACK_DRAW_INTERVAL_MS - 1, 9'100),
            "no fallback within the first interval after begin");
    Require(seek.ShouldDrawFallback(10'000 + SEEK_FALLBACK_DRAW_INTERVAL_MS, 9'100),
            "the first fallback comes one interval after begin");

    // 开始时没有心跳，或心跳已超时：第一次兜底不必等待。
    PlaybackSeek hidden;
    hidden.Begin(0, 10'000, false, 10'000, NO_HUD);
    Require(hidden.ShouldDrawFallback(10'000, NO_HUD), "no heartbeat at begin draws at once");
    PlaybackSeek stale;
    stale.Begin(0, 10'000, false, 10'000, 10'000 - SEEK_HUD_STALE_MS - 1);
    Require(stale.ShouldDrawFallback(10'000, 10'000 - SEEK_HUD_STALE_MS - 1), "a stale heartbeat at begin draws at once");
    Require(!stale.ShouldDrawFallback(10'000, 9'999), "a heartbeat that became fresh again does not draw");
}

// 心跳超时（或没有）之后每 1000 ms 画一次；长时间不画也只补一次；时钟回退时不画。
void CheckFallbackCadence()
{
    PlaybackSeek seek;
    seek.Begin(0, 100'000, false, 0, NO_HUD);
    Require(seek.ShouldDrawFallback(3, NO_HUD), "first fallback");
    seek.MarkFallbackDrawn(3);
    Require(!seek.ShouldDrawFallback(3 + SEEK_FALLBACK_DRAW_INTERVAL_MS - 1, NO_HUD), "not before the interval");
    Require(seek.ShouldDrawFallback(3 + SEEK_FALLBACK_DRAW_INTERVAL_MS, NO_HUD), "one interval later");
    Require(seek.ShouldDrawFallback(3 + SEEK_FALLBACK_DRAW_INTERVAL_MS + 37, NO_HUD), "still due until marked");
    seek.MarkFallbackDrawn(1'040);
    // 兜底重绘让 HUD 呈现了一次：心跳新鲜期间不再画，再次超时后按间隔画。
    Require(!seek.ShouldDrawFallback(2'040, 1'045), "a fresh heartbeat after a fallback suppresses the next one");
    Require(seek.ShouldDrawFallback(2'046, 1'045), "the next fallback once the heartbeat is stale and the interval passed");
    Require(!seek.ShouldDrawFallback(2'039, NO_HUD), "the interval still applies without a heartbeat");
    // 长时间没有机会绘制（例如切出窗口），回来后只补一次。
    Require(seek.ShouldDrawFallback(60'000, NO_HUD), "a long gap makes one draw due");
    seek.MarkFallbackDrawn(60'000);
    Require(!seek.ShouldDrawFallback(60'500, NO_HUD), "and only one");
    // 时钟回退时不画，直到重新满间隔。
    Require(!seek.ShouldDrawFallback(59'000, NO_HUD), "a clock going backward never makes a draw due");
    seek.Retarget(200'000);
    Require(!seek.ShouldDrawFallback(60'500, NO_HUD), "retarget does not reset the cadence");

    seek.End(SeekEndReason::Cancelled, 61'000);
    Require(!seek.ShouldDrawFallback(70'000, NO_HUD), "an ended seek draws nothing");
    seek.MarkFallbackDrawn(70'000);
    Require(!seek.IsActive(), "marking a draw on an idle seek is ignored");

    // 新的一次快进重新计算宽限，不沿用上次的兜底时刻。
    Require(seek.Begin(10, 20, false, 80'000, 79'990), "a new seek");
    Require(!seek.ShouldDrawFallback(80'500, 79'000), "the new seek has its own grace");
}

void CheckEndAndRestore()
{
    struct Case
    {
        SeekEndReason mReason;
        bool mWasPaused;
        bool mRestores;
        const char* mName;
    };
    const Case cases[] = {
        {SeekEndReason::Reached, false, true, "reached while playing"},
        {SeekEndReason::Reached, true, true, "reached while paused"},
        {SeekEndReason::Cancelled, false, true, "cancelled while playing"},
        {SeekEndReason::Cancelled, true, true, "cancelled while paused"},
        {SeekEndReason::PlaybackEnded, true, false, "playback ended"},
        {SeekEndReason::Failed, false, false, "playback failed"},
        {SeekEndReason::Exited, true, false, "exit or restart"}};
    for (const Case& item : cases)
    {
        PlaybackSeek seek;
        seek.Begin(50, 400, item.mWasPaused, 10'000, NO_HUD);
        seek.OnFrameCompleted(260);
        const auto outcome = seek.End(item.mReason, 12'345);
        const std::string name = item.mName;
        Require(outcome.has_value() && outcome->mReason == item.mReason, name + ": an active seek ends with its reason");
        Require(outcome->mStartFrame == 50 && outcome->mTargetFrame == 400 && outcome->mLastCompletedFrame == 260,
                name + ": the outcome carries the frames");
        Require(outcome->mElapsedMs == 2'345, name + ": the outcome carries the wall time");
        if (item.mRestores)
            Require(outcome->mPausedToRestore == item.mWasPaused, name + ": the pre-seek pause state is restored");
        else
            Require(!outcome->mPausedToRestore, name + ": nothing to restore");
        Require(!seek.IsActive() && !seek.GetView().mIsSeeking, name + ": the seek is idle afterwards");
        Require(!seek.End(item.mReason, 20'000), name + ": ending twice yields nothing");
        Require(seek.Begin(260, 300, false, 20'000, NO_HUD), name + ": a new seek can start afterwards");
    }

    PlaybackSeek clockBack;
    clockBack.Begin(0, 10, false, 5'000, NO_HUD);
    Require(clockBack.End(SeekEndReason::Reached, 4'000)->mElapsedMs == 0, "a clock going backward gives 0 ms");

    PlaybackSeek idle;
    Require(!idle.End(SeekEndReason::Cancelled, 0), "ending an idle seek yields nothing");
}

// 输入降频：快进中每 16 ms 完整执行一次，其余跳过；不在快进中时总是完整执行，且忘掉上次的时刻。
void CheckInputThrottle()
{
    SeekInputThrottle throttle;
    for (int call = 0; call < 5; ++call)
        Require(!throttle.ShouldSkip(false, 0), "outside a seek every call runs");

    Require(!throttle.ShouldSkip(true, 1'000), "the first call of a seek runs");
    Require(throttle.ShouldSkip(true, 1'000), "a second call at the same time is skipped");
    Require(throttle.ShouldSkip(true, 1'000 + SEEK_INPUT_TICK_MS - 1), "calls within the tick are skipped");
    Require(!throttle.ShouldSkip(true, 1'000 + SEEK_INPUT_TICK_MS), "a call one tick later runs");
    Require(throttle.ShouldSkip(true, 1'000 + SEEK_INPUT_TICK_MS + 3), "the tick restarts at the full run");
    // 长时间间隔后只完整执行一次，然后重新计时。
    Require(!throttle.ShouldSkip(true, 5'000), "a long gap runs once");
    Require(throttle.ShouldSkip(true, 5'001), "and then waits again");
    // 时钟回退：完整执行并以新时刻计时。
    Require(!throttle.ShouldSkip(true, 4'000), "a clock going backward runs");
    Require(throttle.ShouldSkip(true, 4'010), "and restarts the tick there");

    // 快进结束后照常执行；下一次快进的第一次调用总是完整执行，不受上次时刻影响。
    Require(!throttle.ShouldSkip(false, 0), "after the seek every call runs");
    Require(!throttle.ShouldSkip(true, 4'011), "the first call of the next seek runs");

    // 不限速的快进约 2 ms 一帧：1000 ms 内完整执行约 1000 / 16 次。
    SeekInputThrottle paced;
    int fullRuns = 0;
    for (std::int64_t nowMs = 0; nowMs < 1'000; nowMs += 2)
    {
        if (!paced.ShouldSkip(true, nowMs))
            ++fullRuns;
    }
    Require(fullRuns == 63, "one full run per 16 ms over 1000 ms: " + std::to_string(fullRuns));
}

// 运行时的典型流程：开始、逐帧推进、HUD 心跳先新鲜后中断（F10）、改目标、到达后恢复暂停。
void CheckTypicalFlow()
{
    PlaybackSeek seek;
    std::int64_t nowMs = 0;
    Require(seek.Begin(1'000, 1'300, true, nowMs, nowMs), "begin");
    int draws = 0;
    std::uint32_t frame = 1'000;
    bool isReached = false;
    while (!isReached)
    {
        ++frame;
        nowMs += 2; // 不限速时约 2 ms 一帧。
        // 前 500 ms HUD 照常呈现；之后 HUD 被隐藏，心跳撤回。
        const std::optional<std::int64_t> heartbeat = nowMs < 500 ? std::optional<std::int64_t>(nowMs) : NO_HUD;
        if (seek.ShouldDrawFallback(nowMs, heartbeat))
        {
            ++draws;
            seek.MarkFallbackDrawn(nowMs);
        }
        if (frame == 1'100)
            Require(seek.Retarget(1'800), "retarget midway");
        isReached = seek.OnFrameCompleted(frame);
    }
    Require(frame == 1'800, "the seek stops at the new target");
    // 开始时心跳新鲜：1000 ms 时第一次兜底，此后每 1000 ms 一次（1600 ms 内只有 1000 ms 这一次）。
    Require(draws == 1, "fallback draws only after the HUD disappears and the grace passes: " + std::to_string(draws));
    const auto outcome = seek.End(SeekEndReason::Reached, nowMs);
    Require(outcome && outcome->mPausedToRestore == true && outcome->mElapsedMs == 1'600,
            "the seek ends paused like before the jump");
}
}

int main()
{
    try
    {
        CheckBegin();
        CheckRetarget();
        CheckReachAndProgress();
        CheckNoDrawWhileHudIsFresh();
        CheckGraceAtBegin();
        CheckFallbackCadence();
        CheckEndAndRestore();
        CheckInputThrottle();
        CheckTypicalFlow();
        std::cout << "Playback seek tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
