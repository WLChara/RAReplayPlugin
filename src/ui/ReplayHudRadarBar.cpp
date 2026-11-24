#include "ReplayHudParts.h"

#include "ReplayHudEconomyChart.h"
#include "ReplayHudFormat.h"
#include "../viewer/ReplayViewExchange.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace rareplay::ui::detail
{
namespace
{
constexpr ImU32 TITLE_FILL = IM_COL32(42, 52, 44, 255);
constexpr ImU32 TITLE_LINE = IM_COL32(104, 122, 108, 255);
constexpr ImVec4 TITLE_TEXT_COLOR(0.92f, 0.85f, 0.62f, 1.0f);
// 提示栏浮在地图上，背景略透明；窗口边框与其他浮动窗口一致。
constexpr ImVec4 WINDOW_FILL(0.090f, 0.106f, 0.098f, 0.90f);
constexpr ImVec4 WINDOW_BORDER(0.46f, 0.53f, 0.47f, 1.0f);
constexpr ImVec4 ACTIVE_BUTTON_COLOR(0.42f, 0.36f, 0.19f, 1.0f);
constexpr ImU32 TIME_COLOR = IM_COL32(176, 196, 176, 255);
constexpr ImU32 TEXT_COLOR = IM_COL32(236, 232, 214, 255);
constexpr ImU32 HINT_COLOR = IM_COL32(158, 166, 154, 255);
constexpr ImU32 ICON_FRAME_FILL = IM_COL32(22, 30, 27, 255);
constexpr ImU32 ICON_FRAME_LINE = IM_COL32(118, 136, 120, 255);
constexpr ImU32 GLYPH_COLOR = IM_COL32(224, 213, 178, 255);
// 一条事件的文字最多两行，放不下的部分被裁掉，完整内容见悬停提示。
constexpr int MAX_TEXT_LINES = 2;
constexpr char FOLD_BUTTON_ID[] = "##折叠雷达事件";
constexpr char HISTORY_BUTTON_ID[] = "##雷达事件历史";

struct BarContext
{
    RadarBarMetrics mMetrics;
    float mUnit = 1.0f;
    float mCardGap = 2.0f;
};

// 文字分段在整串文字中的字节范围，以及它的着色方式。
struct SegmentRange
{
    std::size_t mBegin = 0;
    std::size_t mEnd = 0;
    ImU32 mColor = TEXT_COLOR;
};

// 与一行文字的绘制配套：按 wrapWidth 贪心折行，最多 maxLines 行，逐行按分段着色。
void DrawSegmentedText(ImDrawList* drawList, const ImVec2& position, float wrapWidth, int maxLines,
                       const std::string& text, const std::vector<SegmentRange>& segments, float opacity,
                       const ImVec4& clip)
{
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float fontScale = font->FontSize > 0.0f ? fontSize / font->FontSize : 1.0f;
    const char* begin = text.c_str();
    const char* end = begin + text.size();
    const char* line = begin;
    for (int lineIndex = 0; lineIndex < maxLines && line < end; ++lineIndex)
    {
        const char* lineEnd = font->CalcWordWrapPositionA(fontScale, line, end, wrapWidth);
        if (lineEnd <= line)
            lineEnd = std::min(end, line + 1);
        const float y = position.y + static_cast<float>(lineIndex) * fontSize;
        float x = position.x;
        for (const SegmentRange& segment : segments)
        {
            const char* from = std::max(begin + segment.mBegin, line);
            const char* to = std::min(begin + segment.mEnd, lineEnd);
            if (from >= to)
                continue;
            drawList->AddText(font, fontSize, ImVec2(x, y), WithOpacity(segment.mColor, opacity), from, to, 0.0f, &clip);
            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, from, to).x;
        }
        line = lineEnd;
        while (line < end && *line == ' ')
            ++line;
    }
}

// 间谍通用图标：礼帽加风衣轮廓。
void DrawSpyGlyph(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 color)
{
    const float width = max.x - min.x;
    const float height = max.y - min.y;
    const float centerX = (min.x + max.x) * 0.5f;
    const ImVec2 head(centerX, min.y + height * 0.36f);
    drawList->AddCircleFilled(head, height * 0.16f, color, 14);
    // 帽檐。
    drawList->AddLine(ImVec2(centerX - width * 0.17f, head.y - height * 0.14f),
                      ImVec2(centerX + width * 0.17f, head.y - height * 0.14f), color,
                      std::max(1.0f, height * 0.06f));
    const ImVec2 coat[4] = {ImVec2(centerX - width * 0.27f, max.y - height * 0.08f),
                            ImVec2(centerX - width * 0.14f, min.y + height * 0.58f),
                            ImVec2(centerX + width * 0.14f, min.y + height * 0.58f),
                            ImVec2(centerX + width * 0.27f, max.y - height * 0.08f)};
    drawList->AddConvexPolyFilled(coat, 4, color);
}

// 事件图标：超武图标按原始比例放进图标框，缺失时画通用标记；渗透事件画间谍图标。
void DrawEventIcon(ImDrawList* drawList, const viewer::RadarEvent& event, const ImVec2& min, const ImVec2& max,
                   float opacity, ImU32 actorColor, const ReplayHud::IconResolver& iconResolver)
{
    const float unit = GetHudUnit();
    const float rounding = std::max(2.0f, std::round(3.0f * unit));
    const float thickness = std::max(1.0f, std::round(1.5f * unit));
    ImTextureID texture = nullptr;
    if (event.mKind == viewer::RadarEventKind::SuperWeaponLaunched && event.mIcon && iconResolver)
        texture = iconResolver(event.mIcon);
    if (texture && event.mIcon->mWidth > 0 && event.mIcon->mHeight > 0)
    {
        const float boxWidth = max.x - min.x;
        const float boxHeight = max.y - min.y;
        const float fit = std::min(boxWidth / static_cast<float>(event.mIcon->mWidth),
                                   boxHeight / static_cast<float>(event.mIcon->mHeight));
        const float width = std::round(static_cast<float>(event.mIcon->mWidth) * fit);
        const float height = std::round(static_cast<float>(event.mIcon->mHeight) * fit);
        const ImVec2 imageMin(std::round(min.x + (boxWidth - width) * 0.5f), std::round(min.y + (boxHeight - height) * 0.5f));
        drawList->AddImage(texture, imageMin, ImVec2(imageMin.x + width, imageMin.y + height), ImVec2(0.0f, 0.0f),
                           ImVec2(1.0f, 1.0f), WithOpacity(IM_COL32_WHITE, opacity));
        drawList->AddRect(imageMin, ImVec2(imageMin.x + width, imageMin.y + height), WithOpacity(ICON_FRAME_LINE, opacity),
                          0.0f, ImDrawFlags_None, 1.0f);
        return;
    }
    drawList->AddRectFilled(min, max, WithOpacity(ICON_FRAME_FILL, opacity), rounding);
    drawList->AddRect(min, max, WithOpacity(actorColor, opacity), rounding, ImDrawFlags_None, thickness);
    if (event.mKind == viewer::RadarEventKind::SuperWeaponLaunched)
        DrawSuperWeaponGlyph(drawList, ImVec2(min.x + (max.x - min.x) * 0.15f, min.y + (max.y - min.y) * 0.1f),
                             ImVec2(max.x - (max.x - min.x) * 0.15f, max.y - (max.y - min.y) * 0.1f),
                             WithOpacity(GLYPH_COLOR, opacity), thickness);
    else
        DrawSpyGlyph(drawList, min, max, WithOpacity(GLYPH_COLOR, opacity));
}

ImU32 GetRoleColor(RadarTextRole role, const viewer::RadarEvent& event)
{
    switch (role)
    {
    case RadarTextRole::Actor: return GetChartColor(event.mActor.mColorRgb);
    case RadarTextRole::Victim: return GetChartColor(event.mVictim.mColorRgb);
    case RadarTextRole::Plain: break;
    }
    return TEXT_COLOR;
}

// 一条事件：整行是一个可点击的卡片（带位置的事件点击后镜头居中），左侧是玩家颜色条，其后依次是事件图标与
// “时间 + 文字”。min 为行左上角，width 为行宽；行高为 mRowHeight，卡片上下各留 mCardGap。
void DrawRow(const viewer::ReplayViewSnapshot& snapshot, const RadarRow& row, const ImVec2& min, float width,
             const BarContext& context, const ReplayHud::IconResolver& iconResolver,
             viewer::ReplayViewExchange& exchange)
{
    const RadarBarMetrics& m = context.mMetrics;
    const viewer::RadarEvent& event = *row.mEvent;
    ImGui::PushID(static_cast<int>(event.mId));
    ImGui::SetCursorScreenPos(min);
    const bool isClicked = ImGui::InvisibleButton("##事件", ImVec2(width, m.mRowHeight));
    const bool isHovered = ImGui::IsItemHovered();
    if (isClicked && event.mLocation)
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::CenterOnRadarEvent,
                         static_cast<int>(event.mId)});

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float opacity = row.mOpacity;
    const CardMetrics card = GetCardMetrics();
    const float inset = std::round(m.mPadding * 0.5f);
    const ImVec2 cardMin(min.x + inset, min.y + context.mCardGap);
    const ImVec2 cardMax(min.x + width - inset, min.y + m.mRowHeight - context.mCardGap);
    const CardColors colors = GetCardColors(isHovered && event.mLocation ? CardState::Hovered : CardState::Normal);
    drawList->AddRectFilled(cardMin, cardMax, WithOpacity(colors.mFill, opacity), card.mRounding);
    drawList->AddRect(cardMin, cardMax, WithOpacity(colors.mBorder, opacity), card.mRounding, ImDrawFlags_None,
                      card.mBorder);

    // 玩家颜色条：事件的主角（释放方、间谍所属方）。
    const ImU32 actorColor = GetChartColor(event.mActor.mColorRgb);
    drawList->AddRectFilled(cardMin, ImVec2(cardMin.x + m.mStripeWidth, cardMax.y), WithOpacity(actorColor, opacity),
                            card.mRounding, ImDrawFlags_RoundCornersLeft);

    const float centerY = (cardMin.y + cardMax.y) * 0.5f;
    const float iconLeft = cardMin.x + m.mStripeWidth + std::round(m.mPadding * 0.8f);
    const ImVec2 iconMin(iconLeft, std::round(centerY - m.mIconHeight * 0.5f));
    const ImVec2 iconMax(iconLeft + m.mIconWidth, iconMin.y + m.mIconHeight);
    DrawEventIcon(drawList, event, iconMin, iconMax, opacity, actorColor, iconResolver);

    // “时间 名称 …”整串折行；时间用较暗的颜色，玩家名称用玩家颜色。
    std::string text = FormatGameClock(event.mGameTimeMs) + " ";
    std::vector<SegmentRange> segments;
    segments.push_back({0, text.size(), TIME_COLOR});
    const std::vector<RadarTextSegment> parts = BuildRadarEventText(event);
    for (const RadarTextSegment& part : parts)
    {
        const std::size_t partBegin = text.size();
        text += part.mText;
        segments.push_back({partBegin, text.size(), GetRoleColor(part.mRole, event)});
    }
    const float textLeft = iconMax.x + std::round(m.mPadding * 0.8f);
    const float textRight = cardMax.x - std::round(m.mPadding * 0.6f);
    const float fontSize = ImGui::GetFontSize();
    const float wrapWidth = std::max(1.0f, textRight - textLeft);
    // 一行时垂直居中，两行时占满卡片高度。
    const float totalWidth = ImGui::CalcTextSize(text.c_str()).x;
    const int lineCount = totalWidth <= wrapWidth ? 1 : MAX_TEXT_LINES;
    const float textTop = std::round(centerY - fontSize * static_cast<float>(lineCount) * 0.5f);
    const ImVec4 clip(textLeft, cardMin.y, textRight, cardMax.y);
    DrawSegmentedText(drawList, ImVec2(textLeft, textTop), wrapWidth, MAX_TEXT_LINES, text, segments, opacity, clip);

    if (isHovered)
    {
        const std::string full = FormatGameClock(event.mGameTimeMs) + " " + JoinRadarEventText(parts);
        ImGui::SetTooltip("%s%s", full.c_str(), event.mLocation ? "\n点击：镜头移到事件位置" : "");
    }
    ImGui::PopID();
}

