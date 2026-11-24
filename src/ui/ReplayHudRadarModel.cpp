#include "ReplayHudRadarModel.h"

#include <algorithm>
#include <iterator>

namespace rareplay::ui
{
void RadarEventAges::Reset() noexcept
{
    mNowMs = 0;
    mHasNow = false;
    mFrozenMs.clear();
}

void RadarEventAges::Update(const viewer::RadarEventList* events, std::int64_t gameNowMs, bool isHovered)
{
    const std::int64_t delta = mHasNow && gameNowMs > mNowMs ? gameNowMs - mNowMs : 0;
    mNowMs = gameNowMs;
    mHasNow = true;
    if (!events)
    {
        mFrozenMs.clear();
        return;
    }
    // 先按本帧之前的状态判断事件是否仍在显示期内，再累计暂停；年龄已超期的事件之后不会再回到显示期。
    for (const viewer::RadarEvent& event : events->mEvents)
    {
        const std::int64_t frozen = [&]() -> std::int64_t
        {
            const auto found = mFrozenMs.find(event.mId);
            return found == mFrozenMs.end() ? 0 : found->second;
        }();
        const std::int64_t age = std::max<std::int64_t>(0, gameNowMs - event.mGameTimeMs - frozen);
        if (!viewer::IsRadarEventRecent(age))
        {
            mFrozenMs.erase(event.mId);
            continue;
        }
        // 只累计事件存在的那一段：上一帧到现在这段时间里，事件在 mGameTimeMs 之后才存在。
        const std::int64_t existed = std::clamp<std::int64_t>(gameNowMs - event.mGameTimeMs, 0, delta);
        if (isHovered && existed > 0)
            mFrozenMs[event.mId] = frozen + existed;
    }
    // 事件已被历史上限丢弃时一并清掉状态。
    if (!mFrozenMs.empty())
    {
        const std::uint64_t oldestId = events->mEvents.empty() ? UINT64_MAX : events->mEvents.front().mId;
        for (auto iterator = mFrozenMs.begin(); iterator != mFrozenMs.end();)
            iterator = iterator->first < oldestId ? mFrozenMs.erase(iterator) : std::next(iterator);
    }
}

std::int64_t RadarEventAges::GetAgeMs(const viewer::RadarEvent& event) const noexcept
{
    const auto found = mFrozenMs.find(event.mId);
    const std::int64_t frozen = found == mFrozenMs.end() ? 0 : found->second;
    return std::max<std::int64_t>(0, mNowMs - event.mGameTimeMs - frozen);
}

std::vector<RadarRow> SelectRecentRadarRows(const viewer::RadarEventList* events, const RadarEventAges& ages)
{
    std::vector<RadarRow> rows;
    if (!events)
        return rows;
    for (auto iterator = events->mEvents.rbegin();
         iterator != events->mEvents.rend() && rows.size() < viewer::RADAR_EVENT_RECENT_LIMIT; ++iterator)
    {
        const std::int64_t age = ages.GetAgeMs(*iterator);
        if (!viewer::IsRadarEventRecent(age))
            continue;
        rows.push_back({&*iterator, age, viewer::GetRadarEventOpacity(age)});
    }
    return rows;
}

std::vector<RadarRow> SelectHistoryRadarRows(const viewer::RadarEventList* events)
{
    std::vector<RadarRow> rows;
    if (!events)
        return rows;
    rows.reserve(events->mEvents.size());
    for (auto iterator = events->mEvents.rbegin(); iterator != events->mEvents.rend(); ++iterator)
        rows.push_back({&*iterator, 0, 1.0f});
    return rows;
}

std::vector<RadarTextSegment> BuildRadarEventText(const viewer::RadarEvent& event)
{
    if (event.mKind == viewer::RadarEventKind::SuperWeaponLaunched)
        return {{event.mActor.mName, RadarTextRole::Actor}, {" 释放了 ", RadarTextRole::Plain},
                {event.mSubjectName, RadarTextRole::Plain}};
    return {{event.mActor.mName, RadarTextRole::Actor}, {" 的间谍渗透了 ", RadarTextRole::Plain},
            {event.mVictim.mName, RadarTextRole::Victim}, {" 的 ", RadarTextRole::Plain},
            {event.mSubjectName, RadarTextRole::Plain}};
}

std::string JoinRadarEventText(const std::vector<RadarTextSegment>& segments)
{
    std::string text;
    for (const RadarTextSegment& segment : segments)
        text += segment.mText;
    return text;
}
}
