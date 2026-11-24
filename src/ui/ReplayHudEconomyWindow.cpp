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
constexpr ImU32 TITLE_FILL = IM_COL32(42, 52, 44, 255);
constexpr ImU32 TITLE_LINE = IM_COL32(104, 122, 108, 255);
constexpr ImVec4 TITLE_TEXT_COLOR(0.92f, 0.85f, 0.62f, 1.0f);
constexpr ImVec4 WINDOW_FILL(0.090f, 0.106f, 0.098f, 1.0f);
constexpr ImVec4 WINDOW_BORDER(0.46f, 0.53f, 0.47f, 1.0f);
constexpr ImVec4 ACTIVE_BUTTON_COLOR(0.42f, 0.36f, 0.19f, 1.0f);
constexpr ImVec4 ACTIVE_BUTTON_TEXT_COLOR(0.98f, 0.92f, 0.74f, 1.0f);
// 100% 档下绘图区的最小宽度；图例放右侧时图表不能比它更窄，否则图例改放下方。
constexpr float MIN_PLOT_WIDTH = 260.0f;
constexpr char CLOSE_BUTTON_ID[] = "##关闭经济大窗口";
constexpr char DRAG_AREA_ID[] = "##拖动经济大窗口";

struct RangeButton
{
    EconomyTimeRange mRange;
    const char* mLabel;
};
constexpr RangeButton RANGE_BUTTONS[] = {
    {EconomyTimeRange::All, "全部"},
    {EconomyTimeRange::LastTenMinutes, "最近 10 分钟"},
    {EconomyTimeRange::LastFiveMinutes, "最近 5 分钟"}};

// 标题栏：自绘（窗口无系统标题栏），拖动区占标题栏除关闭按钮外的部分，按下时开始拖动。
// 返回是否点了关闭按钮。拖动本身在下一帧窗口放置前按鼠标位置更新，所以窗口位置完全由 EconomyWindowState 持有。
bool DrawTitleBar(EconomyWindowState& window, const ImVec2& windowPosition, const ImVec2& windowSize,
                  float titleHeight, float padding)
{
    const float unit = GetHudUnit();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float border = ImGui::GetStyle().WindowBorderSize;
    const float rounding = std::max(0.0f, ImGui::GetStyle().WindowRounding - border);
    // 窗口内容的裁剪区比窗口边缘向内缩半个内边距；标题栏要铺到边框内侧，所以临时取消裁剪，
    // 只在边框以内绘制，不盖住 ImGui 已画好的窗口边框。
    drawList->PushClipRect(windowPosition, ImVec2(windowPosition.x + windowSize.x, windowPosition.y + titleHeight + 1.0f),
                           false);
    drawList->AddRectFilled(ImVec2(windowPosition.x + border, windowPosition.y + border),
                            ImVec2(windowPosition.x + windowSize.x - border, windowPosition.y + titleHeight), TITLE_FILL,
                            rounding, ImDrawFlags_RoundCornersTop);
    drawList->AddLine(ImVec2(windowPosition.x + border, windowPosition.y + titleHeight),
                      ImVec2(windowPosition.x + windowSize.x - border, windowPosition.y + titleHeight), TITLE_LINE);
    drawList->PopClipRect();

    const float lineHeight = ImGui::GetTextLineHeight();
    ImGui::SetCursorScreenPos(ImVec2(windowPosition.x + padding, windowPosition.y + (titleHeight - lineHeight) * 0.5f));
    ImGui::TextColored(TITLE_TEXT_COLOR, "资金曲线");

    // ImGui 的内容裁剪区比窗口边缘向内缩半个内边距，边缘之外的控件点不到；
    // 所以拖动区从裁剪区边缘开始；关闭按钮与标题文字一样内缩一个内边距，右缘正好是内容区右缘。
    const float inset = std::floor(padding * 0.5f);
    const float closeSize = std::round(titleHeight - 8.0f * unit);
    const float closeLeft = windowPosition.x + windowSize.x - padding - closeSize;
    const float closeTop = windowPosition.y + (titleHeight - closeSize) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(windowPosition.x + inset, windowPosition.y + inset));
    ImGui::InvisibleButton(DRAG_AREA_ID, ImVec2(std::max(1.0f, closeLeft - windowPosition.x - inset - 2.0f),
                                                std::max(1.0f, titleHeight - inset)));
    if (ImGui::IsItemActivated())
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        window.mIsDragging = true;
        window.mDragOffsetX = mouse.x - windowPosition.x;
        window.mDragOffsetY = mouse.y - windowPosition.y;
    }
    ImGui::SetItemTooltip("拖动标题栏移动窗口");

    ImGui::SetCursorScreenPos(ImVec2(closeLeft, closeTop));
    const bool isClosed = ImGui::InvisibleButton(CLOSE_BUTTON_ID, ImVec2(closeSize, closeSize));
    const bool isHovered = ImGui::IsItemHovered();
    if (isHovered)
    {
        drawList->AddRectFilled(ImVec2(closeLeft, closeTop), ImVec2(closeLeft + closeSize, closeTop + closeSize),
                                IM_COL32(128, 66, 58, 255), ImGui::GetStyle().FrameRounding);
    }
    // 字体里没有乘号字形，用两条斜线画“×”。
    const float margin = closeSize * 0.3f;
    const float thickness = std::max(1.5f, 1.5f * unit);
    const ImU32 crossColor = IM_COL32(236, 232, 214, 255);
    drawList->AddLine(ImVec2(closeLeft + margin, closeTop + margin),
                      ImVec2(closeLeft + closeSize - margin, closeTop + closeSize - margin), crossColor, thickness);
    drawList->AddLine(ImVec2(closeLeft + closeSize - margin, closeTop + margin),
                      ImVec2(closeLeft + margin, closeTop + closeSize - margin), crossColor, thickness);
    ImGui::SetItemTooltip("关闭");
    return isClosed;
}

