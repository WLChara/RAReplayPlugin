#include "NativeControlGroups.h"
#include "../control_groups/ControlGroupMembership.h"

using byte = unsigned char;

#include <HouseClass.h>
#include <SessionClass.h>
#include <TargetClass.h>
#include <TechnoClass.h>

#include <algorithm>
#include <tuple>
#include <utility>

namespace rareplay::game_api::native
{
// 73108D/7310FD 的 Campaign 分支直接读取 Owner+1ED，不能换成 helper 的人类玩家并集。
static_assert(offsetof(HouseClass, IsInPlayerControl) == 0x1ED);

bool ReadRecordedControlGroups(replay::RecordedControlGroupSnapshot& outSnapshot,
    std::string& error)
{
    if (!HouseClass::CurrentPlayer)
    {
        error = "没有可采集编队的当前玩家";
        return false;
    }
    const auto& technos = TechnoClass::Array;
    if (technos.Count < 0 || technos.Count > 100'000 ||
        (technos.Count > 0 && !technos.Items))
    {
        error = "原生编队对象数组无效";
        return false;
    }
    replay::RecordedControlGroupSnapshot snapshot;
    for (int index = 0; index < technos.Count; ++index)
    {
        auto* techno = technos.Items[index];
        if (!techno || !techno->Owner)
            continue;
        const bool isOwned = SessionClass::Instance.GameMode == GameMode::Campaign
            ? techno->Owner->IsInPlayerControl : techno->Owner->IsControlledByCurrentPlayer();
        if (!IsRecordedControlGroupMember(techno->Group, techno->IsAlive, techno->Health, isOwned))
            continue;
        if (snapshot.mMembers.size() >= replay::MAX_RECORDED_GROUP_MEMBERS)
        {
            error = "原生编队成员数量超过上限";
            return false;
        }
        // 已核实 Pack 只取已有 UniqueID；不调用 Create_ID 或 As_Techno 的排序缓存路径。
        const TargetClass target(techno);
        snapshot.mMembers.push_back({static_cast<std::uint32_t>(techno->Group),
            static_cast<std::uint32_t>(target.m_ID), static_cast<std::uint32_t>(target.m_RTTI)});
    }
    std::sort(snapshot.mMembers.begin(), snapshot.mMembers.end(), [](const auto& left, const auto& right)
    {
        return std::tie(left.mGroupIndex, left.mObjectId) < std::tie(right.mGroupIndex, right.mObjectId);
    });
    outSnapshot = std::move(snapshot);
    error.clear();
    return true;
}
}
