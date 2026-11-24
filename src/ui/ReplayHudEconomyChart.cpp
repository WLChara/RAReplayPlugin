#include "ReplayHudEconomyChart.h"

#include "ReplayHudFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace rareplay::ui::detail
{
namespace
{
constexpr ImU32 PLOT_BACKGROUND = IM_COL32(13, 18, 15, 255);
constexpr ImU32 PLOT_BORDER = IM_COL32(100, 118, 104, 255);
// 浅色网格线：在深色绘图区上清晰可见但不抢曲线的风头。
constexpr ImU32 GRID_COLOR = IM_COL32(70, 86, 74, 255);
constexpr ImU32 ZERO_LINE_COLOR = IM_COL32(112, 132, 116, 255);
constexpr ImU32 AXIS_TEXT_COLOR = IM_COL32(178, 198, 178, 255);
constexpr ImU32 CURSOR_COLOR = IM_COL32(232, 232, 208, 190);
constexpr ImU32 EMPTY_TEXT_COLOR = IM_COL32(150, 158, 146, 255);
constexpr ImVec4 NOMINAL_COLOR(0.96f, 0.71f, 0.35f, 1.0f);
// 所有曲线合计最多绘制的点数：抗锯齿的粗折线每点 4 个顶点，12000 点约 4.8 万顶点，
// 低于 16 位索引的 64K 上限（即使后端不支持 VtxOffset 也不会溢出），并为文字与网格留足余量。
constexpr std::size_t MAX_TOTAL_DRAW_POINTS = 12000;
constexpr std::size_t MIN_DRAW_POINTS_PER_SERIES = 64;

struct ChartGeometry
{
    ImVec2 mOrigin;
    float mWidth = 0.0f;
    ImVec2 mPlotMin;
    ImVec2 mPlotMax;
    EconomyTimeWindow mWindow;
    EconomyYAxis mYAxis;

    float X(std::int64_t timeMs) const
    {
        return mPlotMin.x + static_cast<float>(static_cast<double>(timeMs - mWindow.mStartMs) /
                                               static_cast<double>(mWindow.mSpanMs)) * (mPlotMax.x - mPlotMin.x);
    }
    float Y(std::int64_t credits) const
    {
        return mPlotMax.y - static_cast<float>(static_cast<double>(credits - mYAxis.mMin) /
                                               static_cast<double>(mYAxis.mMax - mYAxis.mMin)) *
                                (mPlotMax.y - mPlotMin.y);
    }
};

// 用色块代替字形：字体里没有实心方块。
void DrawColorChip(ImU32 color, float size)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImGui::GetWindowDrawList()->AddRectFilled(start, ImVec2(start.x + size, start.y + size), color, 2.0f);
}

void DrawYAxis(ImDrawList* drawList, const ChartGeometry& geometry)
{
    for (std::int64_t value = geometry.mYAxis.mMin; value <= geometry.mYAxis.mMax; value += geometry.mYAxis.mStep)
    {
        const float y = geometry.Y(value);
        drawList->AddLine(ImVec2(geometry.mPlotMin.x, y), ImVec2(geometry.mPlotMax.x, y),
                          value == 0 ? ZERO_LINE_COLOR : GRID_COLOR);
        const std::string label = FormatCreditsTick(value);
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        // 标签垂直居中在刻度线上，并夹在绘图区内，使最顶与最底的标签也完整可见。
        const float top = std::clamp(y - size.y * 0.5f, geometry.mPlotMin.y, geometry.mPlotMax.y - size.y);
        drawList->AddText(ImVec2(geometry.mPlotMin.x - size.x - 4.0f, top), AXIS_TEXT_COLOR, label.c_str());
    }
}

void DrawXAxis(ImDrawList* drawList, const ChartGeometry& geometry)
{
    const std::int64_t startMs = geometry.mWindow.mStartMs;
    const std::int64_t endMs = geometry.mWindow.GetEndMs();
    const float gap = std::max(4.0f, std::round(ImGui::GetFontSize() * 0.35f));
    const float plotWidth = geometry.mPlotMax.x - geometry.mPlotMin.x;
    const float widest = ImGui::CalcTextSize(FormatGameClock(endMs).c_str()).x;
    const int maxTicks = std::max(1, static_cast<int>(plotWidth / (widest + gap)));
    const std::int64_t step = GetNiceTimeStepMs(geometry.mWindow.mSpanMs / maxTicks);
    float lastRight = geometry.mOrigin.x - gap;
    const float left = geometry.mOrigin.x;
    const float right = geometry.mOrigin.x + geometry.mWidth;
    const std::int64_t first = (startMs + step - 1) / step * step;
    for (std::int64_t tick = first; tick <= endMs; tick += step)
    {
        const float x = geometry.X(tick);
        drawList->AddLine(ImVec2(x, geometry.mPlotMin.y), ImVec2(x, geometry.mPlotMax.y), GRID_COLOR);
        const std::string label = FormatGameClock(tick);
        const float width = ImGui::CalcTextSize(label.c_str()).x;
        // 标签居中在刻度上并夹在控件范围内；与上一个标签重叠就省略它。
        const float labelLeft = std::clamp(x - width * 0.5f, left, std::max(left, right - width));
        if (labelLeft < lastRight + gap)
            continue;
        drawList->AddText(ImVec2(labelLeft, geometry.mPlotMax.y + 2.0f), AXIS_TEXT_COLOR, label.c_str());
        lastRight = labelLeft + width;
    }
}

void DrawCurves(ImDrawList* drawList, const ChartGeometry& geometry, const std::vector<ChartSeries>& series,
                const ChartParams& params)
{
    std::size_t visibleCount = 0;
    for (const ChartSeries& entry : series)
        visibleCount += entry.mIsVisible ? 1 : 0;
    if (visibleCount == 0)
        return;
    // 绘制点数不超过绘图区像素宽度的两倍（每像素保留最小与最大值），并受总点数上限约束。
    const float plotWidth = geometry.mPlotMax.x - geometry.mPlotMin.x;
    const std::size_t perSeriesCap = std::clamp(static_cast<std::size_t>(std::max(0.0f, plotWidth) * 2.0f),
        MIN_DRAW_POINTS_PER_SERIES, std::max(MIN_DRAW_POINTS_PER_SERIES, MAX_TOTAL_DRAW_POINTS / visibleCount));
    std::vector<viewer::EconomyPoint> slicePoints;
    std::vector<ImVec2> screenPoints;
    const std::int64_t startMs = geometry.mWindow.mStartMs;
    const std::int64_t endMs = geometry.mWindow.GetEndMs();
    drawList->PushClipRect(geometry.mPlotMin, geometry.mPlotMax, true);
    // 所选玩家最后画，线更粗，不会被其他曲线盖住。
    for (const bool drawSelected : {false, true})
    {
        for (const ChartSeries& entry : series)
        {
            if (!entry.mIsVisible || (entry.mPlayer->mHouseIndex == params.mSelectedHouseIndex) != drawSelected)
                continue;
            const EconomySlice slice = SliceEconomyCurve(*entry.mCurve, startMs, endMs);
            slicePoints.clear();
            if (slice.mLeadIn)
                slicePoints.push_back(*slice.mLeadIn);
            slicePoints.insert(slicePoints.end(), entry.mCurve->mPoints.begin() + static_cast<std::ptrdiff_t>(slice.mFirst),
                               entry.mCurve->mPoints.begin() + static_cast<std::ptrdiff_t>(slice.mEnd));
            if (slice.mLeadOut)
                slicePoints.push_back(*slice.mLeadOut);
            if (slicePoints.empty())
                continue;
            const std::vector<viewer::EconomyPoint> drawn = DecimateEconomyPoints(slicePoints, perSeriesCap);
            screenPoints.clear();
            for (const viewer::EconomyPoint& point : drawn)
                screenPoints.emplace_back(geometry.X(point.mTimeMs), geometry.Y(point.mCredits));
            const ImU32 color = GetChartColor(entry.mPlayer->mColorRgb);
            const float thickness = drawSelected ? params.mSelectedThickness : params.mLineThickness;
            if (screenPoints.size() >= 2)
                drawList->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), color,
                                      ImDrawFlags_None, thickness);
            // 曲线的最后一个样本在窗口内（或只有一个点）时，在末端画圆点标出“当前位置”。
            if (screenPoints.size() == 1 || (slice.mEnd == entry.mCurve->mPoints.size() && !slice.mLeadOut))
                drawList->AddCircleFilled(screenPoints.back(), thickness * 1.6f, color);
        }
    }
    drawList->PopClipRect();
}

