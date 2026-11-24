#include "ReplayHudParts.h"

#include "ReplayHudLayout.h"
#include "ReplayHudScale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace rareplay::ui::detail
{
namespace
{
constexpr viewer::UnitCategory CATEGORIES[] = {
    viewer::UnitCategory::Infantry, viewer::UnitCategory::Vehicle, viewer::UnitCategory::Aircraft,
    viewer::UnitCategory::Building, viewer::UnitCategory::Other};
// 原生 cameo 为 60×48；卡片图标框按此比例，图像在框内保持自身比例。
constexpr float CAMEO_WIDTH = 60.0f;
constexpr float CAMEO_ASPECT = 48.0f / 60.0f;

const char* GetCategoryLabel(viewer::UnitCategory category)
{
    switch (category)
    {
    case viewer::UnitCategory::Infantry: return "步兵";
    case viewer::UnitCategory::Vehicle: return "载具";
    case viewer::UnitCategory::Aircraft: return "飞机";
    case viewer::UnitCategory::Building: return "建筑";
    case viewer::UnitCategory::Other: return "其他";
    }
    return "其他";
}

std::uint32_t CountUnits(const viewer::PlayerSummary& player, std::optional<viewer::UnitCategory> category)
{
    std::uint32_t total = 0;
    for (const auto& unit : player.mUnits)
    {
        if (!category || unit.mCategory == *category)
            total += unit.mCount;
    }
    return total;
}

bool HasUnits(const viewer::PlayerSummary& player, viewer::UnitCategory category)
{
    return std::any_of(player.mUnits.begin(), player.mUnits.end(),
                       [category](const viewer::UnitSummary& unit) { return unit.mCategory == category; });
}

// 子页签按标签自身宽度从左到右排布，放不下就换行；没有单位的类别置灰（仍保留位置，便于定位），
// “其他”只在确有单位或正被选中时出现。选中状态只存于本次会话的 UI。
void DrawCategoryTabs(const viewer::PlayerSummary& player, std::optional<viewer::UnitCategory>& selectedCategory)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = std::max(2.0f, std::floor(style.ItemSpacing.x * 0.5f));
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float lineUsed = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    const auto drawTab = [&](std::optional<viewer::UnitCategory> category, const char* name)
    {
        const std::uint32_t count = CountUnits(player, category);
        const bool isSelected = selectedCategory == category;
        char label[48];
        std::snprintf(label, sizeof(label), "%s %u", name, count);
        const float width = std::min(available, ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f);
        if (lineUsed > 0.0f && lineUsed + spacing + width <= available)
        {
            ImGui::SameLine();
            lineUsed += spacing + width;
        }
        else
        {
            lineUsed = width;
        }
        ImGui::PushID(category ? static_cast<int>(*category) : -1);
        ImGui::PushStyleColor(ImGuiCol_Button, isSelected ? ImVec4(0.42f, 0.36f, 0.19f, 1.0f)
                                                          : ImVec4(0.18f, 0.21f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, isSelected ? ImVec4(0.98f, 0.92f, 0.74f, 1.0f)
                                                        : ImVec4(0.76f, 0.80f, 0.72f, 1.0f));
        ImGui::BeginDisabled(count == 0 && !isSelected);
        if (ImGui::Button(label, ImVec2(width, 0.0f)))
            selectedCategory = category;
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    };
    drawTab(std::nullopt, "全部");
    for (const viewer::UnitCategory category : CATEGORIES)
    {
        if (category == viewer::UnitCategory::Other && !HasUnits(player, category) && selectedCategory != category)
            continue;
        drawTab(category, GetCategoryLabel(category));
    }
    ImGui::PopStyleVar();
}

// 卡片宽度由列宽决定：名称一行（过长截断，悬停显示全名）、图标框为 cameo 比例且不超过原始像素乘以 HUD 缩放，
// 数量角标贴图标右下角。同一行两张卡片高度一致。
void DrawUnitTile(const viewer::UnitSummary& unit, const ReplayHud::IconResolver& iconResolver, float tileWidth)
{
    ImGui::PushID(unit.mTypeId.c_str());
    ImGui::BeginGroup();
    const float fontSize = ImGui::GetFontSize();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float padding = GetCardMetrics().mPadding;
    const float iconWidth = std::floor(std::max(1.0f, std::min(tileWidth - padding * 2.0f,
                                                               CAMEO_WIDTH * fontSize / HUD_BASE_FONT_PIXELS)));
    const float iconHeight = std::floor(iconWidth * CAMEO_ASPECT);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 tileSize(tileWidth, padding * 2.5f + lineHeight + iconHeight);
    ImGui::InvisibleButton("单位卡", tileSize);
    const CardState cardState = GetCardState(false);
    const char* name = unit.mName.empty() ? unit.mTypeId.c_str() : unit.mName.c_str();
    ImGui::SetItemTooltip("%s × %u", name, unit.mCount);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 tileEnd(start.x + tileWidth, start.y + tileSize.y);
    DrawCardBackground(drawList, start, tileEnd, cardState);
    const ImVec4 labelClip(start.x + padding, start.y, tileEnd.x - padding, start.y + padding + lineHeight);
    drawList->AddText(nullptr, 0.0f, ImVec2(start.x + padding, start.y + padding),
                      IM_COL32(236, 235, 219, 255), name, nullptr, 0.0f, &labelClip);
    const ImVec2 iconPosition(std::floor(start.x + (tileWidth - iconWidth) * 0.5f),
                              start.y + padding * 1.5f + lineHeight);
    ImGui::SetCursorScreenPos(iconPosition);
    const UiRect iconRect = DrawIcon(unit.mIcon, iconResolver, ImVec2(iconWidth, iconHeight));
    char count[32];
    std::snprintf(count, sizeof(count), "%u", unit.mCount);
    const ImVec2 textSize = ImGui::CalcTextSize(count);
    // 角标贴在图标右下角，并夹在卡片内，窄卡片中也不越界。
    const float badgeRight = std::min(tileEnd.x - 1.0f, iconRect.mRight);
    const float badgeBottom = std::min(tileEnd.y - 1.0f, iconRect.mBottom);
    const ImVec2 badgeMin(std::max(start.x + 1.0f, badgeRight - textSize.x - 6.0f), badgeBottom - textSize.y - 2.0f);
    drawList->AddRectFilled(badgeMin, ImVec2(badgeRight, badgeBottom), IM_COL32(14, 21, 18, 245), 3.0f);
    drawList->AddText(ImVec2(badgeMin.x + 3.0f, badgeMin.y + 1.0f), IM_COL32(246, 222, 164, 255), count);
    ImGui::SetCursorScreenPos(ImVec2(start.x, tileEnd.y));
    ImGui::EndGroup();
    ImGui::PopID();
}

// 每行最多两张卡片；列数规则见 GetUnitCardColumns。
void DrawUnitGrid(const viewer::PlayerSummary& player, viewer::UnitCategory category,
                  const ReplayHud::IconResolver& iconResolver)
{
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = GetUnitCardColumns(availableWidth, ImGui::GetFontSize(), spacing);
    const float tileWidth = std::floor((availableWidth - spacing * static_cast<float>(columns - 1)) /
                                       static_cast<float>(columns));
    int column = 0;
    for (const auto& unit : player.mUnits)
    {
        if (unit.mCategory != category)
            continue;
        if (column != 0)
            ImGui::SameLine();
        DrawUnitTile(unit, iconResolver, tileWidth);
        column = (column + 1) % columns;
    }
}
}

UiRect DrawIcon(const std::shared_ptr<const viewer::IconImage>& icon,
              const ReplayHud::IconResolver& iconResolver, const ImVec2& size)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImTextureID texture = nullptr;
    if (icon && iconResolver)
    {
        texture = iconResolver(icon);
    }
    if (texture != nullptr)
    {
        // 保持图像自身比例，在框内居中。
        const float scale = std::min(size.x / icon->mWidth, size.y / icon->mHeight);
        const ImVec2 imageSize(icon->mWidth * scale, icon->mHeight * scale);
        const ImVec2 imageMin(start.x + (size.x - imageSize.x) * 0.5f,
                              start.y + (size.y - imageSize.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(texture, imageMin,
            ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y));
        return {imageMin.x, imageMin.y, imageMin.x + imageSize.x, imageMin.y + imageSize.y};
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
                            IM_COL32(75, 81, 72, 255), 3.0f);
    drawList->AddRect(start, ImVec2(start.x + size.x, start.y + size.y),
                      IM_COL32(117, 123, 108, 255), 3.0f);
    const char* placeholder = "?";
    const ImVec2 textSize = ImGui::CalcTextSize(placeholder);
    drawList->AddText(ImVec2(start.x + (size.x - textSize.x) * 0.5f,
                             start.y + (size.y - textSize.y) * 0.5f),
                      IM_COL32(195, 199, 187, 255), placeholder);
    ImGui::SetItemTooltip("图标缺失或尚未载入");
    return {start.x, start.y, start.x + size.x, start.y + size.y};
}

void DrawUnitInventory(const viewer::PlayerSummary* player, const ReplayHud::IconResolver& iconResolver,
                       std::optional<viewer::UnitCategory>& selectedCategory)
{
    if (player == nullptr)
    {
        TextWrappedDisabled("未选择玩家");
        return;
    }
    if (!player->mHasUnitData)
    {
        TextWrappedDisabled("该玩家暂无单位数据");
        return;
    }

    DrawCategoryTabs(*player, selectedCategory);
    ImGui::Separator();
    // 子页签固定在上方，只有卡片列表滚动。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (ImGui::BeginChild("单位列表", ImVec2(0.0f, 0.0f), false))
    {
        // 卡片之间（水平与垂直）留出统一间距。
        CardListSpacing spacing;
        if (selectedCategory)
        {
            if (HasUnits(*player, *selectedCategory))
                DrawUnitGrid(*player, *selectedCategory, iconResolver);
            else
                TextWrappedDisabled("该类当前没有单位");
        }
        else
        {
            for (const viewer::UnitCategory category : CATEGORIES)
            {
                if (!HasUnits(*player, category))
                    continue;
                DrawSectionTitle("%s %u", GetCategoryLabel(category), CountUnits(*player, category));
                DrawUnitGrid(*player, category, iconResolver);
                ImGui::Dummy(ImVec2(0.0f, std::round(ImGui::GetFontSize() * 0.25f)));
            }
            if (player->mUnits.empty())
                TextWrappedDisabled("当前没有单位");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
}