// 时间范围按钮与“全部显示”按钮从左到右排布，放不下就换行。
void DrawToolbar(EconomyUiState& state, const std::vector<int>& candidates)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    float lineUsed = 0.0f;
    const auto place = [&](float width, float extraGap) {
        if (lineUsed > 0.0f && lineUsed + spacing + extraGap + width <= available)
        {
            ImGui::SameLine(0.0f, spacing + extraGap);
            lineUsed += spacing + extraGap + width;
        }
        else
        {
            lineUsed = width;
        }
    };
    for (const RangeButton& button : RANGE_BUTTONS)
    {
        const float width = ImGui::CalcTextSize(button.mLabel).x + style.FramePadding.x * 2.0f;
        place(width, 0.0f);
        const bool isSelected = state.mRange == button.mRange;
        if (isSelected)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ACTIVE_BUTTON_COLOR);
            ImGui::PushStyleColor(ImGuiCol_Text, ACTIVE_BUTTON_TEXT_COLOR);
        }
        if (ImGui::Button(button.mLabel, ImVec2(std::min(width, available), 0.0f)))
            state.mRange = button.mRange;
        if (isSelected)
            ImGui::PopStyleColor(2);
    }
    const float showAllWidth = ImGui::CalcTextSize("全部显示").x + style.FramePadding.x * 2.0f;
    place(showAllWidth, std::round(spacing));
    const bool hasHidden = state.mFilter.CountHidden(candidates) > 0;
    ImGui::BeginDisabled(!hasHidden);
    if (ImGui::Button("全部显示", ImVec2(std::min(showAllWidth, available), 0.0f)))
        state.mFilter.ShowAll();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("恢复显示全部玩家的曲线");
}

// 图例各列的固定宽度；名称占用剩下的宽度。
struct LegendMetrics
{
    float mChip = 0.0f;
    float mCheckboxWidth = 0.0f;
    float mButtonWidth = 0.0f;
    float mMaxValueWidth = 0.0f;
    float mRowHeight = 0.0f;
    /** 名称不少于约 4 个字宽时一行所需的最小宽度（含滚动条）。 */
    float mNeededWidth = 0.0f;
};

LegendMetrics MeasureLegend(const std::vector<ChartSeries>& series)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const CardMetrics card = GetCardMetrics();
    LegendMetrics metrics;
    metrics.mChip = std::max(6.0f, ImGui::GetFontSize() * 0.62f);
    metrics.mCheckboxWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("显示").x;
    metrics.mButtonWidth = ImGui::CalcTextSize("只看").x + style.FramePadding.x * 2.0f;
    for (const ChartSeries& entry : series)
    {
        char value[32];
        const auto credits = GetCurrentCredits(entry);
        if (credits)
            std::snprintf(value, sizeof(value), "%lld", static_cast<long long>(*credits));
        else
            std::snprintf(value, sizeof(value), "—");
        metrics.mMaxValueWidth = std::max(metrics.mMaxValueWidth, ImGui::CalcTextSize(value).x);
    }
    metrics.mRowHeight = ImGui::GetFrameHeight() + card.mPadding;
    metrics.mNeededWidth = card.mPadding * 2.0f + metrics.mChip + card.mPadding + ImGui::GetFontSize() * 4.0f +
                           card.mPadding + metrics.mMaxValueWidth + card.mPadding + metrics.mCheckboxWidth +
                           card.mPadding + metrics.mButtonWidth + style.ScrollbarSize;
    return metrics;
}

