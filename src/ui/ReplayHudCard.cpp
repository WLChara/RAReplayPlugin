#include "ReplayHudParts.h"

#include "ReplayHudScale.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>

namespace rareplay::ui::detail
{
namespace
{
// 面板背景为 (23, 27, 25)；卡片明显更亮（明度 L* 约 10 → 27），再加一圈浅色 1 像素边框，
// 选中卡片换成琥珀色填充、加粗的琥珀边框，悬停时整体提亮一档。
constexpr ImU32 CARD_FILL = IM_COL32(54, 65, 57, 255);
constexpr ImU32 CARD_FILL_HOVER = IM_COL32(64, 77, 68, 255);
constexpr ImU32 CARD_FILL_SELECTED = IM_COL32(96, 81, 44, 255);
constexpr ImU32 CARD_BORDER = IM_COL32(118, 136, 120, 255);
constexpr ImU32 CARD_BORDER_HOVER = IM_COL32(160, 180, 162, 255);
constexpr ImU32 CARD_BORDER_SELECTED = IM_COL32(240, 192, 84, 255);
constexpr ImU32 SECTION_LINE_COLOR = IM_COL32(112, 130, 114, 255);
constexpr ImVec4 SECTION_TITLE_COLOR(0.86f, 0.79f, 0.55f, 1.0f);
}

float GetHudUnit()
{
    return ImGui::GetFontSize() / HUD_BASE_FONT_PIXELS;
}

CardMetrics GetCardMetrics()
{
    const float unit = GetHudUnit();
    CardMetrics metrics;
    metrics.mGap = std::max(4.0f, std::round(6.0f * unit));
    metrics.mPadding = std::max(4.0f, std::round(5.0f * unit));
    metrics.mBorder = std::max(1.0f, std::floor(unit));
    metrics.mRounding = std::round(3.0f * unit);
    return metrics;
}

CardState GetCardState(bool isSelected)
{
    if (isSelected)
        return CardState::Selected;
    return ImGui::IsItemHovered() ? CardState::Hovered : CardState::Normal;
}

CardColors GetCardColors(CardState state)
{
    switch (state)
    {
    case CardState::Hovered: return {CARD_FILL_HOVER, CARD_BORDER_HOVER};
    case CardState::Selected: return {CARD_FILL_SELECTED, CARD_BORDER_SELECTED};
    case CardState::Normal: break;
    }
    return {CARD_FILL, CARD_BORDER};
}

void DrawCardBackground(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, CardState state)
{
    const CardMetrics metrics = GetCardMetrics();
    const CardColors colors = GetCardColors(state);
    const float thickness = state == CardState::Selected ? metrics.mBorder * 2.0f : metrics.mBorder;
    drawList->AddRectFilled(min, max, colors.mFill, metrics.mRounding);
    // AddRect 的描边以路径为中心；粗边框向内收半个多出的宽度，使整圈描边落在卡片矩形内。
    const float inset = (thickness - 1.0f) * 0.5f;
    drawList->AddRect(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset), colors.mBorder,
                      metrics.mRounding, ImDrawFlags_None, thickness);
}

CardListSpacing::CardListSpacing()
{
    const float gap = GetCardMetrics().mGap;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
}

CardListSpacing::~CardListSpacing()
{
    ImGui::PopStyleVar();
}

CardScope::CardScope(bool isSelected) : mIsSelected(isSelected)
{
    const CardMetrics metrics = GetCardMetrics();
    mPadding = metrics.mPadding;
    mStart = ImGui::GetCursorScreenPos();
    mWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    mDrawList = ImGui::GetWindowDrawList();
    // 背景要在内容之后才知道高度：内容画在上层通道，背景补画在下层通道。
    mDrawList->ChannelsSplit(2);
    mDrawList->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(mStart.x + mPadding, mStart.y + mPadding));
    ImGui::BeginGroup();
    // 换行位置是窗口局部坐标；内容区右缘留出内边距。
    const float rightEdge = mStart.x + mWidth - mPadding - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    ImGui::PushTextWrapPos(std::max(1.0f, rightEdge));
}

float CardScope::GetInnerWidth() const noexcept
{
    return std::max(1.0f, mWidth - mPadding * 2.0f);
}

CardScope::~CardScope()
{
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float bottom = ImGui::GetItemRectMax().y + mPadding;
    const ImVec2 end(mStart.x + mWidth, std::max(bottom, mStart.y + mPadding * 2.0f));
    mDrawList->ChannelsSetCurrent(0);
    DrawCardBackground(mDrawList, mStart, end, mIsSelected ? CardState::Selected : CardState::Normal);
    mDrawList->ChannelsMerge();
    // 整块卡片登记为一个占位项，布局据此前进（含 ItemSpacing，即卡片间距）。
    ImGui::SetCursorScreenPos(mStart);
    ImGui::Dummy(ImVec2(mWidth, end.y - mStart.y));
}

void DrawSectionTitle(const char* format, ...)
{
    const CardMetrics metrics = GetCardMetrics();
    ImGui::PushStyleColor(ImGuiCol_Text, SECTION_TITLE_COLOR);
    va_list arguments;
    va_start(arguments, format);
    ImGui::TextV(format, arguments);
    va_end(arguments);
    ImGui::PopStyleColor();
    // 标题下方一条与内容区等宽的分隔线，再留一小段空隙。
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float lineY = start.y + std::round(1.0f * GetHudUnit());
    ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x, lineY), ImVec2(start.x + width, lineY), SECTION_LINE_COLOR,
                                        metrics.mBorder);
    ImGui::Dummy(ImVec2(width, std::round(2.0f * GetHudUnit()) + metrics.mBorder));
}
}
