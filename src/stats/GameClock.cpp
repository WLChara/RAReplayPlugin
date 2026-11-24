#include "GameClock.h"

#include <algorithm>

namespace rareplay::stats
{
namespace
{
// 帧号一次前进超过它时按它封顶，避免异常帧号造成时间突变；正常运行每次只前进 1。
constexpr std::uint32_t MAX_FRAME_GAP = 600;
}

FrameDuration GetFrameDuration(const FrameTimingInput& input) noexcept
{
    if (input.mIsNetwork)
    {
        if (input.mDesiredFrameRate <= 0)
            return {NETWORK_FALLBACK_FRAME_MS, false};
        return {std::max(1, 1000 / input.mDesiredFrameRate), false};
    }
    if (input.mGameSpeed < 1 || input.mGameSpeed > MAX_GAME_SPEED)
        return {NOMINAL_FRAME_MS, true};
    return {input.mGameSpeed * TIMER_TICK_MS, false};
}

bool GameClock::OnFrameCompleted(std::uint32_t frameNumber, const FrameTimingInput& input) noexcept
{
    std::uint32_t frames = 1;
    if (mLastFrame)
    {
        if (frameNumber <= *mLastFrame)
            return false;
        frames = std::min(frameNumber - *mLastFrame, MAX_FRAME_GAP);
    }
    mLastFrame = frameNumber;
    const FrameDuration duration = GetFrameDuration(input);
    mMilliseconds += static_cast<std::int64_t>(duration.mMilliseconds) * frames;
    mIsNominal = mIsNominal || duration.mIsNominal;
    return true;
}

void GameClock::Reset() noexcept
{
    *this = GameClock();
}
}