// 一行：色块、玩家名、当前资金、“显示”勾选框、“只看”按钮，整行是一张卡片。
// 点击名称区域等同于“只看”；被隐藏的玩家变暗。isOnly 表示当前只显示该玩家（卡片高亮）。
void DrawLegendRow(const ChartSeries& entry, const LegendMetrics& metrics, EconomyPlayerFilter& filter,
                   const std::vector<int>& candidates, std::size_t visibleCount, bool isOnly)
{
    const CardMetrics card = GetCardMetrics();
    const viewer::PlayerSummary& player = *entry.mPlayer;
    const int houseIndex = player.mHouseIndex;
    ImGui::PushID(houseIndex);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = metrics.mRowHeight;
    const ImVec2 end(start.x + width, start.y + height);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const bool isRowHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(start, end);
    DrawCardBackground(drawList, start, end,
                       isOnly ? CardState::Selected : isRowHovered ? CardState::Hovered : CardState::Normal);

    const float buttonLeft = end.x - card.mPadding - metrics.mButtonWidth;
    const float checkboxLeft = buttonLeft - card.mPadding - metrics.mCheckboxWidth;
    const float valueRight = checkboxLeft - card.mPadding;
    const char* name = GetPlayerDisplayName(player);

    // 名称、色块与资金所在区域是一个大按钮：点击只看该玩家。
    ImGui::SetCursorScreenPos(start);
    if (ImGui::InvisibleButton("##只看名称", ImVec2(std::max(1.0f, valueRight + card.mPadding * 0.5f - start.x), height)))
        filter.ShowOnly(houseIndex, candidates);
    ImGui::SetItemTooltip("只看 %s", name);

    const float alpha = entry.mIsVisible ? 1.0f : 0.45f;
    const ImU32 color = GetChartColor(player.mColorRgb);
    const float chipTop = start.y + (height - metrics.mChip) * 0.5f;
    if (entry.mIsVisible)
        drawList->AddRectFilled(ImVec2(start.x + card.mPadding, chipTop),
                                ImVec2(start.x + card.mPadding + metrics.mChip, chipTop + metrics.mChip), color, 2.0f);
    else
        drawList->AddRect(ImVec2(start.x + card.mPadding, chipTop),
                          ImVec2(start.x + card.mPadding + metrics.mChip, chipTop + metrics.mChip), color, 2.0f);

    char value[32];
    const auto credits = GetCurrentCredits(entry);
    if (credits)
        std::snprintf(value, sizeof(value), "%lld", static_cast<long long>(*credits));
    else
        std::snprintf(value, sizeof(value), "—");
    const float valueWidth = ImGui::CalcTextSize(value).x;
    const float textTop = start.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    const float valueLeft = std::max(start.x, valueRight - valueWidth);
    const float nameLeft = start.x + card.mPadding + metrics.mChip + card.mPadding;
    const ImVec4 nameClip(nameLeft, start.y, std::max(nameLeft, valueLeft - card.mPadding), end.y);
    drawList->AddText(nullptr, 0.0f, ImVec2(nameLeft, textTop),
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.94f, 0.92f, 0.85f, alpha)), name, nullptr, 0.0f, &nameClip);
    const ImVec4 valueClip(valueLeft, start.y, valueRight + card.mPadding * 0.5f, end.y);
    drawList->AddText(nullptr, 0.0f, ImVec2(valueLeft, textTop),
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.96f, 0.87f, 0.64f, alpha)), value, nullptr, 0.0f, &valueClip);

    // 控件背景比卡片更深并带浅色边框，否则与卡片底色几乎一样而看不出勾选框与按钮。
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.075f, 0.094f, 0.082f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.17f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.18f, 0.22f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.095f, 0.12f, 0.105f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.27f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.63f, 0.56f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, card.mBorder);

    // “显示”勾选框：最后一位可见玩家不能取消勾选（至少保留一位可见）。
    const float widgetTop = start.y + (height - ImGui::GetFrameHeight()) * 0.5f;
    const bool isLastVisible = entry.mIsVisible && visibleCount <= 1;
    bool isVisible = entry.mIsVisible;
    ImGui::SetCursorScreenPos(ImVec2(checkboxLeft, widgetTop));
    ImGui::BeginDisabled(isLastVisible);
    if (ImGui::Checkbox("显示", &isVisible))
        filter.SetVisible(houseIndex, isVisible, candidates);
    ImGui::EndDisabled();
    if (isLastVisible)
        ImGui::SetItemTooltip("至少保留一位可见玩家");
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip(entry.mIsVisible ? "隐藏该玩家的曲线" : "显示该玩家的曲线");

    ImGui::SetCursorScreenPos(ImVec2(buttonLeft, widgetTop));
    if (isOnly)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ACTIVE_BUTTON_COLOR);
        ImGui::PushStyleColor(ImGuiCol_Text, ACTIVE_BUTTON_TEXT_COLOR);
    }
    if (ImGui::Button("只看", ImVec2(metrics.mButtonWidth, 0.0f)))
        filter.ShowOnly(houseIndex, candidates);
    if (isOnly)
        ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(6);

    // 行本身登记为占位项，布局据此前进（含卡片间距）。
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
}

