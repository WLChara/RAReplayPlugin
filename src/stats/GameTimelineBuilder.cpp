#include "GameTimelineBuilder.h"

#include "GameClock.h"

#include <climits>
#include <utility>

namespace rareplay::stats
{
namespace
{
// 速度原值超出 int 时按越界值处理；GetFrameDuration 对 0..6 以外的值一律计名义 16 ms。
int ToSpeedInput(std::uint32_t speed) noexcept
{
    return speed > static_cast<std::uint32_t>(MAX_GAME_SPEED) ? MAX_GAME_SPEED + 1 : static_cast<int>(speed);
}
}

std::optional<int> DecodeTimingDesiredFrameRate(std::span<const std::uint8_t> event) noexcept
{
    if (event.size() < NATIVE_EVENT_BYTES ||
        event[EVENT_TYPE_OFFSET] != static_cast<std::uint8_t>(NativeEventType::Timing))
        return std::nullopt;
    const std::size_t offset = EVENT_PAYLOAD_OFFSET + event_layout::TIMING_REQUESTED_FPS;
    return static_cast<int>(static_cast<std::uint16_t>(event[offset] | (event[offset + 1] << 8)));
}

bool GameTimelineBuilder::AddFrame(std::uint32_t frameNumber, std::optional<std::uint32_t> gameSpeed,
    std::span<const NativeEventBytes> events)
{
    if (HasFailed())
        return false;
    if (frameNumber != mFrameCount || mFrameCount == UINT32_MAX)
        return Fail("Timeline frame " + std::to_string(frameNumber) + " is not the next frame " +
            std::to_string(mFrameCount));

    FrameTimingInput input;
    switch (mTiming)
    {
    case TimelineTiming::RecordedGameSpeed:
        if (gameSpeed)
            mGameSpeed = gameSpeed;
        else if (frameNumber == 0)
            return Fail("Timeline frame 0 carries no game speed");
        input.mGameSpeed = ToSpeedInput(*mGameSpeed);
        break;
    case TimelineTiming::RuntimeGameSpeed:
        if (gameSpeed)
            return Fail("Timeline frame " + std::to_string(frameNumber) + " carries a game speed without recorded speeds");
        ++mFrameCount;
        return true;
    case TimelineTiming::NetworkFrameRate:
        if (gameSpeed)
            return Fail("Timeline frame " + std::to_string(frameNumber) + " carries a game speed in a network replay");
        // 同一帧有多条 Timing 时按执行顺序，最后一条生效。
        for (const NativeEventBytes& event : events)
        {
            if (const auto rate = DecodeTimingDesiredFrameRate(event))
                mDesiredFrameRate = *rate;
        }
        input.mIsNetwork = true;
        input.mDesiredFrameRate = mDesiredFrameRate;
        break;
    }

    const FrameDuration duration = GetFrameDuration(input);
    const std::int64_t previous = mCumulativeMs.empty() ? 0 : mCumulativeMs.back();
    mCumulativeMs.push_back(previous + duration.mMilliseconds);
    if (duration.mIsNominal && !mFirstNominalFrame)
        mFirstNominalFrame = frameNumber;
    ++mFrameCount;
    return true;
}

std::optional<viewer::GameTimeline> GameTimelineBuilder::TakeTimeline(std::optional<int> runtimeGameSpeed)
{
    if (HasFailed() || mFrameCount == 0)
        return std::nullopt;
    viewer::GameTimeline timeline;
    if (mTiming == TimelineTiming::RuntimeGameSpeed)
    {
        if (!runtimeGameSpeed)
            return std::nullopt;
        FrameTimingInput input;
        input.mGameSpeed = *runtimeGameSpeed;
        const int frameMs = GetFrameDuration(input).mMilliseconds;
        timeline.mCumulativeMs.reserve(mFrameCount);
        for (std::uint32_t frame = 0; frame < mFrameCount; ++frame)
            timeline.mCumulativeMs.push_back(static_cast<std::int64_t>(frame + 1) * frameMs);
        // 录像不带速度，运行时速度不一定是原局的速度：整表只是名义时间。
        timeline.mFirstNominalFrame = 0;
    }
    else
    {
        timeline.mCumulativeMs = std::move(mCumulativeMs);
        timeline.mFirstNominalFrame = mFirstNominalFrame;
    }
    *this = GameTimelineBuilder(mTiming);
    return timeline;
}

bool GameTimelineBuilder::Fail(std::string error)
{
    mError = std::move(error);
    return false;
}
}
