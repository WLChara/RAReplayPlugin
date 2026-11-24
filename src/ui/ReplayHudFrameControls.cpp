#include "ReplayHudParts.h"

#include "../viewer/ReplayViewExchange.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

namespace rareplay::ui::detail
{
namespace
{
constexpr const char* SPEED_LABELS[] = {
    "最快", "很快", "快", "正常", "慢", "很慢", "最慢"};
// 底栏内容区比它窄（100% 档）时用窄布局。
constexpr float COMPACT_FOOTER_WIDTH = 730.0f;
// 进度条可见部分的高度（100% 档），与原来的只读进度条相同；可交互时整行高度都可点。
constexpr float SEEK_BAR_HEIGHT = 12.0f;
// 快进目标与拖动目标的标记；拖到不可跳转的位置时改用灰色。
constexpr ImU32 SEEK_MARKER_COLOR = IM_COL32(236, 235, 219, 255);
constexpr ImU32 SEEK_BLOCKED_MARKER_COLOR = IM_COL32(150, 154, 146, 220);

UiRect GetLastItemRect()
{
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return {min.x, min.y, max.x, max.y};
}

const char* GetSpeedLabel(int speed)
{
    return speed >= 0 && speed <= 6 ? SPEED_LABELS[speed] : "未知";
}

bool CanShowSpeedCombo(const viewer::ReplayViewSnapshot& snapshot)
{
    return snapshot.mCanChangePlaybackSpeed && snapshot.mActive;
}

float GetSpeedComboWidth(bool isCompact, float scale)
{
    return (isCompact ? 150.0f : 160.0f) * scale;
}

// 速度控件（下拉框加“速度”，或不可调说明）与宽布局的“F10 隐藏”占用的宽度；同一行的进度条为它们留出位置。
float MeasureSpeedControls(const viewer::ReplayViewSnapshot& snapshot, bool isCompact, float scale)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = 0.0f;
    if (CanShowSpeedCombo(snapshot))
    {
        width = GetSpeedComboWidth(isCompact, scale) + style.ItemSpacing.x + ImGui::CalcTextSize("速度").x;
    }
    else
    {
        char text[64];
        std::snprintf(text, sizeof(text), "速度 %s · 不可调", GetSpeedLabel(snapshot.mPlaybackSpeedSetting));
        width = ImGui::CalcTextSize(text).x;
    }
    if (!isCompact)
        width += style.ItemSpacing.x + ImGui::CalcTextSize("F10 隐藏").x;
    return width;
}

void DrawSpeedControls(const viewer::ReplayViewSnapshot& snapshot, viewer::ReplayViewExchange& exchange,
                       std::vector<UiRect>& inputRegions, bool isCompact, bool isSeeking, float scale)
{
    const int speed = snapshot.mPlaybackSpeedSetting;
    const char* currentLabel = GetSpeedLabel(speed);
    if (CanShowSpeedCombo(snapshot))
    {
        // 快进中不限速，调速在快进期间没有可见效果，控件禁用。
        ImGui::BeginDisabled(isSeeking);
        ImGui::SetNextItemWidth(GetSpeedComboWidth(isCompact, scale));
        if (ImGui::BeginCombo("##播放速度", currentLabel))
        {
            for (int setting = 0; setting <= 6; ++setting)
            {
                if (ImGui::Selectable(SPEED_LABELS[setting], speed == setting))
                {
                    exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::SetPlaybackSpeed, setting});
                }
            }
            const ImVec2 popupPosition = ImGui::GetWindowPos();
            const ImVec2 popupSize = ImGui::GetWindowSize();
            inputRegions.push_back({popupPosition.x, popupPosition.y,
                                    popupPosition.x + popupSize.x, popupPosition.y + popupSize.y});
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("速度");
    }
    else
    {
        ImGui::TextDisabled("速度 %s · 不可调", currentLabel);
    }
    if (!isCompact && ImGui::GetContentRegionAvail().x > 85.0f * scale)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("F10 隐藏");
    }
}

// 只读进度：总帧数已知但不能跳转时，与原来的进度条相同。
UiRect DrawReadOnlyProgress(const SeekBarModel& model, float width, float scale)
{
    ImGui::SetNextItemWidth(width);
    ImGui::ProgressBar(GetBarFractionOfFrame(model.mCurrentFrame, model.mTotalFrames),
                       ImVec2(width, SEEK_BAR_HEIGHT * scale), "");
    return GetLastItemRect();
}