void DrawLegend(const std::vector<ChartSeries>& series, const LegendMetrics& metrics, EconomyUiState& state)
{
    const std::vector<int> candidates = GetChartHouseIndices(series);
    const std::size_t visibleCount = state.mFilter.CountVisible(candidates);
    const std::optional<int> onlyVisible = state.mFilter.GetOnlyVisible(candidates);
    CardListSpacing spacing;
    for (const ChartSeries& entry : series)
    {
        const bool isOnly = onlyVisible && *onlyVisible == entry.mPlayer->mHouseIndex;
        DrawLegendRow(entry, metrics, state.mFilter, candidates, visibleCount, isOnly);
    }
}

// 窗口内容：工具栏、图表（图例在右侧，窗口窄时在下方）与坐标轴说明。
void DrawWindowBody(const viewer::ReplayViewSnapshot& snapshot, int selectedHouseIndex, EconomyUiState& state,
                    const ImVec2& windowPosition, const ImVec2& windowSize, float padding)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const CardMetrics card = GetCardMetrics();
    const float unit = GetHudUnit();
    const float lineHeight = ImGui::GetTextLineHeight();
    const bool isNominal = snapshot.mGameTime && snapshot.mGameTime->mIsNominal;

    std::vector<ChartSeries> series = CollectChartSeries(snapshot, state.mFilter);
    const std::vector<int> candidates = GetChartHouseIndices(series);
    state.mFilter.Normalize(candidates);
    if (!series.empty())
        series = CollectChartSeries(snapshot, state.mFilter);

    DrawToolbar(state, candidates);
    if (series.empty())
    {
        TextWrappedDisabled("暂无资金曲线数据");
        return;
    }

    // 说明文字占一行或两行（窗口很窄时折行）；图表与图例分摊其余高度。
    const float contentWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const char* captionText = isNominal ? "纵轴 资金 · 横轴 游戏时间（名义）" : "纵轴 资金 · 横轴 游戏时间";
    const float captionLines = std::ceil(ImGui::CalcTextSize(captionText).x / std::max(1.0f, contentWidth - 2.0f));
    const float captionHeight = lineHeight * std::max(1.0f, captionLines);
    const float bottom = windowPosition.y + windowSize.y - padding;
    const float bodyTop = ImGui::GetCursorScreenPos().y;
    const float spacing = style.ItemSpacing.y;
    const float bodyHeight = std::max(lineHeight * 8.0f, bottom - bodyTop - captionHeight - spacing);

    const LegendMetrics legend = MeasureLegend(series);
    const float minPlotWidth = std::round(MIN_PLOT_WIDTH * unit);
    const float gap = style.ItemSpacing.x;
    const bool isLegendBeside = ShouldPlaceEconomyLegendBeside(contentWidth, legend.mNeededWidth, minPlotWidth, gap);

    ChartParams params;
    params.mSelectedHouseIndex = selectedHouseIndex;
    params.mRange = state.mRange;
    params.mLineThickness = 2.5f * unit;
    params.mSelectedThickness = 3.5f * unit;
    params.mIsNominalTime = isNominal;

    const float legendRowsHeight = static_cast<float>(series.size()) * (legend.mRowHeight + card.mGap);
    const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
    if (isLegendBeside)
    {
        const float legendWidth = std::clamp(legend.mNeededWidth + ImGui::GetFontSize() * 4.0f, legend.mNeededWidth,
                                             std::max(legend.mNeededWidth, contentWidth - gap - minPlotWidth));
        params.mWidth = std::max(1.0f, contentWidth - legendWidth - gap);
        params.mHeight = bodyHeight;
        ImGui::BeginGroup();
        DrawEconomyPlot(series, params);
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, gap);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, transparent);
        if (ImGui::BeginChild("##经济图例", ImVec2(legendWidth, bodyHeight), false))
            DrawLegend(series, legend, state);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    else
    {
        // 图例在下方：图表至少保留约 8 行字高，其余给图例；放不下的图例在子窗口内滚动。
        const float minPlotHeight = lineHeight * 8.0f;
        const float legendHeight = std::clamp(bodyHeight - minPlotHeight - spacing, std::min(legendRowsHeight, lineHeight * 2.0f),
                                              std::max(legend.mRowHeight, legendRowsHeight));
        params.mWidth = contentWidth;
        params.mHeight = std::max(minPlotHeight, bodyHeight - legendHeight - spacing);
        DrawEconomyPlot(series, params);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, transparent);
        if (ImGui::BeginChild("##经济图例", ImVec2(contentWidth, std::max(legend.mRowHeight, legendHeight)), false))
            DrawLegend(series, legend, state);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    DrawEconomyAxisCaption(isNominal);
}
}

