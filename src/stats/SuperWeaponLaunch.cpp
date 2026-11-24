#include "SuperWeaponLaunch.h"

namespace rareplay::stats
{
bool IsSuperWeaponLaunched(const SuperWeaponFireBegin& before, const SuperWeaponFireEnd& after) noexcept
{
    if (before.mSuperToken != after.mSuperToken)
        return false;
    switch (before.mMode)
    {
    case SuperWeaponClickMode::ChargeDrain:
        return before.mChargeDrainState == CHARGE_DRAIN_READY && after.mChargeDrainState == CHARGE_DRAIN_DRAINING;
    case SuperWeaponClickMode::PreClick:
        return false;
    case SuperWeaponClickMode::PostClick:
        return before.mHasPrerequisite && before.mPrerequisiteIsReady;
    case SuperWeaponClickMode::Normal:
        break;
    }
    return before.mIsReady && !after.mIsReady;
}

void SuperWeaponFireTracker::Begin(const SuperWeaponFireBegin& before)
{
    if (mStack.size() >= MAX_DEPTH)
    {
        mStack.erase(mStack.begin());
        ++mDiscarded;
    }
    mStack.push_back(before);
}

std::optional<SuperWeaponLaunch> SuperWeaponFireTracker::End(const SuperWeaponFireEnd& after)
{
    for (std::size_t index = mStack.size(); index > 0; --index)
    {
        if (mStack[index - 1].mSuperToken != after.mSuperToken)
            continue;
        const SuperWeaponFireBegin before = mStack[index - 1];
        // 其上的残留项没有等到自己的 C2，视为丢失。
        mDiscarded += static_cast<std::uint32_t>(mStack.size() - index);
        mStack.resize(index - 1);
        if (!IsSuperWeaponLaunched(before, after))
            return std::nullopt;
        SuperWeaponLaunch launch;
        launch.mFrame = before.mFrame;
        launch.mHouseIndex = before.mHouseIndex;
        launch.mTypeIndex = before.mTypeIndex;
        launch.mIconTypeIndex = before.mMode == SuperWeaponClickMode::PostClick && before.mHasPrerequisite &&
                                before.mPrerequisiteTypeIndex >= 0
            ? before.mPrerequisiteTypeIndex : before.mTypeIndex;
        launch.mMode = before.mMode;
        launch.mTargetCell = before.mTargetCell;
        return launch;
    }
    ++mDiscarded;
    return std::nullopt;
}

void SuperWeaponFireTracker::Clear() noexcept
{
    mDiscarded += static_cast<std::uint32_t>(mStack.size());
    mStack.clear();
}
}
