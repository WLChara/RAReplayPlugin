#include "../src/stats/GameClock.h"
#include "../src/stats/GameTimelineBuilder.h"
#include "OperationEventBuilder.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace rareplay::stats;
using rareplay::tests::EventBuilder;
using rareplay::tests::NativeEvent;
using rareplay::viewer::GameTimeline;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

NativeEvent MakeTiming(std::uint16_t requestedFps, int house = 0)
{
    using namespace event_layout;
    return EventBuilder(NativeEventType::Timing, house, 0xCC)
        .U16(TIMING_REQUESTED_FPS, requestedFps)
        .U16(TIMING_MAX_AHEAD, 16)
        .U8(TIMING_FRAME_SEND_RATE, 4)
        .Build();
}

// 一帧的输入：帧记录携带的速度与本帧执行的事件。
struct FrameInput
{
    std::optional<std::uint32_t> mGameSpeed;
    std::vector<NativeEvent> mEvents;
};

GameTimeline Build(TimelineTiming timing, const std::vector<FrameInput>& frames,
                   std::optional<int> runtimeSpeed = std::nullopt)
{
    GameTimelineBuilder builder(timing);
    for (std::uint32_t frame = 0; frame < frames.size(); ++frame)
        Require(builder.AddFrame(frame, frames[frame].mGameSpeed, frames[frame].mEvents),
                "frame " + std::to_string(frame) + " is accepted: " + builder.GetError());
    Require(builder.GetFrameCount() == frames.size(), "every frame is counted");
    auto timeline = builder.TakeTimeline(runtimeSpeed);
    Require(timeline.has_value(), "the timeline is produced");
    Require(timeline->GetFrameCount() == frames.size(), "one entry per frame");
    return *timeline;
}

// 与运行时相同地驱动统计时钟：游戏线程在帧完成点读取 GameSpeed（单机，模拟前已写回本帧速度）
// 或 DesiredFrameRate（联机，本帧事件已执行），逐帧比较累计毫秒与名义标记。
// 这里不调用时间表的解码，Timing 事件改变的帧率由测试按自己写入事件的值模拟。
void RequireSameAsGameClock(const GameTimeline& timeline, bool isNetwork, const std::vector<int>& speedOrRateByFrame,
                            const std::string& name, bool expectNominalEverywhere = false)
{
    GameClock clock;
    for (std::uint32_t frame = 0; frame < speedOrRateByFrame.size(); ++frame)
    {
        FrameTimingInput input;
        input.mIsNetwork = isNetwork;
        if (isNetwork)
            input.mDesiredFrameRate = speedOrRateByFrame[frame];
        else
            input.mGameSpeed = speedOrRateByFrame[frame];
        Require(clock.OnFrameCompleted(frame, input), name + ": the clock accumulates frame " + std::to_string(frame));
        const auto time = timeline.GetTimeAt(frame);
        Require(time.has_value(), name + ": frame " + std::to_string(frame) + " is in the timeline");
        Require(time->mMilliseconds == clock.GetMilliseconds(),
                name + ": frame " + std::to_string(frame) + " timeline " + std::to_string(time->mMilliseconds) +
                    " ms equals the game clock " + std::to_string(clock.GetMilliseconds()) + " ms");
        Require(time->mIsNominal == (expectNominalEverywhere || clock.IsNominal()),
                name + ": nominal flag at frame " + std::to_string(frame));
    }
    Require(!timeline.GetTimeAt(static_cast<std::uint32_t>(speedOrRateByFrame.size())),
            name + ": no entry past the last frame");
}