std::optional<UiRect> DrawEconomyWindow(const viewer::ReplayViewSnapshot& snapshot, int selectedHouseIndex,
                                        EconomyUiState& state, const EconomyWindowLayoutInput& layout,
                                        const ImVec2& origin, std::vector<UiRect>& inputRegions)
{
    EconomyWindowState& window = state.mWindow;
    if (!window.mIsOpen)
        return std::nullopt;

    // 位置在窗口放置前确定：拖动中跟随鼠标，再统一夹在画面内，所以拖动永远不会把窗口拖出画面。
    const ImGuiIO& io = ImGui::GetIO();
    std::optional<UiPoint> position;
    if (window.mHasPosition)
        position = UiPoint{window.mLeft, window.mTop};
    if (window.mIsDragging)
    {
        if (io.MouseDown[0])
            position = UiPoint{io.MousePos.x - origin.x - window.mDragOffsetX + 0.0f,
                               io.MousePos.y - origin.y - window.mDragOffsetY + 0.0f};
        else
            window.mIsDragging = false;
    }
    const UiRect rect = GetEconomyWindowRect(layout, position);
    if (position)
    {
        // 记下夹过的位置：画面缩小把窗口推回后，画面再变大也不会自己跑回原处。
        window.mHasPosition = true;
        window.mLeft = rect.mLeft;
        window.mTop = rect.mTop;
    }

    const float unit = GetHudUnit();
    const float padding = std::round(8.0f * unit);
    ImGui::SetNextWindowPos(ImVec2(origin.x + rect.mLeft, origin.y + rect.mTop));
    ImGui::SetNextWindowSize(ImVec2(rect.mRight - rect.mLeft, rect.mBottom - rect.mTop));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, WINDOW_FILL);
    ImGui::PushStyleColor(ImGuiCol_Border, WINDOW_BORDER);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, std::round(4.0f * unit));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, std::floor(unit)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(std::round(6.0f * unit), std::round(5.0f * unit)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(5.0f * unit), std::round(3.0f * unit)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("资金曲线##ReplayEconomyWindow", nullptr, flags))
    {
        const ImVec2 windowPosition = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        inputRegions.push_back({windowPosition.x, windowPosition.y, windowPosition.x + windowSize.x,
                                windowPosition.y + windowSize.y});
        const float titleHeight = ImGui::GetFrameHeight() + std::round(4.0f * unit);
        const bool isClosed = DrawTitleBar(window, windowPosition, windowSize, titleHeight, padding);
        ImGui::SetCursorScreenPos(ImVec2(windowPosition.x + padding,
                                         windowPosition.y + titleHeight + std::round(6.0f * unit)));
        if (isClosed)
        {
            window.mIsOpen = false;
            window.mIsDragging = false;
        }
        else
        {
            DrawWindowBody(snapshot, selectedHouseIndex, state, windowPosition, windowSize, padding);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(2);
    return rect;
}
}
