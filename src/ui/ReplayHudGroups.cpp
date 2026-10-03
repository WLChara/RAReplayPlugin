#include "ReplayHudParts.h"

namespace rareplay::ui::detail
{
void DrawRecordedGroups(const viewer::ReplayViewSnapshot& snapshot)
{
    const char* localName = "未知";
    for (const auto& player : snapshot.mPlayers)
    {
        if (player.mHouseIndex == snapshot.mRecordedLocalHouseIndex)
        {
            localName = player.mName.empty() ? "未命名玩家" : player.mName.c_str();
            break;
        }
    }
    ImGui::TextWrapped("录制方：%s", localName);
    ImGui::TextDisabled("仅显示该玩家的本地编队");
    if (!snapshot.mHasRecordedGroups)
    {
        ImGui::TextDisabled("此录像未记录编队历史");
        return;
    }
    if (snapshot.mRecordedGroups.empty())
    {
        ImGui::TextDisabled("录制方没有已保存编队");
        return;
    }
    for (const auto& group : snapshot.mRecordedGroups)
    {
        ImGui::PushID(group.mNumber);
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.93f, 0.72f, 0.36f, 1.0f), "编队 %d", group.mNumber);
        if (group.mUnits.empty() && group.mUnmatchedMemberCount == 0)
        {
            ImGui::TextDisabled("无单位");
        }
        else
        {
            for (const auto& unit : group.mUnits)
            {
                const char* name = unit.mName.empty() ? unit.mTypeId.c_str() : unit.mName.c_str();
                ImGui::BulletText("%s × %u", name, unit.mCount);
            }
        }
        if (group.mUnmatchedMemberCount != 0)
            ImGui::TextDisabled("未匹配成员：%u", group.mUnmatchedMemberCount);
        ImGui::PopID();
    }
}
}
