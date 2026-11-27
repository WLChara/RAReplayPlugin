#include "PlayerInputEffects.h"

#include "native/NativeGame.h"

using byte = unsigned char;

#include <EventClass.h>
#include <Fundamentals.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <AnimClass.h>
#include <AnimTypeClass.h>
#include <Memory.h>
#include <RulesClass.h>

#include <utility>

namespace rareplay::game_api
{
bool PlayerInputEffects::BeginFrame(std::uint32_t frameNumber, std::string& outError)
{
    if (mHasFrame && mFrameNumber == frameNumber)
    {
        outError.clear();
        return true;
    }
    if (!mEffects.empty())
    {
        outError = "Player input effects were not drained before the next frame";
        return false;
    }
    mFrameNumber = frameNumber;
    mHasFrame = true;
    outError.clear();
    return true;
}

bool PlayerInputEffects::Append(const replay::RecordedInputEffect& effect,
    std::string& outError)
{
    if (!mHasFrame || mEffects.size() >= MAX_EFFECTS_PER_FRAME)
    {
        outError = "Player input effect frame is unavailable or full";
        return false;
    }
    mEffects.push_back(effect);
    outError.clear();
    return true;
}

bool PlayerInputEffects::Drain(std::uint32_t frameNumber,
    std::vector<replay::RecordedInputEffect>& outEffects, std::string& outError)
{
    outEffects.clear();
    if (!mHasFrame || mFrameNumber != frameNumber)
    {
        outError = "Player input effect frame differs from the current game frame";
        return false;
    }
    outEffects = std::move(mEffects);
    mEffects.clear();
    outError.clear();
    return true;
}

void PlayerInputEffects::Reset() noexcept
{
    mEffects.clear();
    mFrameNumber = 0;
    mHasFrame = false;
}

MovementFeedbackCaptureResult PlayerInputEffects::TryBuildMovementFeedback(
    const void* animationType, const void* coordinates, std::int32_t loopDelay,
    std::int32_t loopCount, std::uint32_t flags, std::int32_t forceZAdjust,
    std::uint32_t reverse, replay::RecordedInputEffect& outEffect) noexcept
{
    const auto mode = SessionClass::Instance.GameMode;
    if (mode == GameMode::LAN || mode == GameMode::Internet)
        return MovementFeedbackCaptureResult::Ignored;
    if ((mode != GameMode::Campaign && mode != GameMode::Skirmish) ||
        !native::IsWorldReady() || !RulesClass::Instance ||
        !RulesClass::Instance->MoveFlash || !coordinates || reverse > 1)
        return MovementFeedbackCaptureResult::Invalid;
    const auto* type = static_cast<const AnimTypeClass*>(animationType);
    if (type != RulesClass::Instance->MoveFlash || type->ArrayIndex < 0 ||
        type->ArrayIndex >= AnimTypeClass::Array.Count ||
        AnimTypeClass::Array[type->ArrayIndex] != type)
        return MovementFeedbackCaptureResult::Invalid;
    const auto& location = *static_cast<const CoordStruct*>(coordinates);
    outEffect = {};
    outEffect.mKind = replay::PlayerInputEffectKind::MovementFeedback;
    outEffect.mAnimationTypeIndex = static_cast<std::uint32_t>(type->ArrayIndex);
    outEffect.mX = location.X;
    outEffect.mY = location.Y;
    outEffect.mZ = location.Z;
    outEffect.mLoopDelay = loopDelay;
    outEffect.mLoopCount = loopCount;
    outEffect.mFlags = flags;
    outEffect.mForceZAdjust = forceZAdjust;
    outEffect.mReverse = reverse != 0;
    return MovementFeedbackCaptureResult::Captured;
}

bool PlayerInputEffects::Apply(const std::vector<replay::RecordedInputEffect>& effects,
    std::string& outError)
{
    if (effects.empty())
    {
        outError.clear();
        return true;
    }
    if (effects.size() > MAX_EFFECTS_PER_FRAME)
    {
        outError = "Replay frame exceeds the player input effect limit";
        return false;
    }
    const auto mode = SessionClass::Instance.GameMode;
    if ((mode != GameMode::Campaign && mode != GameMode::Skirmish) ||
        !native::IsWorldReady() || !RulesClass::Instance ||
        !RulesClass::Instance->MoveFlash)
    {
        outError = "Player input effects are unsupported in this game mode or phase";
        return false;
    }
    auto* type = RulesClass::Instance->MoveFlash;
    if (type->ArrayIndex < 0 || type->ArrayIndex >= AnimTypeClass::Array.Count ||
        AnimTypeClass::Array[type->ArrayIndex] != type)
    {
        outError = "Movement feedback animation type is unavailable";
        return false;
    }
    for (const auto& effect : effects)
    {
        if (effect.mKind != replay::PlayerInputEffectKind::MovementFeedback ||
            effect.mAnimationTypeIndex != static_cast<std::uint32_t>(type->ArrayIndex) ||
            effect.mLoopDelay != 0 || effect.mLoopCount != 1 ||
            effect.mFlags != 0x600 || effect.mForceZAdjust != 0 || effect.mReverse)
        {
            outError = "Unsupported movement feedback constructor parameters";
            return false;
        }
    }
    // 先校验整帧，再按记录顺序产生原生副作用，避免坏记录只重演前半帧。
    for (const auto& effect : effects)
    {
        const CoordStruct location{effect.mX, effect.mY, effect.mZ};
        if (!GameCreate<AnimClass>(type, location, effect.mLoopDelay,
                effect.mLoopCount, effect.mFlags, effect.mForceZAdjust,
                effect.mReverse))
        {
            outError = "Cannot create recorded movement feedback animation";
            return false;
        }
    }
    outError.clear();
    return true;
}
}