// 竖直标记线，贯穿整个可点区域的高度。
void DrawSeekMarker(ImDrawList* drawList, float x, const UiRect& rect, ImU32 color, float scale)
{
    const float thickness = std::max(1.0f, std::round(2.0f * scale));
    drawList->AddLine(ImVec2(x, rect.mTop + 1.0f), ImVec2(x, rect.mBottom - 1.0f), color, thickness);
}

/**
 * 可交互进度条：可点区域是整行高度，可见的条居中。已播放部分高亮；快进中淡色标出到目标的区间并画目标标记。
 * 只有在可跳转的位置按下、并在可跳转的位置松开时才提交一次 SeekToFrame，拖动过程中只显示目标标记。
 */
UiRect DrawInteractiveSeekBar(const viewer::ReplayViewSnapshot& snapshot, const SeekBarModel& model,
                              viewer::ReplayViewExchange& exchange, SeekBarState& state, float width, float scale)
{
    ImGui::InvisibleButton("##ReplaySeekBar", ImVec2(std::max(1.0f, width), ImGui::GetFrameHeight()));
    const UiRect rect = GetLastItemRect();
    const bool isActive = ImGui::IsItemActive();
    const bool isHovered = ImGui::IsItemHovered();
    const float barWidth = std::max(1.0f, rect.mRight - rect.mLeft);
    // 拖出进度条时按两端处理：松开在右侧之外即最后一帧。
    const float pointerX = std::clamp(ImGui::GetIO().MousePos.x, rect.mLeft, rect.mRight);
    const std::uint32_t pointerFrame = GetFrameAtBarFraction((pointerX - rect.mLeft) / barWidth, model.mTotalFrames);
    if (ImGui::IsItemActivated())
        state.mIsDragging = CanSeekToFrame(model, pointerFrame);
    if (ImGui::IsItemDeactivated())
    {
        // 命令队列有界、满了丢最旧的：整个拖动只在松开时提交一次。
        if (state.mIsDragging && CanSeekToFrame(model, pointerFrame))
            exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::SeekToFrame,
                             static_cast<int>(std::min<std::uint32_t>(pointerFrame, INT_MAX))});
        state.mIsDragging = false;
    }
    const bool isDragging = isActive && state.mIsDragging;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float barHeight = std::max(4.0f, std::round(SEEK_BAR_HEIGHT * scale));
    const float barTop = std::round((rect.mTop + rect.mBottom - barHeight) * 0.5f);
    const ImVec2 barMin(rect.mLeft, barTop);
    const ImVec2 barMax(rect.mRight, barTop + barHeight);
    const float rounding = ImGui::GetStyle().FrameRounding;
    const auto frameX = [&](std::uint32_t frame) {
        return rect.mLeft + barWidth * GetBarFractionOfFrame(frame, model.mTotalFrames);
    };
    drawList->AddRectFilled(barMin, barMax,
                            ImGui::GetColorU32(isHovered || isActive ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                            rounding);
    const ImU32 playedColor = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    const float playedX = frameX(model.mCurrentFrame);
    if (playedX > barMin.x)
        drawList->AddRectFilled(barMin, ImVec2(playedX, barMax.y), playedColor, rounding,
                                ImDrawFlags_RoundCornersLeft);
    if (model.mIsSeeking)
    {
        const float targetX = frameX(model.mSeekTargetFrame);
        if (targetX > playedX)
            drawList->AddRectFilled(ImVec2(playedX, barMin.y), ImVec2(targetX, barMax.y), WithOpacity(playedColor, 0.35f));
        DrawSeekMarker(drawList, targetX, rect, SEEK_MARKER_COLOR, scale);
    }
    if (isDragging)
    {
        DrawSeekMarker(drawList, pointerX, rect,
                       CanSeekToFrame(model, pointerFrame) ? SEEK_MARKER_COLOR : SEEK_BLOCKED_MARKER_COLOR, scale);
    }
    else if (isHovered)
    {
        drawList->AddLine(ImVec2(pointerX, barMin.y), ImVec2(pointerX, barMax.y), WithOpacity(SEEK_MARKER_COLOR, 0.6f),
                          std::max(1.0f, std::round(scale)));
    }
    if (isHovered || isDragging)
        ImGui::SetTooltip("%s", BuildSeekBarTooltip(model, snapshot.mTimeline.get(), pointerFrame).c_str());
    return rect;
}
}

