#pragma once

#include "../../replay/RecordedControlGroups.h"
#include "../../viewer/ReplayViewModel.h"

#include <unordered_map>

namespace rareplay::game_api
{
/** 将已完成帧的编队记录投影为观看值；resolvedMembers 仅包含由原生现有 ID 匹配的单个单位。 */
void ProjectRecordedControlGroups(
    const std::optional<replay::RecordedControlGroupSnapshot>& recordedGroups,
    const std::unordered_map<std::uint32_t, viewer::UnitSummary>& resolvedMembers,
    viewer::ReplayViewSnapshot& outView);
}
