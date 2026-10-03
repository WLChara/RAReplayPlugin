#include "RecordedControlGroupProjection.h"

#include <map>
#include <utility>

namespace rareplay::game_api
{
void ProjectRecordedControlGroups(
    const std::optional<replay::RecordedControlGroupSnapshot>& recordedGroups,
    const std::unordered_map<std::uint32_t, viewer::UnitSummary>& resolvedMembers,
    viewer::ReplayViewSnapshot& outView)
{
    outView.mHasRecordedGroups = recordedGroups.has_value();
    outView.mRecordedGroups.clear();
    if (!recordedGroups)
        return;
    std::map<std::uint32_t, viewer::ControlGroup> groups;
    std::map<std::uint32_t, std::map<std::pair<int, std::string>, viewer::UnitSummary>> summaries;
    for (const auto& member : recordedGroups->mMembers)
    {
        auto& group = groups[member.mGroupIndex];
        group.mNumber = static_cast<int>(member.mGroupIndex + 1);
        const auto resolved = resolvedMembers.find(member.mObjectId);
        if (resolved == resolvedMembers.end())
        {
            ++group.mUnmatchedMemberCount;
            continue;
        }
        const auto& unit = resolved->second;
        auto& summary = summaries[member.mGroupIndex][{static_cast<int>(unit.mCategory), unit.mTypeId}];
        if (summary.mCount == 0)
        {
            summary = unit;
            summary.mCount = 0;
        }
        ++summary.mCount;
    }
    for (auto& [groupIndex, group] : groups)
    {
        for (auto& [key, summary] : summaries[groupIndex])
            group.mUnits.push_back(std::move(summary));
        outView.mRecordedGroups.push_back(std::move(group));
    }
}
}