bool IsFooterCompact(float contentWidth, float scale) noexcept
{
    return contentWidth < COMPACT_FOOTER_WIDTH * scale;
}

FrameControlRects DrawFrameControls(const viewer::ReplayViewSnapshot& snapshot, const SeekBarModel& seekModel,
                                    viewer::ReplayViewExchange& exchange, std::vector<UiRect>& inputRegions,
                                    SeekBarState& seekBar, bool isCompact, float scale)
{
    FrameControlRects rects;
    const bool isSeeking = seekModel.mIsSeeking;
    // 进度条不可交互时没有进行中的拖动；下次可交互时从新的按下开始。
    if (seekModel.mMode != SeekBarMode::Interactive)
        seekBar.mIsDragging = false;

    const auto status = snapshot.mPlaybackControls.mStatus;
    ImGui::BeginDisabled(status == viewer::PlaybackStatus::Ended || !snapshot.mActive || isSeeking);
    if (ImGui::Button(status == viewer::PlaybackStatus::Playing ? "暂停" : "继续"))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::SetPaused,
            status == viewer::PlaybackStatus::Playing ? 1 : 0});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!snapshot.mPlaybackControls.mCanRestart || !snapshot.mActive);
    if (ImGui::Button("重新播放"))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::RestartPlayback, 0});
    ImGui::EndDisabled();
    if (!snapshot.mPlaybackControls.mCanRestart && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("需要更新启动器并重新打开回放，才能重新播放");
    ImGui::SameLine();
    if (ImGui::Button("退出回放"))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::ExitPlayback, 0});
    if (status == viewer::PlaybackStatus::Ended)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("回放结束");
    }
    if (!snapshot.mPlaybackControls.mRestartError.empty())
        ImGui::TextWrapped("%s", snapshot.mPlaybackControls.mRestartError.c_str());

    // 第二行：帧号（快进中为状态文字与“取消”）、宽布局下的进度条、速度。
    if (isSeeking)
    {
        ImGui::TextUnformatted(seekModel.mSeekStatus.c_str());
        ImGui::SameLine();
        if (ImGui::Button("取消##ReplayCancelSeek"))
            exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::CancelSeek, 0});
        rects.mSeek.mCancelButton = GetLastItemRect();
    }
    else if (seekModel.mMode != SeekBarMode::Hidden)
    {
        ImGui::Text("帧 %u / %u", snapshot.mFrameNumber, seekModel.mTotalFrames);
    }
    else
    {
        ImGui::Text("当前帧 %u · 总帧未知", snapshot.mFrameNumber);
    }

    if (!isCompact && seekModel.mMode != SeekBarMode::Hidden)
    {
        ImGui::SameLine();
        const float width = std::max(100.0f * scale, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x -
                                                         MeasureSpeedControls(snapshot, false, scale));
        rects.mSeek.mBar = seekModel.mMode == SeekBarMode::Interactive
                               ? DrawInteractiveSeekBar(snapshot, seekModel, exchange, seekBar, width, scale)
                               : DrawReadOnlyProgress(seekModel, width, scale);
    }
    ImGui::SameLine();
    DrawSpeedControls(snapshot, exchange, inputRegions, isCompact, isSeeking, scale);

    // 窄布局：可交互的进度条单独占一行，宿主已为这一行加高底栏。
    if (NeedsSeekBarRow(seekModel, isCompact))
    {
        rects.mSeek.mBar = DrawInteractiveSeekBar(snapshot, seekModel, exchange, seekBar,
                                                  ImGui::GetContentRegionAvail().x, scale);
        rects.mSeek.mIsOnOwnRow = true;
    }
    rects.mSeek.mIsInteractive = rects.mSeek.mBar.has_value() && seekModel.mMode == SeekBarMode::Interactive;

    bool halfShroud = snapshot.mPlaybackControls.mIsHalfShroudEnabled;
    ImGui::BeginDisabled(!snapshot.mActive || isSeeking);
    if (ImGui::Checkbox("半透明黑幕（50%）", &halfShroud))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::SetHalfShroud, halfShroud ? 1 : 0});
    ImGui::EndDisabled();
    // 缩放控件放在黑幕开关所在的第三行：前两行的进度条与速度控件排布保持不变。
    rects.mZoom = DrawBattlefieldZoomControls(snapshot, exchange, scale);
    return rects;
}
}
