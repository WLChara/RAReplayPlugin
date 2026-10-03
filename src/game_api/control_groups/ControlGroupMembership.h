#pragma once

#include "../../replay/RecordedControlGroups.h"

namespace rareplay::game_api
{
/** 记录仍存活且可控的成员；暂时离场不解除编队。 */
constexpr bool IsRecordedControlGroupMember(int groupIndex, bool isAlive,
    int health, bool isControlled) noexcept
{
    return isAlive && health > 0 && isControlled && groupIndex >= 0 &&
        groupIndex < static_cast<int>(replay::RECORDED_CONTROL_GROUP_COUNT);
}
}
