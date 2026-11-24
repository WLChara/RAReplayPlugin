#include "ReplayHudParts.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace rareplay::ui::detail
{
namespace
{
constexpr ImU32 OUTAGE_COLOR = IM_COL32(241, 129, 106, 255);

void FormatOptional(char* buffer, std::size_t size, const std::optional<int>& value)
{
    if (value)
        std::snprintf(buffer, size, "%d", *value);
    else
        std::snprintf(buffer, size, "—");
}

// 行高与文字位置都按当前字号推算，窄面板缩小字号后各行仍不重叠；长文字按行宽裁剪，完整内容见提示。
void DrawPlayerRow(const viewer::PlayerSummary& player, bool isSelected, int& selectedHouseIndex)
{
    ImGui::PushID(player.mHouseIndex);
    const float lineHeight = ImGui::GetTextLineHeight();
    const CardMetrics card = GetCardMetrics();
    const float padding = card.mPadding;
    const float lineGap = std::max(1.0f, lineHeight * 0.12f);
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const float rowWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    // 前四行是姓名、身份、资金、电力；其下以分隔线隔开统计小块，宽度不够时小块自动换行。
    const float statGap = std::max(2.0f, std::round(lineGap * 2.0f));
    const std::vector<StatChip> statChips = BuildPlayerStatChips(player);
    const float statWidth = std::max(1.0f, rowWidth - padding * 2.0f);
    const float statHeight = MeasureStatChips(statChips, statWidth);
    const float headerHeight = lineHeight * 4.0f + lineGap * 3.0f;
    const float rowHeight = padding * 2.0f + headerHeight + statGap * 2.0f + statHeight;
    if (ImGui::InvisibleButton("选择玩家", ImVec2(rowWidth, rowHeight)))
    {
        selectedHouseIndex = player.mHouseIndex;
    }
    const ImVec2 rowEnd(rowStart.x + rowWidth, rowStart.y + rowHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // 卡片背景明显亮于面板并带浅色边框，选中的玩家琥珀色高亮；状态必须紧跟 InvisibleButton 取悬停。
    DrawCardBackground(drawList, rowStart, rowEnd, GetCardState(isSelected));

    const float chipSize = std::max(6.0f, lineHeight * 0.62f);
    const float textLeft = rowStart.x + padding;
    const float firstLine = rowStart.y + padding;
    const float chipTop = firstLine + (lineHeight - chipSize) * 0.5f;
    drawList->AddRectFilled(ImVec2(textLeft, chipTop), ImVec2(textLeft + chipSize, chipTop + chipSize),
                            ImGui::ColorConvertFloat4ToU32(GetPlayerColor(player.mColorRgb)), 2.0f);
    const ImVec4 clip(textLeft, rowStart.y, rowEnd.x - padding, rowEnd.y);
    const auto lineTop = [&](int line) { return firstLine + static_cast<float>(line) * (lineHeight + lineGap); };

    const char* playerName = player.mName.empty() ? "未命名玩家" : player.mName.c_str();
    const ImVec4 titleClip(textLeft + chipSize + 4.0f, rowStart.y, rowEnd.x - padding, rowEnd.y);
    drawList->AddText(nullptr, 0.0f, ImVec2(titleClip.x, lineTop(0)), IM_COL32(239, 235, 217, 255),
                      playerName, nullptr, 0.0f, &titleClip);

    char identity[160];
    std::snprintf(identity, sizeof(identity), "%s · %s%s", player.mIsHuman ? "玩家" : "AI",
                  player.mFaction.empty() ? "阵营 —" : player.mFaction.c_str(),
                  player.mDefeated ? " · 已战败" : "");
    drawList->AddText(nullptr, 0.0f, ImVec2(textLeft, lineTop(1)), IM_COL32(171, 189, 166, 255),
                      identity, nullptr, 0.0f, &clip);

    char economy[64];
    if (player.mCredits)
        std::snprintf(economy, sizeof(economy), "资金 %lld", static_cast<long long>(*player.mCredits));
    else
        std::snprintf(economy, sizeof(economy), "资金 —");
    drawList->AddText(nullptr, 0.0f, ImVec2(textLeft, lineTop(2)), IM_COL32(218, 218, 203, 255),
                      economy, nullptr, 0.0f, &clip);

    char output[24];
    char drain[24];
    FormatOptional(output, sizeof(output), player.mPowerOutput);
    FormatOptional(drain, sizeof(drain), player.mPowerDrain);
    const bool hasOutage = player.mPowerOutput && player.mPowerDrain && *player.mPowerOutput < *player.mPowerDrain;
    char power[96];
    std::snprintf(power, sizeof(power), "电力 %s/%s%s", output, drain, hasOutage ? " 停电" : "");
    drawList->AddText(nullptr, 0.0f, ImVec2(textLeft, lineTop(3)),
                      hasOutage ? OUTAGE_COLOR : IM_COL32(176, 198, 176, 255), power, nullptr, 0.0f, &clip);

    const float dividerY = firstLine + headerHeight + statGap;
    drawList->AddLine(ImVec2(textLeft, dividerY), ImVec2(rowEnd.x - padding, dividerY), IM_COL32(128, 146, 130, 200),
                      card.mBorder);
    DrawStatChips(statChips, ImVec2(textLeft, dividerY + statGap), statWidth, clip);

    const std::string statTooltip = BuildPlayerStatTooltip(player);
    ImGui::SetItemTooltip("%s\n%s\n%s\n%s%s%s", playerName, identity, economy, power,
                          statTooltip.empty() ? "" : "\n", statTooltip.c_str());
    ImGui::PopID();
}
}

ImVec4 GetPlayerColor(std::uint32_t rgb)
{
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

const viewer::PlayerSummary* FindPlayer(const viewer::ReplayViewSnapshot& snapshot, int houseIndex)
{
    for (const auto& player : snapshot.mPlayers)
    {
        if (player.mHouseIndex == houseIndex)
        {
            return &player;
        }
    }
    return nullptr;
}

namespace
{
void TextWrappedFitV(const char* format, va_list arguments)
{
    // 换行位置为窗口局部坐标；内容区右缘左移 1 像素抵消 ImGui 折行宽度的向上取整。
    ImGui::PushTextWrapPos(std::max(1.0f, ImGui::GetContentRegionMax().x - 1.0f));
    ImGui::TextV(format, arguments);
    ImGui::PopTextWrapPos();
}
}

void TextWrappedFit(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    TextWrappedFitV(format, arguments);
    va_end(arguments);
}

void TextWrappedDisabled(const char* format, ...)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    va_list arguments;
    va_start(arguments, format);
    TextWrappedFitV(format, arguments);
    va_end(arguments);
    ImGui::PopStyleColor();
}

void DrawOverview(const viewer::ReplayViewSnapshot& snapshot, int& selectedHouseIndex)
{
    if (!snapshot.mActive)
    {
        TextWrappedDisabled("当前没有活动回放");
    }
    if (!snapshot.mReplayName.empty())
    {
        TextWrappedFit("%s", snapshot.mReplayName.c_str());
    }
    if (!snapshot.mMapName.empty())
    {
        TextWrappedDisabled("地图：%s", snapshot.mMapName.c_str());
    }
    ImGui::Separator();
    DrawSectionTitle("玩家与经济");
    if (snapshot.mPlayers.empty())
    {
        TextWrappedDisabled("暂无玩家数据");
    }
    {
        CardListSpacing spacing;
        for (const auto& player : snapshot.mPlayers)
        {
            DrawPlayerRow(player, selectedHouseIndex == player.mHouseIndex, selectedHouseIndex);
        }
    }
    ImGui::Separator();
    if (!snapshot.mHasPlayerVision)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.71f, 0.40f, 1.0f));
        TextWrappedFit("仅切换信息 · 地图视野未切换");
        ImGui::PopStyleColor();
    }
    else
    {
        TextWrappedDisabled("地图视野由宿主管理");
    }
}

}
