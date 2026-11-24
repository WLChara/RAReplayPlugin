#pragma once

#include "RecordedControlGroups.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace rareplay::replay
{
using RecordedEvent = std::array<std::uint8_t, 111>;

enum class PlayerInputEffectKind : std::uint32_t
{
    MovementFeedback = 1
};

struct RecordedInputEffect
{
    PlayerInputEffectKind mKind = PlayerInputEffectKind::MovementFeedback;
    std::uint32_t mAnimationTypeIndex = 0;
    std::int32_t mX = 0;
    std::int32_t mY = 0;
    std::int32_t mZ = 0;
    std::int32_t mLoopDelay = 0;
    std::int32_t mLoopCount = 1;
    std::uint32_t mFlags = 0;
    std::int32_t mForceZAdjust = 0;
    bool mReverse = false;
};

struct ReplayFrame
{
    std::uint32_t mFrameNumber = 0;
    std::uint32_t mChecksum = 0;
    bool mHasChecksum = false;
    std::vector<RecordedEvent> mEvents;
    std::vector<RecordedInputEffect> mInputEffects;
    std::optional<RecordedControlGroupSnapshot> mRecordedGroups;
    /**
     * v4 起单机帧流记录的“本帧起的游戏速度”（GameOptionsClass::GameSpeed，0..6）。
     * 空表示沿用前帧；单机帧流第 0 帧必有，联机帧流与 v1–3 帧流一律为空。
     */
    std::optional<std::uint32_t> mGameSpeed;
};

struct ReplaySessionInfo
{
    std::uint32_t mGameMode = 0;
    std::uint32_t mLocalHouseIndex = 0;
    std::uint32_t mFirstFrame = 0;
    std::uint32_t mFrameSendRate = 0;
    std::uint32_t mRandomSeed = 0;
    std::uint32_t mStreamVersion = 2;
};
}
