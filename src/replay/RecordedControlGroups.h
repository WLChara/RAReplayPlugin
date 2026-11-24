#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rareplay::replay
{
constexpr std::uint32_t RECORDED_CONTROL_GROUP_COUNT = 10;
constexpr std::uint32_t MAX_RECORDED_GROUP_MEMBERS = 16'384;
// packed NetID 的通用 Abstract 目标标记；不代表具体单位类别。
constexpr std::uint32_t RECORDED_GROUP_TARGET_KIND = 52;

struct RecordedGroupMember
{
    std::uint32_t mGroupIndex = 0;
    std::uint32_t mObjectId = 0;
    std::uint32_t mTargetKind = RECORDED_GROUP_TARGET_KIND;

    bool operator==(const RecordedGroupMember&) const = default;
};

/** 录制方的完整编队表；成员按 (编队号, 对象 ID) 严格升序排列。 */
struct RecordedControlGroupSnapshot
{
    std::vector<RecordedGroupMember> mMembers;

    bool operator==(const RecordedControlGroupSnapshot&) const = default;
};

/**
 * 验证帧的编队元数据。v3 起（含 v4、v5）第 0 帧必须有快照；空快照清空全部编队，
 * 缺失快照表示沿用前帧。v1/v2 不得携带编队数据。失败时返回 false。
 */
bool ValidateRecordedControlGroups(
    const std::optional<RecordedControlGroupSnapshot>& snapshot,
    std::uint32_t frameNumber, std::uint32_t streamVersion, std::string& error);
}