// 悬停读数：时间取第一条可见曲线中离鼠标最近的样本，各曲线在该时刻取最近的样本点。
void DrawHoverReadout(ImDrawList* drawList, const ChartGeometry& geometry, const std::vector<ChartSeries>& series,
                      const ChartParams& params)
{
    const ChartSeries* reference = nullptr;
    for (const ChartSeries& entry : series)
    {
        if (entry.mIsVisible && !entry.mCurve->mPoints.empty())
        {
            reference = &entry;
            break;
        }
    }
    if (reference == nullptr)
        return;
    const float plotWidth = std::max(1.0f, geometry.mPlotMax.x - geometry.mPlotMin.x);
    const float mouseX = std::clamp(ImGui::GetIO().MousePos.x, geometry.mPlotMin.x, geometry.mPlotMax.x);
    const double ratio = static_cast<double>(mouseX - geometry.mPlotMin.x) / static_cast<double>(plotWidth);
    const std::int64_t mouseTimeMs = geometry.mWindow.mStartMs +
        static_cast<std::int64_t>(ratio * static_cast<double>(geometry.mWindow.mSpanMs));
    const auto snapped = FindNearestEconomyPoint(*reference->mCurve, mouseTimeMs);
    const std::int64_t timeMs = snapped ? snapped->mTimeMs : mouseTimeMs;
    const float lineX = std::clamp(geometry.X(timeMs), geometry.mPlotMin.x, geometry.mPlotMax.x);
    drawList->AddLine(ImVec2(lineX, geometry.mPlotMin.y), ImVec2(lineX, geometry.mPlotMax.y), CURSOR_COLOR);

    ImGui::BeginTooltip();
    const std::string clock = FormatGameClock(timeMs);
    ImGui::TextUnformatted(clock.c_str());
    if (params.mIsNominalTime)
    {
        ImGui::SameLine();
        ImGui::TextColored(NOMINAL_COLOR, "（名义）");
    }
    const float chip = std::max(6.0f, ImGui::GetFontSize() * 0.62f);
    drawList->PushClipRect(geometry.mPlotMin, geometry.mPlotMax, true);
    for (const ChartSeries& entry : series)
    {
        if (!entry.mIsVisible)
            continue;
        const ImU32 color = GetChartColor(entry.mPlayer->mColorRgb);
        DrawColorChip(color, chip);
        ImGui::SameLine();
        const auto point = FindNearestEconomyPoint(*entry.mCurve, timeMs);
        if (point)
        {
            ImGui::Text("%s %lld", GetPlayerDisplayName(*entry.mPlayer), static_cast<long long>(point->mCredits));
            const ImVec2 marker(lineX, geometry.Y(point->mCredits));
            drawList->AddCircleFilled(marker, 4.0f, color);
            drawList->AddCircle(marker, 4.0f, PLOT_BACKGROUND, 12, 1.5f);
        }
        else
        {
            ImGui::Text("%s —", GetPlayerDisplayName(*entry.mPlayer));
        }
    }
    drawList->PopClipRect();
    ImGui::EndTooltip();
}
}