void CheckTimingDecode()
{
    Require(static_cast<int>(NativeEventType::Timing) == 32, "Timing is event type 32");
    const NativeEvent timing = MakeTiming(45);
    Require(timing[EVENT_PAYLOAD_OFFSET] == 45 && timing[EVENT_PAYLOAD_OFFSET + 1] == 0,
            "RequestedFPS sits at event byte +7, little endian");
    Require(DecodeTimingDesiredFrameRate(timing) == 45, "a Timing event yields its requested FPS");
    Require(DecodeTimingDesiredFrameRate(MakeTiming(0)) == 0, "zero is passed through unchanged");
    Require(DecodeTimingDesiredFrameRate(MakeTiming(0xFFFF)) == 65535, "the u16 is not clamped or sign-extended");
    Require(DecodeTimingDesiredFrameRate(MakeTiming(0x1234)) == 0x1234, "both bytes are read");

    // 其他类型、哪怕载荷相同，也不是 Timing。
    for (const NativeEventType type : {NativeEventType::Empty, NativeEventType::GameSpeed, NativeEventType::ResponseTime,
             NativeEventType::ProcessTime, NativeEventType::LatencyFudge, NativeEventType::FrameInfo,
             NativeEventType::MegaMission})
    {
        NativeEvent event = timing;
        event[EVENT_TYPE_OFFSET] = static_cast<std::uint8_t>(type);
        Require(!DecodeTimingDesiredFrameRate(event), "event type " + std::to_string(static_cast<int>(type)) +
                                                          " is not a Timing event");
    }
    const std::vector<std::uint8_t> shortEvent(timing.begin(), timing.begin() + NATIVE_EVENT_BYTES - 1);
    Require(!DecodeTimingDesiredFrameRate(shortEvent), "a truncated event is rejected");
    Require(!DecodeTimingDesiredFrameRate({}), "an empty span is rejected");
}

// 单机 v4/v5：第 0 帧带速度，之后只在变化时带；空表示沿用前值。
void CheckRecordedSpeedChanges()
{
    std::vector<FrameInput> frames(40);
    std::map<std::uint32_t, std::uint32_t> changes{{0, 4}, {10, 6}, {25, 1}, {26, 1}, {33, 3}};
    for (const auto& [frame, speed] : changes)
        frames[frame].mGameSpeed = speed;
    // 无关事件（含 Timing）不影响单机时长。
    frames[12].mEvents = {MakeTiming(10), EventBuilder(NativeEventType::MegaMission, 0).Build()};
    const GameTimeline timeline = Build(TimelineTiming::RecordedGameSpeed, frames);

    std::vector<int> speeds;
    int speed = 0;
    for (std::uint32_t frame = 0; frame < frames.size(); ++frame)
    {
        if (frames[frame].mGameSpeed)
            speed = static_cast<int>(*frames[frame].mGameSpeed);
        speeds.push_back(speed);
    }
    Require(timeline.mCumulativeMs[0] == 64, "frame 0 at speed 4 is 64 ms");
    Require(timeline.mCumulativeMs[9] == 10 * 64, "ten frames at speed 4");
    Require(timeline.mCumulativeMs[10] == 10 * 64 + 96, "the recorded speed applies to its own frame");
    Require(!timeline.mFirstNominalFrame, "no nominal frame");
    RequireSameAsGameClock(timeline, false, speeds, "recorded speeds");
}

