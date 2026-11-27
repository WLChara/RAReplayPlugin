#pragma once

#include "../replay/ReplayTypes.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace rareplay::game_api
{
enum class MovementFeedbackCaptureResult
{
    Ignored,
    Captured,
    Invalid
};

/** 游戏线程上的玩家输入副作用缓冲；重复进入同一帧不清除未提交记录。 */
class PlayerInputEffects final
{
public:
    bool BeginFrame(std::uint32_t frameNumber, std::string& outError);
    bool Append(const replay::RecordedInputEffect& effect, std::string& outError);
    bool Drain(std::uint32_t frameNumber,
        std::vector<replay::RecordedInputEffect>& outEffects, std::string& outError);
    /** 进程内重开时回到构造后的状态：丢弃未取走的记录，忘掉上一场的帧号。 */
    void Reset() noexcept;
    static MovementFeedbackCaptureResult TryBuildMovementFeedback(const void* animationType,
        const void* coordinates, std::int32_t loopDelay, std::int32_t loopCount,
        std::uint32_t flags, std::int32_t forceZAdjust, std::uint32_t reverse,
        replay::RecordedInputEffect& outEffect) noexcept;
    static bool Apply(const std::vector<replay::RecordedInputEffect>& effects,
        std::string& outError);

private:
    static constexpr std::size_t MAX_EFFECTS_PER_FRAME = 4096;

    std::vector<replay::RecordedInputEffect> mEffects;
    std::uint32_t mFrameNumber = 0;
    bool mHasFrame = false;
};
}