std::vector<ChartSeries> CollectChartSeries(const viewer::ReplayViewSnapshot& snapshot,
                                            const EconomyPlayerFilter& filter)
{
    std::vector<ChartSeries> series;
    for (const viewer::PlayerSummary& player : snapshot.mPlayers)
    {
        if (player.mEconomy)
            series.push_back({&player, player.mEconomy.get(), filter.IsVisible(player.mHouseIndex)});
    }
    return series;
}

std::vector<int> GetChartHouseIndices(const std::vector<ChartSeries>& series)
{
    std::vector<int> indices;
    indices.reserve(series.size());
    for (const ChartSeries& entry : series)
        indices.push_back(entry.mPlayer->mHouseIndex);
    return indices;
}

std::vector<int> CollectEconomyHouseIndices(const viewer::ReplayViewSnapshot& snapshot)
{
    std::vector<int> indices;
    for (const viewer::PlayerSummary& player : snapshot.mPlayers)
    {
        if (player.mEconomy)
            indices.push_back(player.mHouseIndex);
    }
    return indices;
}

ImU32 GetChartColor(std::uint32_t rgb)
{
    return ImGui::ColorConvertFloat4ToU32(GetPlayerColor(EnsureChartLineColor(rgb)));
}

const char* GetPlayerDisplayName(const viewer::PlayerSummary& player)
{
    return player.mName.empty() ? "未命名玩家" : player.mName.c_str();
}

std::optional<std::int64_t> GetCurrentCredits(const ChartSeries& entry)
{
    if (entry.mPlayer->mCredits)
        return *entry.mPlayer->mCredits;
    if (entry.mCurve != nullptr && !entry.mCurve->mPoints.empty())
        return entry.mCurve->mPoints.back().mCredits;
    return std::nullopt;
}

