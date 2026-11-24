#include "../src/stats/GameClock.h"

#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::stats;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

FrameTimingInput Offline(int speed)
{
    FrameTimingInput input;
    input.mGameSpeed = speed;
    return input;
}

FrameTimingInput Network(int desiredFrameRate)
{
    FrameTimingInput input;
    input.mIsNetwork = true;
    input.mDesiredFrameRate = desiredFrameRate;
    input.mGameSpeed = 6;
    return input;
}

// 单机每帧 = 速度档 × 16 ms；速度 0 与非法速度按名义 16 ms 并标记名义。
void CheckOfflineDurations()
{
    for (int speed = 1; speed <= 6; ++speed)
    {
        const FrameDuration duration = GetFrameDuration(Offline(speed));
        Require(duration.mMilliseconds == speed * 16 && !duration.mIsNominal,
                "offline speed " + std::to_string(speed) + " is speed x 16 ms");
    }
    for (const int speed : {0, -1, 7, 100})
    {
        const FrameDuration duration = GetFrameDuration(Offline(speed));
        Require(duration.mMilliseconds == NOMINAL_FRAME_MS && duration.mIsNominal,
                "speed " + std::to_string(speed) + " is nominal 16 ms");
    }
    // 联机速度不影响单机规则的读取：只看 mIsNetwork。
    Require(GetFrameDuration(Network(30)).mMilliseconds == 33, "network ignores GameSpeed");
}

// 联机每帧 = 1000 / DesiredFrameRate 取整，与原生一致；非正帧率为 33 ms。
void CheckNetworkDurations()
{
    const struct { int mRate; int mMs; } expected[] = {{60, 16}, {45, 22}, {30, 33}, {20, 50}, {15, 66}, {12, 83}, {10, 100}};
    for (const auto& item : expected)
    {
        const FrameDuration duration = GetFrameDuration(Network(item.mRate));
        Require(duration.mMilliseconds == item.mMs && !duration.mIsNominal,
                "network rate " + std::to_string(item.mRate));
    }
    Require(GetFrameDuration(Network(0)).mMilliseconds == 33, "zero rate falls back to 33 ms");
    Require(GetFrameDuration(Network(-5)).mMilliseconds == 33, "negative rate falls back to 33 ms");
    Require(GetFrameDuration(Network(5000)).mMilliseconds == 1, "huge rate never yields zero duration");
}

void CheckAccumulation()
{
    GameClock clock;
    Require(!clock.HasStarted() && clock.GetMilliseconds() == 0 && !clock.IsNominal(), "fresh clock");
    for (std::uint32_t frame = 0; frame < 10; ++frame)
        Require(clock.OnFrameCompleted(frame, Offline(2)), "frame accumulates");
    Require(clock.GetMilliseconds() == 320 && clock.HasStarted(), "ten frames at speed 2 are 320 ms");

    // 暂停时帧号不推进：反复登记同一帧不累加。
    for (int repeat = 0; repeat < 5; ++repeat)
        Require(!clock.OnFrameCompleted(9, Offline(2)), "repeated frame is ignored");
    Require(clock.GetMilliseconds() == 320, "pause does not accumulate");

    // 中途变速：之后的帧按新速度累加，已累加的不变。
    for (std::uint32_t frame = 10; frame < 15; ++frame)
        clock.OnFrameCompleted(frame, Offline(4));
    Require(clock.GetMilliseconds() == 320 + 5 * 64, "speed change applies to later frames only");
    Require(!clock.OnFrameCompleted(3, Offline(2)) && clock.GetMilliseconds() == 640, "backward frame is ignored");

    clock.Reset();
    Require(!clock.HasStarted() && clock.GetMilliseconds() == 0, "reset clears the clock");
}

void CheckFirstFrameAndGaps()
{
    GameClock clock;
    Require(clock.OnFrameCompleted(40, Offline(1)) && clock.GetMilliseconds() == 16,
            "first registration counts one frame whatever its number");
    clock.OnFrameCompleted(43, Offline(1));
    Require(clock.GetMilliseconds() == 16 + 3 * 16, "a skipped-frame gap is filled with frame durations");
    clock.OnFrameCompleted(43 + 100000, Offline(1));
    Require(clock.GetMilliseconds() == 16 + 3 * 16 + 600 * 16, "an absurd jump is capped");
}

// 联机：DesiredFrameRate 由 Timing 事件在回放中途改变，之后的帧按新帧率累加。
void CheckNetworkRateChange()
{
    GameClock clock;
    for (std::uint32_t frame = 0; frame < 30; ++frame)
        clock.OnFrameCompleted(frame, Network(60));
    Require(clock.GetMilliseconds() == 30 * 16, "network frames at 60 fps");
    for (std::uint32_t frame = 30; frame < 40; ++frame)
        clock.OnFrameCompleted(frame, Network(20));
    Require(clock.GetMilliseconds() == 30 * 16 + 10 * 50 && !clock.IsNominal(), "network rate change");
}

// 名义标记一旦出现就保持：速度 0 的帧之后换回有限速度，时间仍只是名义值。
void CheckNominalIsSticky()
{
    GameClock clock;
    clock.OnFrameCompleted(0, Offline(3));
    Require(!clock.IsNominal(), "real speed is not nominal");
    clock.OnFrameCompleted(1, Offline(0));
    Require(clock.IsNominal() && clock.GetMilliseconds() == 48 + 16, "speed 0 frame is counted as 16 ms and nominal");
    clock.OnFrameCompleted(2, Offline(3));
    Require(clock.IsNominal() && clock.GetMilliseconds() == 48 + 16 + 48, "nominal marker stays");
    clock.Reset();
    Require(!clock.IsNominal(), "reset clears nominal marker");
}
}

int main()
{
    try
    {
        CheckOfflineDurations();
        CheckNetworkDurations();
        CheckAccumulation();
        CheckFirstFrameAndGaps();
        CheckNetworkRateChange();
        CheckNominalIsSticky();
        std::cout << "Game clock tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
