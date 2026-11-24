#include "RadarEventLog.h"

#include "../viewer/RadarDisplayRules.h"

#include <algorithm>
#include <utility>

namespace rareplay::stats
{
viewer::RadarPlayerRef MakeNeutralPlayerRef()
{
    viewer::RadarPlayerRef player;
    player.mHouseIndex = -1;
    player.mName = NEUTRAL_PLAYER_NAME;
    return player;
}

RadarEventDraft MakeSuperWeaponLaunchDraft(viewer::RadarPlayerRef actor, std::string superWeaponName,
    std::shared_ptr<const viewer::IconImage> icon, const viewer::WorldPoint& location)
{
    RadarEventDraft draft;
    draft.mKind = viewer::RadarEventKind::SuperWeaponLaunched;
    draft.mActor = std::move(actor);
    draft.mSubjectName = std::move(superWeaponName);
    draft.mIcon = std::move(icon);
    draft.mLocation = location;
    return draft;
}

RadarEventDraft MakeInfiltrationDraft(viewer::RadarPlayerRef spyOwner, viewer::RadarPlayerRef buildingOwner,
    std::string buildingName, const viewer::WorldPoint& location)
{
    RadarEventDraft draft;
    draft.mKind = viewer::RadarEventKind::BuildingInfiltrated;
    draft.mActor = std::move(spyOwner);
    draft.mVictim = std::move(buildingOwner);
    draft.mSubjectName = std::move(buildingName);
    draft.mLocation = location;
    return draft;
}

void RadarEventLog::Reset()
{
    mEvents.clear();
    mNextId = 1;
    mCached.reset();
}

std::uint64_t RadarEventLog::Add(RadarEventDraft draft, std::int64_t gameTimeMs)
{
    viewer::RadarEvent event;
    event.mId = mNextId++;
    event.mKind = draft.mKind;
    event.mGameTimeMs = gameTimeMs;
    event.mActor = std::move(draft.mActor);
    event.mVictim = std::move(draft.mVictim);
    event.mSubjectName = std::move(draft.mSubjectName);
    event.mIcon = std::move(draft.mIcon);
    event.mLocation = draft.mLocation;
    const std::uint64_t id = event.mId;
    mEvents.push_back(std::move(event));
    if (mEvents.size() > viewer::RADAR_EVENT_HISTORY_LIMIT)
        mEvents.erase(mEvents.begin(), mEvents.begin() +
            static_cast<std::ptrdiff_t>(mEvents.size() - viewer::RADAR_EVENT_HISTORY_LIMIT));
    mCached.reset();
    return id;
}

const viewer::RadarEvent* RadarEventLog::Find(std::uint64_t id) const noexcept
{
    // 编号单调递增，事件按编号有序。
    const auto found = std::lower_bound(mEvents.begin(), mEvents.end(), id,
        [](const viewer::RadarEvent& event, std::uint64_t value) { return event.mId < value; });
    return found != mEvents.end() && found->mId == id ? &*found : nullptr;
}

std::shared_ptr<const viewer::RadarEventList> RadarEventLog::GetSnapshot()
{
    if (!mCached)
    {
        auto list = std::make_shared<viewer::RadarEventList>();
        list->mEvents = mEvents;
        mCached = std::move(list);
    }
    return mCached;
}
}