void DrawTriangle(ImDrawList* drawList, const ImVec2& center, float size, bool isPointingDown, ImU32 color)
{
    const float half = size * 0.5f;
    if (isPointingDown)
        drawList->AddTriangleFilled(ImVec2(center.x - half, center.y - half * 0.6f),
                                    ImVec2(center.x + half, center.y - half * 0.6f),
                                    ImVec2(center.x, center.y + half * 0.7f), color);
    else
        drawList->AddTriangleFilled(ImVec2(center.x - half * 0.6f, center.y - half),
                                    ImVec2(center.x - half * 0.6f, center.y + half),
                                    ImVec2(center.x + half * 0.7f, center.y), color);
}

// 标题行：折叠按钮、标题文字，以及展开时右侧的“历史”开关。
void DrawTitleRow(RadarBarState& state, const ImVec2& windowPosition, float windowWidth, const BarContext& context)
{
    const RadarBarMetrics& m = context.mMetrics;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float border = ImGui::GetStyle().WindowBorderSize;
    const float rounding = std::max(0.0f, ImGui::GetStyle().WindowRounding - border);
    drawList->AddRectFilled(ImVec2(windowPosition.x + border, windowPosition.y + border),
                            ImVec2(windowPosition.x + windowWidth - border, windowPosition.y + m.mTitleHeight),
                            TITLE_FILL, rounding, state.mIsCollapsed ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
    if (!state.mIsCollapsed)
        drawList->AddLine(ImVec2(windowPosition.x + border, windowPosition.y + m.mTitleHeight),
                          ImVec2(windowPosition.x + windowWidth - border, windowPosition.y + m.mTitleHeight), TITLE_LINE);

    // 折叠按钮：标题行左端的方块，三角形朝下表示已展开、朝右表示已折叠。
    ImGui::SetCursorScreenPos(windowPosition);
    if (ImGui::InvisibleButton(FOLD_BUTTON_ID, ImVec2(m.mTitleHeight, m.mTitleHeight)))
        state.mIsCollapsed = !state.mIsCollapsed;
    const bool isFoldHovered = ImGui::IsItemHovered();
    if (isFoldHovered)
        drawList->AddRectFilled(ImVec2(windowPosition.x + border, windowPosition.y + border),
                                ImVec2(windowPosition.x + m.mTitleHeight, windowPosition.y + m.mTitleHeight - 1.0f),
                                IM_COL32(255, 255, 255, 28), rounding);
    DrawTriangle(drawList, ImVec2(windowPosition.x + m.mTitleHeight * 0.5f, windowPosition.y + m.mTitleHeight * 0.5f),
                 std::round(m.mTitleHeight * 0.34f), !state.mIsCollapsed, IM_COL32(236, 232, 214, 255));
    ImGui::SetItemTooltip("%s", state.mIsCollapsed ? "展开雷达事件" : "折叠雷达事件");

    const float lineHeight = ImGui::GetTextLineHeight();
    drawList->AddText(ImVec2(windowPosition.x + m.mTitleHeight, windowPosition.y + (m.mTitleHeight - lineHeight) * 0.5f),
                      ImGui::ColorConvertFloat4ToU32(TITLE_TEXT_COLOR), "雷达事件");

    if (state.mIsCollapsed)
        return;
    // “历史”开关：按下后标题行下改为可滚动的全部历史（不淡出）。
    const char* label = "历史";
    const float buttonWidth = std::round(ImGui::CalcTextSize(label).x + m.mPadding * 2.0f);
    const float buttonHeight = std::round(m.mTitleHeight * 0.74f);
    const ImVec2 buttonMin(windowPosition.x + windowWidth - m.mPadding - buttonWidth,
                           windowPosition.y + std::round((m.mTitleHeight - buttonHeight) * 0.5f));
    ImGui::SetCursorScreenPos(buttonMin);
    if (ImGui::InvisibleButton(HISTORY_BUTTON_ID, ImVec2(buttonWidth, buttonHeight)))
        state.mShowHistory = !state.mShowHistory;
    const bool isHovered = ImGui::IsItemHovered();
    const ImVec4 inactiveFill(0.095f, 0.12f, 0.105f, 1.0f);
    const ImVec4 hoveredFill(0.22f, 0.27f, 0.22f, 1.0f);
    const ImVec4 fill = state.mShowHistory ? ACTIVE_BUTTON_COLOR : isHovered ? hoveredFill : inactiveFill;
    const CardMetrics card = GetCardMetrics();
    drawList->AddRectFilled(buttonMin, ImVec2(buttonMin.x + buttonWidth, buttonMin.y + buttonHeight),
                            ImGui::ColorConvertFloat4ToU32(fill), card.mRounding);
    drawList->AddRect(buttonMin, ImVec2(buttonMin.x + buttonWidth, buttonMin.y + buttonHeight),
                      IM_COL32(140, 160, 142, 255), card.mRounding, ImDrawFlags_None, card.mBorder);
    drawList->AddText(ImVec2(buttonMin.x + std::round((buttonWidth - ImGui::CalcTextSize(label).x) * 0.5f),
                             buttonMin.y + std::round((buttonHeight - lineHeight) * 0.5f)),
                      state.mShowHistory ? IM_COL32(250, 235, 189, 255) : IM_COL32(236, 232, 214, 255), label);
    ImGui::SetItemTooltip("%s", state.mShowHistory ? "返回最近事件（30 秒内最多 5 条）"
                                                   : "查看全部历史事件（最多 200 条，可滚动）");
}

void DrawHint(const char* text, const ImVec2& windowPosition, float windowWidth, const BarContext& context)
{
    const RadarBarMetrics& m = context.mMetrics;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float lineHeight = ImGui::GetTextLineHeight();
    const ImVec4 clip(windowPosition.x, windowPosition.y + m.mTitleHeight, windowPosition.x + windowWidth,
                      windowPosition.y + m.mTitleHeight + m.mHintHeight);
    drawList->AddText(nullptr, 0.0f,
                      ImVec2(windowPosition.x + m.mPadding + m.mStripeWidth,
                             windowPosition.y + m.mTitleHeight + std::round((m.mHintHeight - lineHeight) * 0.5f)),
                      HINT_COLOR, text, nullptr, 0.0f, &clip);
}
}

