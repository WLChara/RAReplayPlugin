#include "ReplayHudParts.h"

#include "ReplayHudEconomyChart.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace rareplay::ui::detail
{
namespace
{
constexpr ImVec4 OPEN_BUTTON_COLOR(0.42f, 0.36f, 0.19f, 1.0f);
constexpr ImVec4 OPEN_BUTTON_TEXT_COLOR(0.98f, 0.92f, 0.74f, 1.0f);

// 垂直方向：图表高度随可用高度自适应，下限保证可读（约六行字高），上限随宽度（避免在宽高比极端的区域里被拉得过长）。
float ChoosePlotHeight(float availableHeight, float width, float reservedHeight, float xAxisHeight, float spacing)
{
    const float lineHeight = ImGui::GetTextLineHeight();
    const float minimum = lineHeight * 6.0f;
    const float maximum = std::max(minimum, width * 1.1f);
    return std::clamp(availableHeight - xAxisHeight - reservedHeight - spacing * 3.0f, minimum, maximum);
}

// 小图图例每行一张卡片：色块、名称（过长时裁剪）、右对齐的当前资金；点击选择观看信息的玩家。
// 被筛选隐藏的玩家变暗，色块只留边框，提示里说明曲线已隐藏。
void DrawLegend(const std::vector<ChartSeries>& series, int& selectedHouseIndex)
{
    const CardMetrics card = GetCardMetrics();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float chip = std::max(6.0f, ImGui::GetFontSize() * 0.62f);
    CardListSpacing spacing;
    for (const ChartSeries& entry : series)
    {
        const viewer::PlayerSummary& player = *entry.mPlayer;
        ImGui::PushID(player.mHouseIndex);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const float height = lineHeight + card.mPadding * 2.0f;
        if (ImGui::InvisibleButton("图例", ImVec2(width, height)))
            selectedHouseIndex = player.mHouseIndex;
        const bool isSelected = player.mHouseIndex == selectedHouseIndex;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 end(start.x + width, start.y + height);
        DrawCardBackground(drawList, start, end, GetCardState(isSelected));

        const auto credits = GetCurrentCredits(entry);
        char value[32];
        if (credits)
            std::snprintf(value, sizeof(value), "%lld", static_cast<long long>(*credits));
        else
            std::snprintf(value, sizeof(value), "—");
        const float valueWidth = ImGui::CalcTextSize(value).x;
        const float top = start.y + card.mPadding;
        const float chipTop = top + (lineHeight - chip) * 0.5f;
        const ImU32 color = GetChartColor(player.mColorRgb);
        const float alpha = entry.mIsVisible ? 1.0f : 0.45f;
        if (entry.mIsVisible)
        {
            drawList->AddRectFilled(ImVec2(start.x + card.mPadding, chipTop),
                                    ImVec2(start.x + card.mPadding + chip, chipTop + chip), color, 2.0f);
        }
        else
        {
            drawList->AddRect(ImVec2(start.x + card.mPadding, chipTop),
                              ImVec2(start.x + card.mPadding + chip, chipTop + chip), color, 2.0f);
        }
        const float nameLeft = start.x + card.mPadding + chip + card.mPadding;
        const float valueLeft = std::max(nameLeft, end.x - card.mPadding - valueWidth);
        const char* name = GetPlayerDisplayName(player);
        const ImVec4 nameClip(nameLeft, start.y, std::max(nameLeft, valueLeft - card.mPadding), end.y);
        const ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.94f, 0.92f, 0.85f, alpha));
        const ImU32 valueColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.96f, 0.87f, 0.64f, alpha));
        drawList->AddText(nullptr, 0.0f, ImVec2(nameLeft, top), nameColor, name, nullptr, 0.0f, &nameClip);
        const ImVec4 valueClip(valueLeft, start.y, end.x - card.mPadding * 0.5f, end.y);
        drawList->AddText(nullptr, 0.0f, ImVec2(valueLeft, top), valueColor, value, nullptr, 0.0f, &valueClip);
        ImGui::SetItemTooltip("%s\n资金 %s%s", name, value, entry.mIsVisible ? "" : "\n（曲线已隐藏）");
        ImGui::PopID();
    }
}

// 大窗口开关：打开时按钮高亮并写“收起大图”，再点一次关闭。
void DrawWindowToggle(EconomyWindowState& window)
{
    const bool isOpen = window.mIsOpen;
    if (isOpen)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, OPEN_BUTTON_COLOR);
        ImGui::PushStyleColor(ImGuiCol_Text, OPEN_BUTTON_TEXT_COLOR);
    }
    if (ImGui::Button(isOpen ? "收起大图" : "展开大图", ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), 0.0f)))
    {
        window.mIsOpen = !isOpen;
        window.mIsDragging = false;
    }
    if (isOpen)
        ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip(isOpen ? "关闭资金曲线大窗口" : "在地图区打开可拖动的资金曲线大窗口");
}
}

void DrawEconomyChart(const viewer::ReplayViewSnapshot& snapshot, int& selectedHouseIndex, EconomyUiState& state)
{
    DrawSectionTitle("资金曲线");
    DrawWindowToggle(state.mWindow);
    const std::vector<ChartSeries> series = CollectChartSeries(snapshot, state.mFilter);
    if (series.empty())
    {
        TextWrappedDisabled("暂无资金曲线数据");
        return;
    }
    state.mFilter.Normalize(GetChartHouseIndices(series));
    // 筛选可能刚被 Normalize 恢复，重新取一次可见性。
    const std::vector<ChartSeries> shown = CollectChartSeries(snapshot, state.mFilter);
    DrawEconomyFilterHint(shown, state.mFilter);

    const ImGuiStyle& style = ImGui::GetStyle();
    const CardMetrics card = GetCardMetrics();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float legendHeight = static_cast<float>(shown.size()) * (lineHeight + card.mPadding * 2.0f + card.mGap);
    // 横轴说明在窄面板里通常占两行。
    const float captionHeight = lineHeight * 2.0f + style.ItemSpacing.y;
    const float xAxisHeight = lineHeight + 2.0f;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);

    ChartParams params;
    params.mId = "资金曲线图";
    params.mRange = EconomyTimeRange::All;
    params.mSelectedHouseIndex = selectedHouseIndex;
    params.mWidth = width;
    const float plotHeight = ChoosePlotHeight(ImGui::GetContentRegionAvail().y, width, legendHeight + captionHeight,
                                              xAxisHeight, style.ItemSpacing.y);
    params.mHeight = plotHeight + xAxisHeight;
    const float unit = GetHudUnit();
    params.mLineThickness = 2.0f * unit;
    params.mSelectedThickness = 3.0f * unit;
    params.mIsNominalTime = snapshot.mGameTime && snapshot.mGameTime->mIsNominal;
    DrawEconomyPlot(shown, params);
    DrawEconomyAxisCaption(params.mIsNominalTime);
    DrawLegend(shown, selectedHouseIndex);
}
}
