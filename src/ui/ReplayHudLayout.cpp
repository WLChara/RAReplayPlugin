#include "ReplayHudLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rareplay::ui
{
namespace
{
// 两种解释的各边相差不超过 1 个客户区像素即视为同一位置；更小的差异被向外取整吸收。
constexpr float EXACT_TOLERANCE = 1.0f;
// 低于此尺寸时文字与标签无法排版，宁可不显示面板而保留原生侧栏。
constexpr float MIN_PANEL_WIDTH = 96.0f;
constexpr float MIN_PANEL_HEIGHT = 120.0f;

bool IsFiniteRect(const UiRect& rect) noexcept
{
    return std::isfinite(rect.mLeft) && std::isfinite(rect.mTop) &&
           std::isfinite(rect.mRight) && std::isfinite(rect.mBottom);
}

bool IsUsableImageRect(const UiRect& rect, float displayWidth, float displayHeight) noexcept
{
    return IsFiniteRect(rect) && rect.mRight > rect.mLeft && rect.mBottom > rect.mTop &&
           rect.mLeft >= 0.0f && rect.mTop >= 0.0f &&
           rect.mRight <= displayWidth + EXACT_TOLERANCE && rect.mBottom <= displayHeight + EXACT_TOLERANCE;
}

UiRect MapThrough(const UiRect& image, float canvasWidth, float canvasHeight, const viewer::NativeRect& rect) noexcept
{
    const float scaleX = (image.mRight - image.mLeft) / canvasWidth;
    const float scaleY = (image.mBottom - image.mTop) / canvasHeight;
    return {image.mLeft + static_cast<float>(rect.mX) * scaleX,
            image.mTop + static_cast<float>(rect.mY) * scaleY,
            image.mLeft + static_cast<float>(rect.mX + rect.mWidth) * scaleX,
            image.mTop + static_cast<float>(rect.mY + rect.mHeight) * scaleY};
}

bool IsSamePlacement(const UiRect& left, const UiRect& right) noexcept
{
    return std::abs(left.mLeft - right.mLeft) <= EXACT_TOLERANCE &&
           std::abs(left.mTop - right.mTop) <= EXACT_TOLERANCE &&
           std::abs(left.mRight - right.mRight) <= EXACT_TOLERANCE &&
           std::abs(left.mBottom - right.mBottom) <= EXACT_TOLERANCE;
}

// 向外取整：放大时原生边缘像素经双线性插值会跨入相邻屏幕像素，取整后面板仍完整盖住旧侧栏。
UiRect SnapOutward(const UiRect& rect, float displayWidth, float displayHeight) noexcept
{
    return {std::clamp(std::floor(rect.mLeft), 0.0f, displayWidth),
            std::clamp(std::floor(rect.mTop), 0.0f, displayHeight),
            std::clamp(std::ceil(rect.mRight), 0.0f, displayWidth),
            std::clamp(std::ceil(rect.mBottom), 0.0f, displayHeight)};
}
}

std::optional<NativeRectPlacement> MapNativeRect(const viewer::NativeUiLayout& layout,
    const viewer::NativeRect& rect, float displayWidth, float displayHeight,
    const GameImagePlacement& image, NativeRectPolicy policy) noexcept
{
    if (layout.mCanvasWidth <= 0 || layout.mCanvasHeight <= 0 || rect.mWidth <= 0 || rect.mHeight <= 0 ||
        rect.mX < 0 || rect.mY < 0 ||
        static_cast<std::int64_t>(rect.mX) + rect.mWidth > layout.mCanvasWidth ||
        static_cast<std::int64_t>(rect.mY) + rect.mHeight > layout.mCanvasHeight ||
        !std::isfinite(displayWidth) || !std::isfinite(displayHeight) ||
        displayWidth <= 0.0f || displayHeight <= 0.0f)
        return std::nullopt;

    const float canvasWidth = static_cast<float>(layout.mCanvasWidth);
    const float canvasHeight = static_cast<float>(layout.mCanvasHeight);
    const bool hasImageRect = IsUsableImageRect(image.mRect, displayWidth, displayHeight);
    NativeRectPlacement placement;
    if (image.mIsImageRect && hasImageRect)
    {
        // 宿主确认的图像区域同时覆盖非等比拉伸与留边：客户区 = 原点 + 游戏坐标 × 图像尺寸 / 画布尺寸。
        placement.mRect = MapThrough(image.mRect, canvasWidth, canvasHeight, rect);
        placement.mIsExact = true;
    }
    else
    {
        // 图像区域未知时同时考虑整块拉伸（cnc-ddraw maintas=false）和居中等比留边（maintas=true）。
        const UiRect stretched = MapThrough({0.0f, 0.0f, displayWidth, displayHeight}, canvasWidth, canvasHeight, rect);
        const float fitScale = std::min(displayWidth / canvasWidth, displayHeight / canvasHeight);
        const float fitLeft = (displayWidth - canvasWidth * fitScale) * 0.5f;
        const float fitTop = (displayHeight - canvasHeight * fitScale) * 0.5f;
        const UiRect fitted = MapThrough({fitLeft, fitTop, fitLeft + canvasWidth * fitScale,
                                          fitTop + canvasHeight * fitScale}, canvasWidth, canvasHeight, rect);
        UiRect candidates[3] = {stretched, fitted, stretched};
        std::size_t candidateCount = 2;
        if (hasImageRect)
            candidates[candidateCount++] = MapThrough(image.mRect, canvasWidth, canvasHeight, rect);

        placement.mIsExact = true;
        placement.mRect = stretched;
        for (std::size_t index = 1; index < candidateCount; ++index)
        {
            const UiRect& candidate = candidates[index];
            placement.mIsExact = placement.mIsExact && IsSamePlacement(stretched, candidate);
            // AvoidAbove：上沿取最低以免盖住雷达；CoverAll：上沿取最高以盖住整块原生区域。
            // 左、右、下都取并集，避免漏出原生内容。
            placement.mRect.mTop = policy == NativeRectPolicy::AvoidAbove
                ? std::max(placement.mRect.mTop, candidate.mTop)
                : std::min(placement.mRect.mTop, candidate.mTop);
            placement.mRect.mLeft = std::min(placement.mRect.mLeft, candidate.mLeft);
            placement.mRect.mRight = std::max(placement.mRect.mRight, candidate.mRight);
            placement.mRect.mBottom = std::max(placement.mRect.mBottom, candidate.mBottom);
        }
        if (placement.mIsExact)
            placement.mRect = stretched;
    }

    if (!IsFiniteRect(placement.mRect))
        return std::nullopt;
    placement.mRect = SnapOutward(placement.mRect, displayWidth, displayHeight);
    if (placement.mRect.mRight <= placement.mRect.mLeft || placement.mRect.mBottom <= placement.mRect.mTop)
        return std::nullopt;
    return placement;
}

std::optional<NativeRectPlacement> GetSidebarPanelPlacement(
    const std::optional<viewer::NativeUiLayout>& layout, float displayWidth, float displayHeight,
    const GameImagePlacement& image, float scale) noexcept
{
    if (!layout)
        return std::nullopt;
    auto placement = MapNativeRect(*layout, layout->mSidebarPanel, displayWidth, displayHeight, image);
    if (!placement || placement->mRect.mRight - placement->mRect.mLeft < MIN_PANEL_WIDTH ||
        placement->mRect.mBottom - placement->mRect.mTop < MIN_PANEL_HEIGHT)
        return std::nullopt;
    if (std::isfinite(scale) && scale > 1.0f)
    {
        UiRect& rect = placement->mRect;
        const float nativeWidth = rect.mRight - rect.mLeft;
        rect.mLeft = std::max(0.0f, std::floor(rect.mRight - nativeWidth * scale));
    }
    return placement;
}

std::optional<UiRect> GetCommandBarClientRect(const std::optional<viewer::NativeUiLayout>& layout,
    float displayWidth, float displayHeight, const GameImagePlacement& image) noexcept
{
    if (!layout || !layout->mCommandBar)
        return std::nullopt;
    const auto placement = MapNativeRect(*layout, *layout->mCommandBar, displayWidth, displayHeight, image,
                                         NativeRectPolicy::CoverAll);
    if (!placement)
        return std::nullopt;
    return placement->mRect;
}

int GetUnitCardColumns(float availableWidth, float fontSize, float spacing) noexcept
{
    constexpr int MAX_COLUMNS = 2;
    const float minimumCardWidth = fontSize * 2.5f;
    return std::isfinite(availableWidth) && availableWidth >= minimumCardWidth * MAX_COLUMNS + spacing
        ? MAX_COLUMNS : 1;
}

UiPoint NativePointMapper::MapPoint(float canvasX, float canvasY) const noexcept
{
    return {mImage.mLeft + canvasX * (mImage.mRight - mImage.mLeft) / mCanvasWidth,
            mImage.mTop + canvasY * (mImage.mBottom - mImage.mTop) / mCanvasHeight};
}

UiRect NativePointMapper::MapRect(const viewer::NativeRect& rect) const noexcept
{
    const UiPoint topLeft = MapPoint(static_cast<float>(rect.mX), static_cast<float>(rect.mY));
    const UiPoint bottomRight = MapPoint(static_cast<float>(rect.mX + rect.mWidth),
                                         static_cast<float>(rect.mY + rect.mHeight));
    return {topLeft.mX, topLeft.mY, bottomRight.mX, bottomRight.mY};
}

std::optional<NativePointMapper> MakeNativePointMapper(int canvasWidth, int canvasHeight, float displayWidth,
    float displayHeight, const GameImagePlacement& image) noexcept
{
    if (canvasWidth <= 0 || canvasHeight <= 0 || !std::isfinite(displayWidth) || !std::isfinite(displayHeight) ||
        displayWidth <= 0.0f || displayHeight <= 0.0f)
        return std::nullopt;
    NativePointMapper mapper;
    mapper.mCanvasWidth = static_cast<float>(canvasWidth);
    mapper.mCanvasHeight = static_cast<float>(canvasHeight);
    const bool hasImageRect = IsUsableImageRect(image.mRect, displayWidth, displayHeight);
    if (image.mIsImageRect && hasImageRect)
    {
        mapper.mImage = image.mRect;
        mapper.mIsExact = true;
        return mapper;
    }
    // 图像区域未知：拉伸与居中等比留边两种解释（以及宿主的参考矩形）一致时才算确定，否则取最可能的一种。
    const UiRect stretched{0.0f, 0.0f, displayWidth, displayHeight};
    const float fitScale = std::min(displayWidth / mapper.mCanvasWidth, displayHeight / mapper.mCanvasHeight);
    const float fitWidth = mapper.mCanvasWidth * fitScale;
    const float fitHeight = mapper.mCanvasHeight * fitScale;
    const float fitLeft = (displayWidth - fitWidth) * 0.5f;
    const float fitTop = (displayHeight - fitHeight) * 0.5f;
    const UiRect fitted{fitLeft, fitTop, fitLeft + fitWidth, fitTop + fitHeight};
    mapper.mIsExact = IsSamePlacement(stretched, fitted) && (!hasImageRect || IsSamePlacement(stretched, image.mRect));
    mapper.mImage = hasImageRect ? image.mRect : stretched;
    return mapper;
}

RadarBarMetrics GetRadarBarMetrics(float scale) noexcept
{
    const float factor = std::isfinite(scale) ? std::max(1.0f, scale) : 1.0f;
    RadarBarMetrics metrics;
    metrics.mWidth = std::round(metrics.mWidth * factor);
    metrics.mTitleHeight = std::round(metrics.mTitleHeight * factor);
    metrics.mRowHeight = std::round(metrics.mRowHeight * factor);
    metrics.mPadding = std::max(4.0f, std::round(metrics.mPadding * factor));
    metrics.mHintHeight = std::round(metrics.mHintHeight * factor);
    metrics.mIconWidth = std::round(metrics.mIconWidth * factor);
    metrics.mIconHeight = std::round(metrics.mIconHeight * factor);
    metrics.mStripeWidth = std::max(2.0f, std::round(metrics.mStripeWidth * factor));
    return metrics;
}

namespace
{
// 提示栏竖直方向的可用范围与固定的上沿：上沿按“最多 5 条”的整块高度在 FPS 卡下沿与底栏上沿之间居中，
// 与条数、折叠、模式无关。
struct RadarBarSpan
{
    float mTop = 0.0f;
    float mAreaBottom = 0.0f;
    float mAreaHeight = 0.0f;
};

RadarBarSpan GetRadarBarSpan(const RadarBarLayoutInput& input, float displayHeight, float scale,
                             const RadarBarMetrics& metrics) noexcept
{
    const float gap = std::round(HUD_EDGE_GAP * scale);
    const float footerTop = std::isfinite(input.mFooterTop) ? std::clamp(input.mFooterTop, 1.0f, displayHeight)
                                                            : displayHeight;
    const float areaTop = gap + std::round(FPS_CARD_HEIGHT * scale) + gap;
    RadarBarSpan span;
    span.mAreaBottom = footerTop - gap;
    span.mAreaHeight = std::max(0.0f, span.mAreaBottom - areaTop);
    const float fullBlock = metrics.mTitleHeight +
        static_cast<float>(viewer::RADAR_EVENT_RECENT_LIMIT) * metrics.mRowHeight + metrics.mPadding;
    span.mTop = areaTop + std::floor(std::max(0.0f, span.mAreaHeight - fullBlock) * 0.5f);
    return span;
}
}

std::size_t GetRadarBarRowCapacity(const RadarBarLayoutInput& input) noexcept
{
    const float displayHeight = std::isfinite(input.mDisplayHeight) ? std::max(1.0f, input.mDisplayHeight) : 1.0f;
    const float scale = std::isfinite(input.mScale) ? std::max(1.0f, input.mScale) : 1.0f;
    const RadarBarMetrics metrics = GetRadarBarMetrics(scale);
    const RadarBarSpan span = GetRadarBarSpan(input, displayHeight, scale, metrics);
    if (span.mAreaHeight < metrics.mTitleHeight)
        return 0;
    const float room = span.mAreaBottom - span.mTop - metrics.mTitleHeight - metrics.mPadding;
    if (room < metrics.mRowHeight)
        return 0;
    return std::min(viewer::RADAR_EVENT_RECENT_LIMIT, static_cast<std::size_t>(std::floor(room / metrics.mRowHeight)));
}

UiRect GetRadarBarRect(const RadarBarLayoutInput& input) noexcept
{
    const float displayWidth = std::isfinite(input.mDisplayWidth) ? std::max(1.0f, input.mDisplayWidth) : 1.0f;
    const float displayHeight = std::isfinite(input.mDisplayHeight) ? std::max(1.0f, input.mDisplayHeight) : 1.0f;
    const float scale = std::isfinite(input.mScale) ? std::max(1.0f, input.mScale) : 1.0f;
    const RadarBarMetrics metrics = GetRadarBarMetrics(scale);
    const float gap = std::round(HUD_EDGE_GAP * scale);
    float mapRight = displayWidth;
    if (input.mPanelLeft && std::isfinite(*input.mPanelLeft))
        mapRight = std::clamp(*input.mPanelLeft, 1.0f, displayWidth);

    // 宽度不超过地图区（两侧各留一个外边距）；地图区极窄时至少保留 1 像素。
    const float width = std::max(1.0f, std::min(metrics.mWidth, mapRight - gap * 2.0f));
    const float left = std::min(gap, std::max(0.0f, displayWidth - width));

    const RadarBarSpan span = GetRadarBarSpan(input, displayHeight, scale, metrics);
    float top = span.mTop;
    float height = metrics.mTitleHeight;
    if (!input.mIsCollapsed)
    {
        if (input.mShowHistory)
            height += static_cast<float>(RADAR_BAR_HISTORY_ROWS) * metrics.mRowHeight + metrics.mPadding;
        else if (input.mRowCount == 0)
            height += metrics.mHintHeight;
        else
            height += static_cast<float>(std::min(input.mRowCount, viewer::RADAR_EVENT_RECENT_LIMIT)) *
                      metrics.mRowHeight + metrics.mPadding;
    }
    // 放不下时收到可用高度，但至少保留标题行；可用空间连标题行都放不下（极小画面）时只保证不越出画面。
    // 最近模式的条数应先按 GetRadarBarRowCapacity 截断，这样收高度只裁掉提示行或历史列表的底部。
    if (span.mAreaHeight >= metrics.mTitleHeight)
        height = std::min(height, span.mAreaBottom - top);
    top = std::clamp(top, 0.0f, std::max(0.0f, displayHeight - metrics.mTitleHeight));
    height = std::min(height, displayHeight - top);
    return {left, top, left + width, top + std::max(1.0f, height)};
}

UiRect ClampRectToDisplay(const UiRect& rect, float displayWidth, float displayHeight) noexcept
{
    const float width = rect.mRight - rect.mLeft;
    const float height = rect.mBottom - rect.mTop;
    const float left = std::clamp(rect.mLeft, 0.0f, std::max(0.0f, displayWidth - width));
    const float top = std::clamp(rect.mTop, 0.0f, std::max(0.0f, displayHeight - height));
    return {left, top, left + width, top + height};
}

UiRect GetEconomyWindowRect(const EconomyWindowLayoutInput& input, std::optional<UiPoint> position) noexcept
{
    const float displayWidth = std::isfinite(input.mDisplayWidth) ? std::max(1.0f, input.mDisplayWidth) : 1.0f;
    const float displayHeight = std::isfinite(input.mDisplayHeight) ? std::max(1.0f, input.mDisplayHeight) : 1.0f;
    const float scale = std::isfinite(input.mScale) ? std::max(1.0f, input.mScale) : 1.0f;
    float areaWidth = displayWidth;
    if (input.mPanelLeft && std::isfinite(*input.mPanelLeft))
        areaWidth = std::clamp(*input.mPanelLeft, 1.0f, displayWidth);
    float areaHeight = std::isfinite(input.mFooterTop) ? std::clamp(input.mFooterTop, 1.0f, displayHeight) : displayHeight;

    const auto fit = [](float area, float minimum) {
        return std::floor(std::min(area, std::max(area * ECONOMY_WINDOW_AREA_RATIO, minimum)));
    };
    const float width = std::max(1.0f, fit(areaWidth, std::round(ECONOMY_WINDOW_MIN_WIDTH * scale)));
    const float height = std::max(1.0f, fit(areaHeight, std::round(ECONOMY_WINDOW_MIN_HEIGHT * scale)));
    UiRect rect{0.0f, 0.0f, width, height};
    if (position)
    {
        rect = {std::floor(position->mX), std::floor(position->mY), std::floor(position->mX) + width,
                std::floor(position->mY) + height};
    }
    else
    {
        const float left = std::floor((areaWidth - width) * 0.5f);
        const float top = std::floor((areaHeight - height) * 0.5f);
        rect = {left, top, left + width, top + height};
    }
    return ClampRectToDisplay(rect, displayWidth, displayHeight);
}

UiRect GetFooterRect(const FooterLayoutInput& input) noexcept
{
    const float width = std::isfinite(input.mDisplayWidth) ? std::max(0.0f, input.mDisplayWidth) : 0.0f;
    const float height = std::isfinite(input.mDisplayHeight) ? std::max(0.0f, input.mDisplayHeight) : 0.0f;
    float right = width;
    if (input.mPanelLeft && std::isfinite(*input.mPanelLeft))
        right = std::clamp(std::floor(*input.mPanelLeft), 0.0f, width);
    float footerHeight = std::isfinite(input.mContentHeight) ? std::max(0.0f, input.mContentHeight) : 0.0f;
    // 命令栏可能不贴画面下缘（例如上下留边）；底栏贴下缘，所以需要覆盖到命令栏上沿为止。
    if (input.mCommandBar && std::isfinite(input.mCommandBar->mTop) &&
        input.mCommandBar->mBottom > input.mCommandBar->mTop)
        footerHeight = std::max(footerHeight, height - input.mCommandBar->mTop);
    footerHeight = std::clamp(std::ceil(footerHeight), 0.0f, height);
    return {0.0f, height - footerHeight, right, height};
}
}
