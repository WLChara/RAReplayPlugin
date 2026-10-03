#include "RecordedControlGroups.h"

#include <tuple>
#include <unordered_set>

namespace rareplay::replay
{
bool ValidateRecordedControlGroups(
    const std::optional<RecordedControlGroupSnapshot>& snapshot,
    std::uint32_t frameNumber, std::uint32_t streamVersion, std::string& error)
{
    if (streamVersion < 1 || streamVersion > 3 || (streamVersion < 3 && snapshot))
    {
        error = "编队数据与帧流版本不兼容";
        return false;
    }
    if (streamVersion == 3 && frameNumber == 0 && !snapshot)
    {
        error = "编队帧流缺少初始完整快照";
        return false;
    }
    if (!snapshot)
    {
        error.clear();
        return true;
    }
    if (snapshot->mMembers.size() > MAX_RECORDED_GROUP_MEMBERS)
    {
        error = "编队成员数量超过上限";
        return false;
    }
    std::unordered_set<std::uint32_t> objectIds;
    objectIds.reserve(snapshot->mMembers.size());
    const RecordedGroupMember* previous = nullptr;
    for (const auto& member : snapshot->mMembers)
    {
        if (member.mGroupIndex >= RECORDED_CONTROL_GROUP_COUNT || member.mObjectId == 0 ||
            member.mTargetKind != RECORDED_GROUP_TARGET_KIND)
        {
            error = "编队号或成员对象标识无效";
            return false;
        }
        if (previous && std::tie(previous->mGroupIndex, previous->mObjectId) >=
            std::tie(member.mGroupIndex, member.mObjectId))
        {
            error = "编队成员没有按约定严格排序";
            return false;
        }
        if (!objectIds.insert(member.mObjectId).second)
        {
            error = "编队成员对象 ID 重复";
            return false;
        }
        previous = &member;
    }
    error.clear();
    return true;
}
}