void DrawEconomyPlot(const std::vector<ChartSeries>& series, const ChartParams& params)
{
    std::vector<const viewer::EconomyCurve*> allCurves;
    std::vector<const viewer::EconomyCurve*> visibleCurves;
    for (const ChartSeries& entry : series)
    {
        allCurves.push_back(entry.mCurve);
        if (entry.mIsVisible)
            visibleCurves.push_back(entry.mCurve);
    }
    // 横轴按全部曲线确定，切换玩家显示不会让时间轴跳动；纵轴只看窗口内的可见曲线。
    const std::optional<EconomyTimeBounds> dataBounds = GetEconomyTimeBounds(allCurves);
    ChartGeometry geometry;
    geometry.mWindow = ResolveEconomyTimeWindow(params.mRange, dataBounds);

    const float lineHeight = ImGui::GetTextLineHeight();
    const float xAxisHeight = lineHeight + 2.0f;
    const float plotHeight = std::max(lineHeight * 3.0f, params.mHeight - xAxisHeight);
    const int targetTicks = std::clamp(static_cast<int>(plotHeight / (lineHeight * 2.0f)), 2, 6);
    geometry.mYAxis = ComputeEconomyYAxis(
        GetEconomyValueBounds(visibleCurves, geometry.mWindow.mStartMs, geometry.mWindow.GetEndMs()), targetTicks);

    geometry.mOrigin = ImGui::GetCursorScreenPos();
    geometry.mWidth = std::max(1.0f, params.mWidth);
    float labelWidth = 0.0f;
    for (std::int64_t value = geometry.mYAxis.mMin; value <= geometry.mYAxis.mMax; value += geometry.mYAxis.mStep)
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(FormatCreditsTick(value).c_str()).x);
    const float rightMargin = std::max(3.0f, std::round(ImGui::GetFontSize() * 0.3f));
    // 左侧留给纵轴标签；极窄时仍保证绘图区至少 2 个字高宽。
    const float plotLeft = std::min(geometry.mOrigin.x + labelWidth + 8.0f,
        geometry.mOrigin.x + std::max(0.0f, geometry.mWidth - lineHeight * 2.0f - rightMargin));
    geometry.mPlotMin = ImVec2(plotLeft, geometry.mOrigin.y);
    geometry.mPlotMax = ImVec2(geometry.mOrigin.x + geometry.mWidth - rightMargin, geometry.mOrigin.y + plotHeight);

    ImGui::InvisibleButton(params.mId, ImVec2(geometry.mWidth, plotHeight + xAxisHeight));
    const bool isHovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(geometry.mPlotMin, geometry.mPlotMax, PLOT_BACKGROUND);
    DrawYAxis(drawList, geometry);
    DrawXAxis(drawList, geometry);
    drawList->AddRect(geometry.mPlotMin, geometry.mPlotMax, PLOT_BORDER);
    DrawCurves(drawList, geometry, series, params);
    if (!dataBounds)
    {
        const char* text = "尚无采样";
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImVec4 clip(geometry.mPlotMin.x, geometry.mPlotMin.y, geometry.mPlotMax.x, geometry.mPlotMax.y);
        drawList->AddText(nullptr, 0.0f,
                          ImVec2(geometry.mPlotMin.x + (geometry.mPlotMax.x - geometry.mPlotMin.x - size.x) * 0.5f,
                                 geometry.mPlotMin.y + (geometry.mPlotMax.y - geometry.mPlotMin.y - size.y) * 0.5f),
                          EMPTY_TEXT_COLOR, text, nullptr, 0.0f, &clip);
    }
    else if (isHovered && ImGui::IsMouseHoveringRect(geometry.mPlotMin, geometry.mPlotMax))
    {
        DrawHoverReadout(drawList, geometry, series, params);
    }
}

void DrawEconomyAxisCaption(bool isNominalTime)
{
    // 名义时间时整行改用琥珀色，提醒横轴只是名义值（单机速度 0 的帧按 16 毫秒计）。
    ImGui::PushStyleColor(ImGuiCol_Text, isNominalTime ? NOMINAL_COLOR
                                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    TextWrappedFit(isNominalTime ? "纵轴 资金 · 横轴 游戏时间（名义）" : "纵轴 资金 · 横轴 游戏时间");
    ImGui::PopStyleColor();
}

void DrawEconomyFilterHint(const std::vector<ChartSeries>& series, const EconomyPlayerFilter& filter)
{
    const std::vector<int> indices = GetChartHouseIndices(series);
    const std::size_t hidden = filter.CountHidden(indices);
    if (hidden == 0)
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, NOMINAL_COLOR);
    if (const auto only = filter.GetOnlyVisible(indices))
    {
        const auto found = std::find_if(series.begin(), series.end(),
            [&only](const ChartSeries& entry) { return entry.mPlayer->mHouseIndex == *only; });
        TextWrappedFit("只看：%s", found == series.end() ? "—" : GetPlayerDisplayName(*found->mPlayer));
    }
    else
    {
        TextWrappedFit("已隐藏 %zu 位", hidden);
    }
    ImGui::PopStyleColor();
}
}