// 速度 0（原生不限速）与越界值：按 16 ms 计并从那一帧起标名义，与 GetFrameDuration 相同。
void CheckRecordedSpeedZeroAndOutOfRange()
{
    std::vector<FrameInput> frames(20);
    frames[0].mGameSpeed = 2;
    frames[5].mGameSpeed = 0;
    frames[8].mGameSpeed = 3;
    frames[12].mGameSpeed = 7;
    frames[14].mGameSpeed = 0xFFFFFFFFu;
    frames[16].mGameSpeed = 5;
    const GameTimeline timeline = Build(TimelineTiming::RecordedGameSpeed, frames);
    Require(timeline.mFirstNominalFrame == 5u, "the first speed-0 frame starts the nominal range");
    Require(!timeline.GetTimeAt(4)->mIsNominal && timeline.GetTimeAt(5)->mIsNominal && timeline.GetTimeAt(19)->mIsNominal,
            "nominal starts at frame 5 and never ends");
    Require(timeline.mCumulativeMs[5] - timeline.mCumulativeMs[4] == NOMINAL_FRAME_MS, "speed 0 counts 16 ms");
    Require(timeline.mCumulativeMs[12] - timeline.mCumulativeMs[11] == NOMINAL_FRAME_MS, "speed 7 counts 16 ms");
    Require(timeline.mCumulativeMs[14] - timeline.mCumulativeMs[13] == NOMINAL_FRAME_MS, "a huge speed counts 16 ms");

    std::vector<int> speeds;
    int speed = 0;
    for (const FrameInput& frame : frames)
    {
        if (frame.mGameSpeed)
            speed = *frame.mGameSpeed > 1000u ? 1000 : static_cast<int>(*frame.mGameSpeed);
        speeds.push_back(speed);
    }
    RequireSameAsGameClock(timeline, false, speeds, "speed 0 and out of range");

    std::vector<FrameInput> zeroStart(3);
    zeroStart[0].mGameSpeed = 0;
    const GameTimeline zeroTimeline = Build(TimelineTiming::RecordedGameSpeed, zeroStart);
    Require(zeroTimeline.mFirstNominalFrame == 0u && zeroTimeline.mCumulativeMs[2] == 3 * NOMINAL_FRAME_MS,
            "a replay recorded at speed 0 is nominal from frame 0");
}

// 联机：初值 60（16 ms），Timing 事件从执行它的那一帧本身起计入。
void CheckNetworkTiming()
{
    std::vector<FrameInput> frames(50);
    frames[0].mEvents = {EventBuilder(NativeEventType::FrameInfo, 0).Build()};
    frames[10].mEvents = {EventBuilder(NativeEventType::MegaMission, 1).Build(), MakeTiming(30)};
    // 同一帧两条：按执行顺序，后一条生效。
    frames[20].mEvents = {MakeTiming(45), MakeTiming(20)};
    // 0 原样写入 DesiredFrameRate，帧时长取 33 ms。
    frames[30].mEvents = {MakeTiming(0)};
    frames[35].mEvents = {MakeTiming(60, 2)};
    // 联机帧流里的 GameSpeed 事件不影响时长（统计时钟只看 DesiredFrameRate）。
    frames[40].mEvents = {EventBuilder(NativeEventType::GameSpeed, 0).U32(0, 1).Build()};
    const GameTimeline timeline = Build(TimelineTiming::NetworkFrameRate, frames);

    Require(timeline.mCumulativeMs[0] == 16, "the first network frame uses the spawner rate 60");
    Require(timeline.mCumulativeMs[9] == 10 * 16, "ten frames at 60");
    Require(timeline.mCumulativeMs[10] == 10 * 16 + 33, "a Timing event counts from its own frame");
    Require(timeline.mCumulativeMs[20] - timeline.mCumulativeMs[19] == 50, "the last Timing of a frame wins");
    Require(timeline.mCumulativeMs[30] - timeline.mCumulativeMs[29] == NETWORK_FALLBACK_FRAME_MS, "rate 0 counts 33 ms");
    Require(timeline.mCumulativeMs[35] - timeline.mCumulativeMs[34] == 16, "rate 60 again");
    Require(!timeline.mFirstNominalFrame, "network time is never marked nominal");

    std::vector<int> rates;
    int rate = NETWORK_INITIAL_DESIRED_FRAME_RATE;
    for (std::uint32_t frame = 0; frame < frames.size(); ++frame)
    {
        if (frame == 10)
            rate = 30;
        else if (frame == 20)
            rate = 20;
        else if (frame == 30)
            rate = 0;
        else if (frame == 35)
            rate = 60;
        rates.push_back(rate);
    }
    RequireSameAsGameClock(timeline, true, rates, "network");
}

