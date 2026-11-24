#include "ReplayHudParts.h"

#include "../viewer/ReplayViewExchange.h"

#include <algorithm>
#include <cmath>

namespace rareplay::ui::detail
{
namespace
{
// 文字按最宽的情形占固定宽度：换档或出现等待标记时按钮不左右移动，连续点击不会点偏。
constexpr char WIDEST_ZOOM_LABEL[] = "缩放 400%…";
constexpr char WIDEST_COMPACT_ZOOM_LABEL[] = "400%…";
// 与半透明黑幕开关之间的间距（100% 档）。
constexpr float GROUP_GAP = 24.0f;

UiRect GetLastItemRect()
{
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return {min.x, min.y, max.x, max.y};
}

// 小按钮：不可用时置灰且点击无效；返回是否被点击。
bool DrawZoomButton(const char* label, bool isEnabled, const ImVec2& size, UiRect& outRect)
{
    ImGui::BeginDisabled(!isEnabled);
    const bool isClicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    outRect = GetLastItemRect();
    return isClicked;
}
}

std::optional<ZoomControlRects> DrawBattlefieldZoomControls(const viewer::ReplayViewSnapshot& snapshot,
                                                            viewer::ReplayViewExchange& exchange, float scale)
{
    // 快进中画面只按节拍刷新，缩放按钮与暂停、半黑幕一同禁用（游戏线程也丢弃这些命令）。
    const ZoomControlModel model = BuildZoomControlModel(snapshot.mBattlefieldZoom,
        snapshot.mActive && !snapshot.mPlaybackControls.mSeek.mIsSeeking);
    if (model.mMode == ZoomControlMode::Hidden)
        return std::nullopt;

    const ImGuiStyle& style = ImGui::GetStyle();
    // 本行剩余宽度：上一个控件（黑幕开关）右缘到窗口内容区右缘。依次尝试“完整文字 + 宽间距”“短文字 + 宽间距”
    // “短文字 + 普通间距”，取第一种放得下的；都放不下时用最后一种，超出部分与底栏其他控件一样被窗口裁剪。
    const float lineRemaining =
        ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - ImGui::GetItemRectMax().x;
    const float wideGap = std::round(GROUP_GAP * scale);
    const float narrowGap = style.ItemSpacing.x;
    const auto chooseLayout = [&](float fullWidth, float compactWidth, bool& outIsCompact) {
        outIsCompact = wideGap + fullWidth > lineRemaining;
        if (!outIsCompact)
            return wideGap;
        return wideGap + compactWidth <= lineRemaining ? wideGap : narrowGap;
    };

    ZoomControlRects rects;
    rects.mMode = model.mMode;
    bool isCompact = false;
    if (model.mMode == ZoomControlMode::Unavailable)
    {
        const float gap = chooseLayout(ImGui::CalcTextSize(model.mLabel.c_str()).x,
                                       ImGui::CalcTextSize(model.mCompactLabel.c_str()).x, isCompact);
        ImGui::SameLine(0.0f, gap);
        ImGui::TextDisabled("%s", (isCompact ? model.mCompactLabel : model.mLabel).c_str());
        rects.mGroup = GetLastItemRect();
        if (isCompact && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", model.mLabel.c_str());
        return rects;
    }

    const float buttonSide = ImGui::GetFrameHeight();
    const ImVec2 buttonSize(buttonSide, buttonSide);
    const float resetWidth = ImGui::CalcTextSize("复原").x + style.FramePadding.x * 2.0f;
    const float buttonsWidth = style.ItemSpacing.x + buttonSide + style.ItemInnerSpacing.x + buttonSide +
                               style.ItemSpacing.x + resetWidth;
    const float fullLabelWidth =
        std::max(ImGui::CalcTextSize(WIDEST_ZOOM_LABEL).x, ImGui::CalcTextSize(model.mLabel.c_str()).x);
    const float compactLabelWidth = std::max(ImGui::CalcTextSize(WIDEST_COMPACT_ZOOM_LABEL).x,
                                             ImGui::CalcTextSize(model.mCompactLabel.c_str()).x);
    const float gap = chooseLayout(fullLabelWidth + buttonsWidth, compactLabelWidth + buttonsWidth, isCompact);
    const std::string& label = isCompact ? model.mCompactLabel : model.mLabel;
    const float labelWidth = isCompact ? compactLabelWidth : fullLabelWidth;

    ImGui::SameLine(0.0f, gap);
    // 按钮以战术区中心为锚点：点击时鼠标在底栏上，而底栏第三行与战术区底部重叠，读鼠标会把锚点放到按钮下方的地面。
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label.c_str());
    // 补足到固定宽度后再放按钮。
    ImGui::SameLine(0.0f, labelWidth - ImGui::GetItemRectSize().x + style.ItemSpacing.x);
    if (DrawZoomButton("－##BattlefieldZoomOut", model.mCanZoomOut, buttonSize, rects.mZoomOut))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::AdjustBattlefieldZoomAtCenter, -1});
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    if (DrawZoomButton("＋##BattlefieldZoomIn", model.mCanZoomIn, buttonSize, rects.mZoomIn))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::AdjustBattlefieldZoomAtCenter, 1});
    ImGui::SameLine();
    if (DrawZoomButton("复原##BattlefieldZoomReset", model.mCanReset, ImVec2(0.0f, 0.0f), rects.mReset))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::ResetBattlefieldZoom, 0});
    ImGui::EndGroup();
    rects.mGroup = GetLastItemRect();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (isCompact)
            ImGui::SetTooltip("%s\n%s", model.mLabel.c_str(), ZOOM_CONTROL_TOOLTIP);
        else
            ImGui::SetTooltip("%s", ZOOM_CONTROL_TOOLTIP);
    }
    return rects;
}
}
