#include "ReplayHud.h"

#include "ReplayHudParts.h"
#include "ReplayHudLayout.h"
#include "ReplayHudScale.h"
#include "../viewer/ReplayViewExchange.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rareplay::ui
{
namespace
{
constexpr float FPS_WIDTH = 258.0f;
constexpr float FOOTER_HEIGHT = 106.0f;
constexpr char SEEKING_LABEL[] = "快进中";
// “快进中”标签的内边距（100% 档）与半透明背景（与 HUD 窗口同色，降低不透明度，透出下面停住的画面）。
constexpr float SEEKING_LABEL_PADDING_X = 24.0f;
constexpr float SEEKING_LABEL_PADDING_Y = 12.0f;
constexpr ImVec4 SEEKING_LABEL_BACKGROUND(0.105f, 0.125f, 0.118f, 0.72f);

void RecordWindowRegion(std::vector<UiRect>& regions)
{
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    regions.push_back({position.x, position.y, position.x + size.x, position.y + size.y});
}

/**
 * 快进中战场不重绘，在地图区（侧栏面板左缘、底栏上缘以内）正中显示“快进中”：当前字体，半透明背景，不拦截输入。
 * mapRight 与 footerTop 相对 origin；返回标签窗口矩形（主视口客户区坐标），窗口也记入 visibleRegions。
 */
UiRect DrawSeekingLabel(const ImVec2& origin, float mapRight, float footerTop, float scale,
                        std::vector<UiRect>& visibleRegions)
{
    const ImVec2 textSize = ImGui::CalcTextSize(SEEKING_LABEL);
    const ImVec2 padding(std::round(SEEKING_LABEL_PADDING_X * scale), std::round(SEEKING_LABEL_PADDING_Y * scale));
    const ImVec2 size(std::ceil(textSize.x) + 2.0f * padding.x, std::ceil(textSize.y) + 2.0f * padding.y);
    const float left = std::max(0.0f, std::round((mapRight - size.x) * 0.5f));
    const float top = std::max(0.0f, std::round((footerTop - size.y) * 0.5f));
    ImGui::SetNextWindowPos(ImVec2(origin.x + left, origin.y + top));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, SEEKING_LABEL_BACKGROUND);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoInputs;
    UiRect rect{origin.x + left, origin.y + top, origin.x + left + size.x, origin.y + top + size.y};
    if (ImGui::Begin("快进中##ReplaySeekingLabel", nullptr, flags))
    {
        RecordWindowRegion(visibleRegions);
        rect = visibleRegions.back();
        ImGui::TextUnformatted(SEEKING_LABEL);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return rect;
}

// 半径与内边距随 HUD 缩放，其余样式尺寸由 ApplyStyleScale 统一缩放。
void PushHudStyle(float scale)
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.105f, 0.125f, 0.118f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.135f, 0.155f, 0.140f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.205f, 0.225f, 0.194f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.28f, 0.30f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.32f, 0.19f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.44f, 0.39f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.53f, 0.43f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.39f, 0.79f, 0.67f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.86f, 0.69f, 0.31f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.25f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.34f, 0.35f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.43f, 0.38f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.20f, 0.24f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.40f, 0.37f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.36f, 0.32f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocused, ImVec4(0.20f, 0.24f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, ImVec4(0.36f, 0.32f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.80f, 0.65f, 0.31f, 1.0f));
    // 分隔线统一为较浅的灰绿，与卡片边框和分区标题下的分隔线一致。
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.44f, 0.51f, 0.45f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);
    // 内边距取整，避免 ImGui 下取整窗口区域后控件越出半个像素。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(12.0f * scale), std::round(10.0f * scale)));
}

void PopHudStyle()
{
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(19);
}
}