// 单机 v1–v3：只记帧数，需要运行时速度；拿到后整段按同一速度换算，并从第 0 帧起标名义。
void CheckRuntimeSpeed()
{
    GameTimelineBuilder builder(TimelineTiming::RuntimeGameSpeed);
    Require(builder.NeedsRuntimeGameSpeed(), "v1-v3 replays need the runtime speed");
    for (std::uint32_t frame = 0; frame < 30; ++frame)
        Require(builder.AddFrame(frame, std::nullopt, {}), "frames without speed are accepted");
    Require(builder.GetFrameCount() == 30, "only the frame count is kept");
    Require(!builder.TakeTimeline(), "no timeline before the runtime speed is known");
    Require(builder.GetFrameCount() == 30 && !builder.HasFailed(), "asking early leaves the builder unchanged");

    const auto timeline = builder.TakeTimeline(3);
    Require(timeline && timeline->GetFrameCount() == 30 && timeline->mCumulativeMs[29] == 30 * 48,
            "thirty frames at speed 3");
    Require(timeline->mFirstNominalFrame == 0u, "the whole timeline is nominal");
    RequireSameAsGameClock(*timeline, false, std::vector<int>(30, 3), "runtime speed", true);
    Require(builder.GetFrameCount() == 0, "taking the timeline resets the builder");

    GameTimelineBuilder zeroBuilder(TimelineTiming::RuntimeGameSpeed);
    for (std::uint32_t frame = 0; frame < 5; ++frame)
        zeroBuilder.AddFrame(frame, std::nullopt, {});
    const auto zeroTimeline = zeroBuilder.TakeTimeline(0);
    Require(zeroTimeline && zeroTimeline->mCumulativeMs[4] == 5 * NOMINAL_FRAME_MS, "runtime speed 0 counts 16 ms");
    RequireSameAsGameClock(*zeroTimeline, false, std::vector<int>(5, 0), "runtime speed 0", true);

    // 带速度的帧不属于 v1–v3 帧流。
    GameTimelineBuilder invalid(TimelineTiming::RuntimeGameSpeed);
    Require(!invalid.AddFrame(0, 3u, {}) && invalid.HasFailed(), "a recorded speed in a v1-v3 stream fails");
}

void CheckInvalidInput()
{
    {
        GameTimelineBuilder builder(TimelineTiming::RecordedGameSpeed);
        Require(!builder.NeedsRuntimeGameSpeed(), "recorded speeds need nothing at runtime");
        Require(!builder.AddFrame(0, std::nullopt, {}) && builder.HasFailed() && !builder.GetError().empty(),
                "frame 0 without a speed fails");
        Require(!builder.AddFrame(0, 3u, {}), "a failed builder rejects later frames");
        Require(!builder.TakeTimeline(), "a failed builder yields no timeline");
    }
    {
        GameTimelineBuilder builder(TimelineTiming::RecordedGameSpeed);
        Require(builder.AddFrame(0, 3u, {}) && builder.AddFrame(1, std::nullopt, {}), "contiguous frames");
        Require(!builder.AddFrame(3, std::nullopt, {}), "a gap fails");
    }
    {
        GameTimelineBuilder builder(TimelineTiming::NetworkFrameRate);
        Require(!builder.AddFrame(1, std::nullopt, {}), "a stream must start at frame 0");
    }
    {
        GameTimelineBuilder builder(TimelineTiming::NetworkFrameRate);
        Require(builder.AddFrame(0, std::nullopt, {}) && !builder.AddFrame(0, std::nullopt, {}),
                "a repeated frame fails");
    }
    {
        GameTimelineBuilder builder(TimelineTiming::NetworkFrameRate);
        Require(!builder.AddFrame(0, 4u, {}), "a network stream cannot carry speeds");
    }
    {
        GameTimelineBuilder builder(TimelineTiming::NetworkFrameRate);
        Require(!builder.TakeTimeline(), "no frames, no timeline");
    }
}
}

int main()
{
    try
    {
        CheckTimingDecode();
        CheckRecordedSpeedChanges();
        CheckRecordedSpeedZeroAndOutOfRange();
        CheckNetworkTiming();
        CheckRuntimeSpeed();
        CheckInvalidInput();
        std::cout << "Game timeline builder tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
