#include "ReplayHud.h"

#include "ReplayHudParts.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace rareplay::ui
{
namespace
{
constexpr ImVec4 NOTICE_COLOR(0.96f, 0.71f, 0.35f, 1.0f);
constexpr char HUD_SCALE_POPUP[] = "界面缩放##HudScalePopup";

struct PanelTab
{
    ReplayPanel mPanel;
    const char* mLabel;
};

constexpr PanelTab PANEL_TABS[] = {
    {ReplayPanel::Overview, "总览"},
    {ReplayPanel::Production, "生产"},
    {ReplayPanel::Units, "部队"},
    {ReplayPanel::Groups, "编队"},
    {ReplayPanel::Economy, "经济"}};

void RecordWindowRegion(std::vector<UiRect>& regions)
{
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    regions.push_back({position.x, position.y, position.x + size.x, position.y + size.y});
}

// 标签按钮按可用宽度排成网格并均分行宽，窄面板自动折成两行，不截断也不溢出。
void DrawPanelTabs(ReplayPanel& selectedPanel)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = std::max(2.0f, std::floor(style.ItemSpacing.x * 0.5f));
    const float available = ImGui::GetContentRegionAvail().x;
    float minimumWidth = 0.0f;
    for (const PanelTab& tab : PANEL_TABS)
        minimumWidth = std::max(minimumWidth, ImGui::CalcTextSize(tab.mLabel).x + style.FramePadding.x * 2.0f + 2.0f);
    constexpr int TAB_COUNT = static_cast<int>(std::size(PANEL_TABS));
    int columns = std::clamp(static_cast<int>((available + spacing) / (minimumWidth + spacing)), 1, TAB_COUNT);
    const int rows = (TAB_COUNT + columns - 1) / columns;
    columns = (TAB_COUNT + rows - 1) / rows;
    const float buttonWidth = std::max(1.0f, (available - spacing * static_cast<float>(columns - 1)) /
                                                 static_cast<float>(columns));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    for (int index = 0; index < TAB_COUNT; ++index)
    {
        const PanelTab& tab = PANEL_TABS[index];
        if (index % columns != 0)
            ImGui::SameLine();
        const bool isSelected = selectedPanel == tab.mPanel;
        ImGui::PushStyleColor(ImGuiCol_Button, isSelected ? ImVec4(0.42f, 0.36f, 0.19f, 1.0f)
                                                          : ImVec4(0.18f, 0.21f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, isSelected ? ImVec4(0.98f, 0.92f, 0.74f, 1.0f)
                                                        : ImVec4(0.76f, 0.80f, 0.72f, 1.0f));
        if (ImGui::Button(tab.mLabel, ImVec2(buttonWidth, 0.0f)))
            selectedPanel = tab.mPanel;
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();
}

// 齿轮图形按钮，不依赖字体中的符号字形；尺寸为一个控件行高的正方形。
bool DrawGearButton(const char* id, float size)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool isClicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const ImGuiCol background = ImGui::IsItemActive() ? ImGuiCol_ButtonActive :
        ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(start, ImVec2(start.x + size, start.y + size), ImGui::GetColorU32(background),
                            ImGui::GetStyle().FrameRounding);
    const ImVec2 center(start.x + size * 0.5f, start.y + size * 0.5f);
    const float radius = size * 0.30f;
    const ImU32 color = IM_COL32(226, 214, 176, 255);
    constexpr int TEETH = 8;
    for (int tooth = 0; tooth < TEETH; ++tooth)
    {
        const float angle = static_cast<float>(tooth) * 6.2831853f / TEETH;
        const ImVec2 direction(std::cos(angle), std::sin(angle));
        drawList->AddLine(ImVec2(center.x + direction.x * radius * 0.6f, center.y + direction.y * radius * 0.6f),
                          ImVec2(center.x + direction.x * radius * 1.35f, center.y + direction.y * radius * 1.35f),
                          color, std::max(1.5f, size * 0.13f));
    }
    drawList->AddCircleFilled(center, radius, color);
    drawList->AddCircleFilled(center, radius * 0.45f, ImGui::GetColorU32(background));
    return isClicked;
}

// 观看信息所用的玩家；只改变本界面，不改变地图视野或游戏控制权。右侧齿轮打开界面缩放。
void DrawPlayerSelector(const viewer::ReplayViewSnapshot& snapshot, int& selectedHouseIndex, int scalePercent,
                        viewer::ReplayViewExchange& exchange, std::vector<UiRect>& inputRegions)
{
    const viewer::PlayerSummary* selected = detail::FindPlayer(snapshot, selectedHouseIndex);
    const float lineHeight = ImGui::GetFrameHeight();
    const float chipSize = std::max(6.0f, ImGui::GetFontSize() * 0.62f);
    const ImVec2 chipStart = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(chipSize, lineHeight));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float chipTop = chipStart.y + (lineHeight - chipSize) * 0.5f;
    drawList->AddRectFilled(ImVec2(chipStart.x, chipTop), ImVec2(chipStart.x + chipSize, chipTop + chipSize),
                            selected ? ImGui::ColorConvertFloat4ToU32(detail::GetPlayerColor(selected->mColorRgb))
                                     : IM_COL32(90, 96, 88, 255), 2.0f);
    ImGui::SameLine();
    const float gearSize = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - gearSize - ImGui::GetStyle().ItemSpacing.x));
    const char* preview = selected == nullptr ? "未选择玩家" :
        selected->mName.empty() ? "未命名玩家" : selected->mName.c_str();
    if (ImGui::BeginCombo("##观看玩家", preview, ImGuiComboFlags_HeightLarge))
    {
        if (snapshot.mPlayers.empty())
            ImGui::TextDisabled("暂无玩家数据");
        for (const auto& player : snapshot.mPlayers)
        {
            ImGui::PushID(player.mHouseIndex);
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##玩家", player.mHouseIndex == selectedHouseIndex))
                selectedHouseIndex = player.mHouseIndex;
            const float rowHeight = ImGui::GetItemRectSize().y;
            const float rowRight = ImGui::GetItemRectMax().x;
            ImDrawList* popupDrawList = ImGui::GetWindowDrawList();
            const float itemChipTop = rowStart.y + (rowHeight - chipSize) * 0.5f;
            popupDrawList->AddRectFilled(ImVec2(rowStart.x, itemChipTop),
                ImVec2(rowStart.x + chipSize, itemChipTop + chipSize),
                ImGui::ColorConvertFloat4ToU32(detail::GetPlayerColor(player.mColorRgb)), 2.0f);
            const char* name = player.mName.empty() ? "未命名玩家" : player.mName.c_str();
            const ImVec4 clip(rowStart.x, rowStart.y, rowRight, rowStart.y + rowHeight);
            popupDrawList->AddText(nullptr, 0.0f, ImVec2(rowStart.x + chipSize + 5.0f, rowStart.y),
                                   ImGui::GetColorU32(player.mDefeated ? ImGuiCol_TextDisabled : ImGuiCol_Text),
                                   name, nullptr, 0.0f, &clip);
            ImGui::PopID();
        }
        RecordWindowRegion(inputRegions);
        ImGui::EndCombo();
    }
    if (selected != nullptr)
        ImGui::SetItemTooltip("观看信息：%s（不切换地图视野）", preview);

    ImGui::SameLine();
    if (DrawGearButton("界面缩放按钮", gearSize))
        ImGui::OpenPopup(HUD_SCALE_POPUP);
    char current[48];
    detail::FormatHudScaleLabel(snapshot.mHudScaleSetting, scalePercent, current, sizeof(current));
    ImGui::SetItemTooltip("界面缩放：%s", current);
    if (ImGui::BeginPopup(HUD_SCALE_POPUP))
    {
        ImGui::TextUnformatted("界面缩放");
        ImGui::Separator();
        detail::DrawHudScaleOptions(snapshot, scalePercent, exchange);
        RecordWindowRegion(inputRegions);
        ImGui::EndPopup();
    }
}
}

