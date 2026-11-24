#include "../game_api/PlayerInputEffects.h"
#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"

#include <Syringe.h>

// 仅记录本地移动点击在 0x4D7F93 创建的反馈动画；原构造函数继续执行。
DEFINE_HOOK(0x421EA0, ReplayMovementFeedbackHook, 6)
{
    if (R->Stack<DWORD>(0) != rareplay::target::MOVE_FLASH_ANIMATION_RETURN ||
        !rareplay::plugin::ShouldCapturePlayerInputEffect())
        return 0;

    rareplay::replay::RecordedInputEffect effect;
    const auto result = rareplay::game_api::PlayerInputEffects::TryBuildMovementFeedback(
        reinterpret_cast<const void*>(R->Stack<DWORD>(4)),
        reinterpret_cast<const void*>(R->Stack<DWORD>(8)),
        R->Stack<std::int32_t>(12), R->Stack<std::int32_t>(16),
        R->Stack<std::uint32_t>(20), R->Stack<std::int32_t>(24),
        R->Stack<std::uint32_t>(28), effect);
    if (result == rareplay::game_api::MovementFeedbackCaptureResult::Captured)
        rareplay::plugin::OnPlayerInputEffectCaptured(effect);
    else if (result == rareplay::game_api::MovementFeedbackCaptureResult::Invalid)
        rareplay::plugin::OnPlayerInputEffectCaptureFailed();
    return 0;
}
