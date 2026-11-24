#include "../src/plugin/PlaybackSeek.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::plugin::PlaybackSeek;
using rareplay::plugin::SEEK_DRAW_INTERVAL_MS;
using rareplay::plugin::SeekEndReason;

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
    Require(!seek.Begin(100, 100, false, 0), "a target equal to the current frame is rejected");
    Require(!seek.Begin(100, 40, false, 0), "a backward target is rejected");
    Require(!seek.IsActive(), "rejected targets leave the seek idle");

    Require(seek.Begin(100, 500, true, 1'000), "a forward target starts the seek");
    Require(seek.IsActive() && seek.GetTargetFrame() == 500, "the target is recorded");
    const auto view = seek.GetView();
    Require(view.mIsSeeking && view.mStartFrame == 100 && view.mTargetFrame == 500, "the view carries start and target");
    Require(Near(seek.GetProgress(), 0.0f), "progress starts at 0");
    Require(!seek.Begin(200, 900, false, 2'000), "a second begin while seeking is rejected");
    Require(seek.GetView().mStartFrame == 100 && seek.GetTargetFrame() == 500,
            "a rejected second begin changes nothing");
}

void CheckRetarget()
{
    PlaybackSeek idle;
    Require(!idle.Retarget(900), "retargeting needs an active seek");

    PlaybackSeek seek;
    seek.Begin(100, 500, false, 0);
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
    seek.Begin(1'000, 1'004, false, 0);
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
    overshoot.Begin(10, 20, false, 0);
    Require(overshoot.OnFrameCompleted(21) && Near(overshoot.GetProgress(), 1.0f), "overshooting counts as reached");

    PlaybackSeek idle;
    Require(!idle.OnFrameCompleted(5) && Near(idle.GetProgress(), 0.0f), "an idle seek never reaches");
}

void CheckDrawCadence()
{
    PlaybackSeek idle;
    Require(!idle.ShouldDraw(1'000'000), "an idle seek leaves drawing to the caller");

    PlaybackSeek seek;
    seek.Begin(0, 10'000, false, 5'000);
    // 开始时刻算作“刚画过”。
    Require(!seek.ShouldDraw(5'000), "no draw right at the start");
    Require(!seek.ShouldDraw(5'000 + SEEK_DRAW_INTERVAL_MS - 1), "no draw before 100 ms");
    Require(seek.ShouldDraw(5'000 + SEEK_DRAW_INTERVAL_MS), "draw once 100 ms have passed");
    Require(seek.ShouldDraw(5'000 + SEEK_DRAW_INTERVAL_MS + 37), "still due until marked");
    seek.MarkDrawn(5'137);
    Require(!seek.ShouldDraw(5'137 + 99), "the interval restarts at the draw");
    Require(seek.ShouldDraw(5'137 + 100), "the next draw is 100 ms later");
    // 长时间没有机会绘制（例如切出窗口），回来后只补一次。
    seek.MarkDrawn(5'237);
    Require(seek.ShouldDraw(60'000), "a long gap makes one draw due");
    seek.MarkDrawn(60'000);
    Require(!seek.ShouldDraw(60'050), "and only one");
    // 时钟回退时不画，直到重新满间隔。
    Require(!seek.ShouldDraw(59'000), "a clock going backward never makes a draw due");
    seek.Retarget(20'000);
    Require(!seek.ShouldDraw(60'050), "retarget does not reset the cadence");

    seek.End(SeekEndReason::Cancelled, 61'000);
    Require(!seek.ShouldDraw(70'000), "an ended seek draws nothing on the cadence");
    seek.MarkDrawn(70'000);
    Require(!seek.IsActive(), "marking a draw on an idle seek is ignored");
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
        seek.Begin(50, 400, item.mWasPaused, 10'000);
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
        Require(seek.Begin(260, 300, false, 20'000), name + ": a new seek can start afterwards");
    }

    PlaybackSeek clockBack;
    clockBack.Begin(0, 10, false, 5'000);
    Require(clockBack.End(SeekEndReason::Reached, 4'000)->mElapsedMs == 0, "a clock going backward gives 0 ms");

    PlaybackSeek idle;
    Require(!idle.End(SeekEndReason::Cancelled, 0), "ending an idle seek yields nothing");
}

// 运行时的典型流程：开始、逐帧推进、按节拍绘制、改目标、到达后恢复暂停。
void CheckTypicalFlow()
{
    PlaybackSeek seek;
    std::int64_t nowMs = 0;
    Require(seek.Begin(1'000, 1'300, true, nowMs), "begin");
    int draws = 0;
    std::uint32_t frame = 1'000;
    bool isReached = false;
    while (!isReached)
    {
        ++frame;
        nowMs += 2; // 不限速时约 2 ms 一帧。
        if (seek.ShouldDraw(nowMs))
        {
            ++draws;
            seek.MarkDrawn(nowMs);
        }
        if (frame == 1'100)
            Require(seek.Retarget(1'500), "retarget midway");
        isReached = seek.OnFrameCompleted(frame);
    }
    Require(frame == 1'500, "the seek stops at the new target");
    Require(draws == 10, "one draw per 100 ms over 1000 ms: " + std::to_string(draws));
    const auto outcome = seek.End(SeekEndReason::Reached, nowMs);
    Require(outcome && outcome->mPausedToRestore == true && outcome->mElapsedMs == 1'000,
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
        CheckDrawCadence();
        CheckEndAndRestore();
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
