#include "SuperWeaponMarkTracker.h"

#include "../viewer/RadarDisplayRules.h"

#include <algorithm>
#include <utility>

namespace rareplay::stats
{
void SuperWeaponMarkTracker::Add(std::shared_ptr<const viewer::SuperWeaponMarkInfo> info,
    const viewer::WorldPoint& world)
{
    if (!info)
        return;
    mMarks.push_back({std::move(info), world});
    if (mMarks.size() > viewer::SUPERWEAPON_MARK_LIMIT)
        mMarks.erase(mMarks.begin(), mMarks.begin() +
            static_cast<std::ptrdiff_t>(mMarks.size() - viewer::SUPERWEAPON_MARK_LIMIT));
}

bool SuperWeaponMarkTracker::Prune(std::int64_t gameTimeMs)
{
    const auto expired = std::remove_if(mMarks.begin(), mMarks.end(),
        [gameTimeMs](const ActiveSuperWeaponMark& mark)
        {
            return viewer::IsSuperWeaponMarkExpired(gameTimeMs - mark.mInfo->mReleasedAtMs);
        });
    const bool hasRemoved = expired != mMarks.end();
    mMarks.erase(expired, mMarks.end());
    return hasRemoved;
}
}