void ReplayHud::DrawSidebarPanel(const viewer::ReplayViewSnapshot& snapshot, const UiRect& rect,
                                 viewer::ReplayViewExchange& exchange)
{
    const float width = rect.mRight - rect.mLeft;
    const float height = rect.mBottom - rect.mTop;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos(ImVec2(rect.mLeft, rect.mTop));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    // 原生侧栏已停止绘制，下面是旧像素：背景必须完全不透明且铺满到角，不能留圆角或半透明。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.090f, 0.106f, 0.098f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.33f, 0.26f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // 缩放后的样式尺寸取整：ImGui 会把窗口与工作区下取整，小数内边距会让控件越出半个像素。
    const auto pixels = [this](float value) { return std::round(value * mScale); };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, std::floor(mScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pixels(6.0f), pixels(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(pixels(5.0f), pixels(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pixels(4.0f), pixels(3.0f)));
    if (ImGui::Begin("回放面板##ReplaySidebarPanel", nullptr, flags))
    {
        // 字号由宿主按缩放重建的字体决定，面板不再自行缩小字号；窄处靠换行、单列与截断加提示。
        RecordWindowRegion(mInputRegions);
        if (snapshot.mIsPreview)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
            detail::TextWrappedFit("布局预览 · 示例数据");
            ImGui::PopStyleColor();
        }
        if (snapshot.mIsNativeRadarAvailable && !*snapshot.mIsNativeRadarAvailable)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
            detail::TextWrappedFit("录制方当前无雷达");
            ImGui::PopStyleColor();
        }
        detail::DrawGameTimeHeader(snapshot);
        DrawPlayerSelector(snapshot, mSelectedHouseIndex, mScalePercent, exchange, mInputRegions);
        ReplayPanel shownPanel = mSelectedPanel;
        DrawPanelTabs(shownPanel);
        if (shownPanel != mSelectedPanel)
            mSelectedPanel = shownPanel;
        mSelectPanelPending = false;
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::BeginChild("面板内容", ImVec2(0.0f, 0.0f), false))
        {
            const viewer::PlayerSummary* selectedPlayer = detail::FindPlayer(snapshot, mSelectedHouseIndex);
            switch (mSelectedPanel)
            {
            case ReplayPanel::Overview:
                detail::DrawOverview(snapshot, mSelectedHouseIndex);
                ImGui::Separator();
                detail::DrawHudScaleCombo(snapshot, mScalePercent, exchange, mInputRegions);
                break;
            case ReplayPanel::Production:
                detail::DrawProductionLines(snapshot, selectedPlayer, mIconResolver);
                break;
            case ReplayPanel::Units:
                detail::DrawUnitInventory(selectedPlayer, mIconResolver, mSelectedUnitCategory);
                break;
            case ReplayPanel::Groups: detail::DrawRecordedGroups(snapshot); break;
            case ReplayPanel::Economy: detail::DrawEconomyChart(snapshot, mSelectedHouseIndex, mEconomy); break;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(2);
}
}