std::optional<UiRect> DrawRadarBar(const viewer::ReplayViewSnapshot& snapshot, RadarBarState& state,
                                   const RadarBarLayoutInput& layoutInput, const ImVec2& origin,
                                   const ReplayHud::IconResolver& iconResolver, viewer::ReplayViewExchange& exchange,
                                   std::vector<UiRect>& inputRegions)
{
    const viewer::RadarEventList* events = snapshot.mRadarEvents.get();
    if (!events)
    {
        state.mAges.Reset();
        state.mWasHovered = false;
        return std::nullopt;
    }
    // 没有游戏时钟（统计停用）时以最新事件的时间为“现在”，事件不会淡出。
    const std::int64_t gameNowMs = snapshot.mGameTime ? snapshot.mGameTime->mMilliseconds
        : events->mEvents.empty() ? 0 : events->mEvents.back().mGameTimeMs;
    state.mAges.Update(events, gameNowMs, state.mWasHovered);
    std::vector<RadarRow> rows = state.mShowHistory ? SelectHistoryRadarRows(events)
                                                    : SelectRecentRadarRows(events, state.mAges);
    // 最近模式只显示放得下的最新几条（画面小时少于 5 条），不让行越出窗口。
    if (!state.mShowHistory)
        rows.resize(std::min(rows.size(), GetRadarBarRowCapacity(layoutInput)));

    RadarBarLayoutInput layout = layoutInput;
    layout.mIsCollapsed = state.mIsCollapsed;
    layout.mShowHistory = state.mShowHistory;
    layout.mRowCount = rows.size();
    const UiRect rect = GetRadarBarRect(layout);

    BarContext context;
    context.mMetrics = GetRadarBarMetrics(layout.mScale);
    context.mUnit = GetHudUnit();
    context.mCardGap = std::max(1.0f, std::round(2.0f * context.mUnit));
    const RadarBarMetrics& m = context.mMetrics;

    ImGui::SetNextWindowPos(ImVec2(origin.x + rect.mLeft, origin.y + rect.mTop));
    ImGui::SetNextWindowSize(ImVec2(rect.mRight - rect.mLeft, rect.mBottom - rect.mTop));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, WINDOW_FILL);
    ImGui::PushStyleColor(ImGuiCol_Border, WINDOW_BORDER);
    // 内容全部按窗口左上角手工定位，所以去掉内边距，使标题行的按钮能贴边点到。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, std::floor(context.mUnit)));
    // 折叠后的高度（标题行）比默认的窗口最小尺寸更矮；不放开的话 ImGui 会把窗口撑到最小尺寸，盖住标题行之外的地图。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse;
    bool isHovered = false;
    if (ImGui::Begin("雷达事件##ReplayRadarBar", nullptr, flags))
    {
        const ImVec2 windowPosition = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        inputRegions.push_back({windowPosition.x, windowPosition.y, windowPosition.x + windowSize.x,
                                windowPosition.y + windowSize.y});
        DrawTitleRow(state, windowPosition, windowSize.x, context);
        if (!state.mIsCollapsed)
        {
            const float listTop = windowPosition.y + m.mTitleHeight;
            if (!state.mShowHistory)
            {
                if (rows.empty())
                    DrawHint("近 30 秒没有雷达事件", windowPosition, windowSize.x, context);
                for (std::size_t index = 0; index < rows.size(); ++index)
                    DrawRow(snapshot, rows[index], ImVec2(windowPosition.x, listTop + static_cast<float>(index) * m.mRowHeight),
                            windowSize.x, context, iconResolver, exchange);
            }
            else
            {
                // 历史：可滚动的子窗口，行宽扣掉滚动条；没有事件时写一行提示。
                const float listHeight = std::max(1.0f, windowPosition.y + windowSize.y - listTop - m.mPadding);
                ImGui::SetCursorScreenPos(ImVec2(windowPosition.x, listTop));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                if (ImGui::BeginChild("##雷达事件历史列表", ImVec2(windowSize.x, listHeight), false,
                                      ImGuiWindowFlags_NoBackground))
                {
                    if (rows.empty())
                    {
                        ImGui::SetCursorPosY(std::round((m.mHintHeight - ImGui::GetTextLineHeight()) * 0.5f));
                        ImGui::SetCursorPosX(m.mPadding + m.mStripeWidth);
                        ImGui::TextDisabled("暂无事件");
                    }
                    const float rowWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(rows.size()), m.mRowHeight);
                    while (clipper.Step())
                    {
                        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                            DrawRow(snapshot, rows[static_cast<std::size_t>(index)], ImGui::GetCursorScreenPos(),
                                    rowWidth, context, iconResolver, exchange);
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);
            }
        }
        isHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    state.mWasHovered = isHovered;
    return rect;
}
}