void ReplayHud::Draw(const viewer::ReplayViewSnapshot& snapshot, viewer::ReplayViewExchange& exchange)
{
    mInputRegions.clear();
    mVisibleRegions.clear();
    mEconomyWindowRect.reset();
    mRadarBarRect.reset();
    mZoomControlRects.reset();
    mSeekBarRects = SeekBarRects();
    mSeekingLabelRect.reset();
    mDrawnMarkCount = 0;
    mIsSidebarPanelCovering = false;
    if (mSessionId != snapshot.mSessionId)
    {
        mSessionId = snapshot.mSessionId;
        mSelectedHouseIndex = -1;
        mSeekBar = SeekBarState();
        if (!mSelectUnitCategoryPending)
            mSelectedUnitCategory.reset();
        if (!mSelectPanelPending)
        {
            mSelectedPanel = ReplayPanel::Overview;
        }
        // 经济大窗口的开合、位置、时间范围与玩家筛选只在本次会话内保持。
        if (!mEconomyPresetPending)
            mEconomy = EconomyUiState();
        // 提示栏的折叠与历史模式同样只在本次会话内保持。
        if (!mRadarPresetPending)
            mRadar = RadarBarState();
        else
            mRadar.mAges.Reset();
    }
    mRadarPresetPending = false;

    mSelectUnitCategoryPending = false;
    {
        // 筛选的候选是有资金曲线的玩家；先应用预置的“只看”，再保证至少一位可见（玩家集合变化时自愈）。
        const std::vector<int> candidates = detail::CollectEconomyHouseIndices(snapshot);
        if (mPendingEconomyOnly)
            mEconomy.mFilter.ShowOnly(*mPendingEconomyOnly, candidates);
        mEconomy.mFilter.Normalize(candidates);
    }
    mPendingEconomyOnly.reset();
    mEconomyPresetPending = false;
    if (detail::FindPlayer(snapshot, mSelectedHouseIndex) == nullptr)
    {
        mSelectedHouseIndex = snapshot.mRecordedLocalHouseIndex;
        if (detail::FindPlayer(snapshot, mSelectedHouseIndex) == nullptr && !snapshot.mPlayers.empty())
        {
            mSelectedHouseIndex = snapshot.mPlayers.front().mHouseIndex;
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 extent = viewport->WorkSize;
    if (extent.x < 400.0f || extent.y < 300.0f)
    {
        return;
    }

    ApplyStyleScale();
    PushHudStyle(mScale);
    const float edgeGap = std::round(HUD_EDGE_GAP * mScale);
    const float footerHeight =
        std::round((snapshot.mPlaybackControls.mRestartError.empty() ? FOOTER_HEIGHT : 144.0f) * mScale);
    const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

    // 超武落点图标画在背景绘制层，所有 HUD 窗口之下；位置来自游戏线程每个迭代发布的独立通道。
    mDrawnMarkCount = detail::DrawSuperWeaponMarks(snapshot, exchange, mGameImage, origin, extent, mScale,
                                                   mIconResolver);

    // 面板占用原生侧栏区并按缩放向左加宽；布局未就绪、侧栏在左或空间不足时不显示，原生侧栏照常绘制。
    const auto panelPlacement = GetSidebarPanelPlacement(snapshot.mNativeUiLayout, extent.x, extent.y,
                                                         mGameImage, mScale);
    const float mapRight = panelPlacement ? panelPlacement->mRect.mLeft : extent.x;

    const float fpsWidth = std::min(std::round(FPS_WIDTH * mScale),
                                    std::max(std::round(180.0f * mScale), mapRight - edgeGap * 2.0f));
    ImGui::SetNextWindowPos(ImVec2(origin.x + edgeGap, origin.y + edgeGap));
    ImGui::SetNextWindowSize(ImVec2(fpsWidth, std::round(FPS_CARD_HEIGHT * mScale)));
    if (ImGui::Begin("录制帧率##ReplayFps", nullptr, fixedFlags | ImGuiWindowFlags_NoInputs))
    {
        RecordWindowRegion(mVisibleRegions);
        detail::DrawFpsCard(snapshot, mScale);
    }
    ImGui::End();

    if (panelPlacement)
    {
        const UiRect& rect = panelPlacement->mRect;
        DrawSidebarPanel(snapshot, {origin.x + rect.mLeft, origin.y + rect.mTop,
                                    origin.x + rect.mRight, origin.y + rect.mBottom}, exchange);
        mIsSidebarPanelCovering = panelPlacement->mIsExact;
    }

    // 底栏贴左、下边缘，右到面板左缘（面板未显示时到画面右缘），并加高到盖住原生命令栏；
    // 它盖住的是仍在绘制的原生画面，所以背景不透明、无圆角，整块区域都拦截输入。
    FooterLayoutInput footerInput;
    footerInput.mDisplayWidth = extent.x;
    footerInput.mDisplayHeight = extent.y;
    footerInput.mContentHeight = footerHeight;
    if (panelPlacement)
        footerInput.mPanelLeft = panelPlacement->mRect.mLeft;
    footerInput.mCommandBar = GetCommandBarClientRect(snapshot.mNativeUiLayout, extent.x, extent.y, mGameImage);
    UiRect footer = GetFooterRect(footerInput);
    // 底栏宽度与高度无关：先按宽度判定窄布局，窄布局下可交互的进度条单独占一行，再为这一行加高。
    const SeekBarModel seekModel = BuildSeekBarModel(snapshot);
    const bool isFooterCompact = detail::IsFooterCompact(
        footer.mRight - footer.mLeft - 2.0f * ImGui::GetStyle().WindowPadding.x, mScale);
    if (detail::NeedsSeekBarRow(seekModel, isFooterCompact))
    {
        footerInput.mContentHeight += std::round(ImGui::GetFrameHeightWithSpacing());
        footer = GetFooterRect(footerInput);
    }
    ImGui::SetNextWindowPos(ImVec2(origin.x + footer.mLeft, origin.y + footer.mTop));
    ImGui::SetNextWindowSize(ImVec2(footer.mRight - footer.mLeft, footer.mBottom - footer.mTop));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.090f, 0.106f, 0.098f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("播放状态##ReplayFooter", nullptr, fixedFlags | ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        RecordWindowRegion(mInputRegions);
        // 战局缩放控件与进度条也在底栏窗口内，随整块底栏拦截输入，点击不会漏给游戏。
        detail::FrameControlRects frameRects = detail::DrawFrameControls(snapshot, seekModel, exchange, mInputRegions,
                                                                         mSeekBar, isFooterCompact, mScale);
        mZoomControlRects = frameRects.mZoom;
        mSeekBarRects = std::move(frameRects.mSeek);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // 快进中战场停在原处，中央的“快进中”说明画面没有在更新；它在提示栏与经济大窗口之下，F10 隐藏时随 HUD 一起不画。
    if (seekModel.mIsSeeking)
        mSeekingLabelRect = DrawSeekingLabel(origin, mapRight, footer.mTop, mScale, mVisibleRegions);

    // 雷达事件提示栏在地图区左侧中部，位于经济大窗口之下：两者重叠时大窗口盖住提示栏。
    RadarBarLayoutInput radarLayout;
    radarLayout.mDisplayWidth = extent.x;
    radarLayout.mDisplayHeight = extent.y;
    if (panelPlacement)
        radarLayout.mPanelLeft = panelPlacement->mRect.mLeft;
    radarLayout.mFooterTop = footer.mTop;
    radarLayout.mScale = mScale;
    mRadarBarRect = detail::DrawRadarBar(snapshot, mRadar, radarLayout, origin, mIconResolver, exchange, mInputRegions);

    // 经济大窗口在地图区上层，最后创建以排在面板与底栏之上；窗口矩形整块拦截输入。
    if (mEconomy.mWindow.mIsOpen)
    {
        EconomyWindowLayoutInput windowLayout;
        windowLayout.mDisplayWidth = extent.x;
        windowLayout.mDisplayHeight = extent.y;
        if (panelPlacement)
            windowLayout.mPanelLeft = panelPlacement->mRect.mLeft;
        windowLayout.mFooterTop = footer.mTop;
        windowLayout.mScale = mScale;
        mEconomyWindowRect = detail::DrawEconomyWindow(snapshot, mSelectedHouseIndex, mEconomy, windowLayout,
                                                       origin, mInputRegions);
    }
    mVisibleRegions.insert(mVisibleRegions.end(), mInputRegions.begin(), mInputRegions.end());
    PopHudStyle();
}

void ReplayHud::Reset()
{
    mSessionId = 0;
    mSelectedHouseIndex = -1;
    mInputRegions.clear();
    mVisibleRegions.clear();
    mSelectedPanel = ReplayPanel::Overview;
    mSelectPanelPending = false;
    mSelectedUnitCategory.reset();
    mSelectUnitCategoryPending = false;
    mEconomy = EconomyUiState();
    mEconomyPresetPending = false;
    mPendingEconomyOnly.reset();
    mEconomyWindowRect.reset();
    mRadar = RadarBarState();
    mRadarPresetPending = false;
    mRadarBarRect.reset();
    mZoomControlRects.reset();
    mSeekBarRects = SeekBarRects();
    mSeekBar = SeekBarState();
    mSeekingLabelRect.reset();
    mDrawnMarkCount = 0;
    mIsSidebarPanelCovering = false;
    mAppliedStyleScale = 0.0f;
}

void ReplayHud::SetRadarBarCollapsed(bool isCollapsed) noexcept
{
    mRadar.mIsCollapsed = isCollapsed;
    mRadarPresetPending = true;
}

void ReplayHud::SetRadarBarHistory(bool showHistory) noexcept
{
    mRadar.mShowHistory = showHistory;
    mRadarPresetPending = true;
}

void ReplayHud::SetEconomyWindowOpen(bool isOpen) noexcept
{
    mEconomy.mWindow.mIsOpen = isOpen;
    if (!isOpen)
        mEconomy.mWindow.mIsDragging = false;
    mEconomyPresetPending = true;
}

void ReplayHud::SelectEconomyRange(EconomyTimeRange range) noexcept
{
    mEconomy.mRange = range;
    mEconomyPresetPending = true;
}

void ReplayHud::ShowOnlyEconomyPlayer(int houseIndex) noexcept
{
    mPendingEconomyOnly = houseIndex;
    mEconomyPresetPending = true;
}

void ReplayHud::SetScalePercent(int percent) noexcept
{
    mScalePercent = std::clamp(percent, HUD_SCALE_MIN_PERCENT, HUD_SCALE_MAX_PERCENT);
    mScale = static_cast<float>(mScalePercent) / 100.0f;
}

void ReplayHud::ApplyStyleScale()
{
    if (mAppliedStyleScale == mScale)
        return;
    // 从默认样式重新缩放，避免在已缩放的尺寸上累乘；颜色由 PushHudStyle 覆盖。
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.ScaleAllSizes(mScale);
    mAppliedStyleScale = mScale;
}

void ReplayHud::SetIconResolver(IconResolver resolver)
{
    mIconResolver = std::move(resolver);
}

void ReplayHud::SelectPanel(ReplayPanel panel)
{
    mSelectedPanel = panel;
    mSelectPanelPending = true;
}

const std::vector<UiRect>& ReplayHud::GetInputRegions() const noexcept
{
    return mInputRegions;
}

const std::vector<UiRect>& ReplayHud::GetVisibleRegions() const noexcept
{
    return mVisibleRegions;
}
}
