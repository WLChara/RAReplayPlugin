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
    TextWrappedFit("录制方：%s", localName);
    TextWrappedDisabled("仅显示该玩家的本地编队");
    if (!snapshot.mHasRecordedGroups)
    {
        TextWrappedDisabled("此录像未记录编队历史");
        return;
    }
    if (snapshot.mRecordedGroups.empty())
    {
        TextWrappedDisabled("录制方没有已保存编队");
        return;
    }
    // 每个编队一张卡片，卡片之间留出统一间距。
    CardListSpacing spacing;
    for (const auto& group : snapshot.mRecordedGroups)
    {
        ImGui::PushID(group.mNumber);
        {
            CardScope card;
            ImGui::TextColored(ImVec4(0.93f, 0.72f, 0.36f, 1.0f), "编队 %d", group.mNumber);
            if (group.mUnits.empty() && group.mUnmatchedMemberCount == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::Text("无单位");
                ImGui::PopStyleColor();
            }
            else
            {
                for (const auto& unit : group.mUnits)
                {
                    const char* name = unit.mName.empty() ? unit.mTypeId.c_str() : unit.mName.c_str();
                    // BulletText 不换行；窄面板改用圆点加自动换行文字（换行位置由卡片内边距决定）。
                    ImGui::Bullet();
                    ImGui::SameLine();
                    ImGui::Text("%s × %u", name, unit.mCount);
                }
            }
            if (group.mUnmatchedMemberCount != 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::Text("未匹配成员：%u", group.mUnmatchedMemberCount);
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopID();
    }
}
}
