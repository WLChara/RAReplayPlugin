#pragma once

#include "ReplayHud.h"
#include "../viewer/RadarDisplayRules.h"

#include <cstddef>
#include <optional>

namespace rareplay::ui
{
/** 100% 档下 HUD 贴边的外边距与左上 FPS 卡的高度；其余浮动面板据此避让。 */
constexpr float HUD_EDGE_GAP = 14.0f;
constexpr float FPS_CARD_HEIGHT = 180.0f;

/** 换算不唯一时如何取保守矩形。 */
enum class NativeRectPolicy
{
    /** 上沿取各解释中最低者，避免向上盖住原生内容（侧栏面板与雷达）。 */
    AvoidAbove,
    /** 上沿取各解释中最高者，保证完整盖住该原生区域（底部命令栏）。 */
    CoverAll
};

/** 游戏画布矩形换算到客户区后的结果。 */
struct NativeRectPlacement
{
    /** 主视口客户区坐标，已向外取整并夹在显示区域内。 */
    UiRect mRect;
    /**
     * true：换算唯一确定（宿主确认的图像区域，或拉伸与等比留边两种解释一致）。
     * false：两种解释不一致时取的保守矩形——上沿取最低以避开雷达，其余边取并集；
     * 它不保证与原生区域重合，不能据此停止原生侧栏绘制。
     */
    bool mIsExact = false;
};

/** 画布矩形无效或不在画布内、显示尺寸无效时返回空。左、右、下沿在两种策略下都取各解释的并集。 */
std::optional<NativeRectPlacement> MapNativeRect(const viewer::NativeUiLayout& layout,
    const viewer::NativeRect& rect, float displayWidth, float displayHeight,
    const GameImagePlacement& image, NativeRectPolicy policy = NativeRectPolicy::AvoidAbove) noexcept;

/**
 * 回放面板在客户区的位置：原生 SidebarBounds 换算后，右缘、上沿和下沿不变，宽度乘以 HUD 缩放并向左延伸，
 * 盖住地图右侧一条（左缘向下取整、不小于 0）。上沿仍是雷达块下沿，雷达块与资金条保持原生宽度。
 * 布局缺失、换算失败或原生宽高小于可排版的最小尺寸时返回空，此时不显示面板，原生侧栏照常。
 * scale 小于 1 或非有限时按 1 处理；加宽不改变 mIsExact，因为加宽后的面板仍完整包含原生侧栏区。
 */
std::optional<NativeRectPlacement> GetSidebarPanelPlacement(
    const std::optional<viewer::NativeUiLayout>& layout, float displayWidth, float displayHeight,
    const GameImagePlacement& image, float scale = 1.0f) noexcept;

/** 原生底部命令栏在客户区的矩形；布局或命令栏几何缺失时为空。换算不唯一时上沿取最高，保证底栏能盖住它。 */
std::optional<UiRect> GetCommandBarClientRect(const std::optional<viewer::NativeUiLayout>& layout,
    float displayWidth, float displayHeight, const GameImagePlacement& image) noexcept;

/**
 * 部队页每行卡片数：最多两列；可用宽度放不下两张最窄卡片（各 2.5 个字号宽，加一个间距）时为一列。
 * 100% 档、原生 168 像素宽的面板（内容约 156 像素、字号 18）为两列。
 */
int GetUnitCardColumns(float availableWidth, float fontSize, float spacing) noexcept;

/** 底栏布局输入，均为主视口客户区坐标。 */
struct FooterLayoutInput
{
    float mDisplayWidth = 0.0f;
    float mDisplayHeight = 0.0f;
    /** 底栏自身内容需要的高度，已含 HUD 缩放。 */
    float mContentHeight = 0.0f;
    /** 侧栏面板左缘；面板未显示时为空。 */
    std::optional<float> mPanelLeft;
    /** 原生底部命令栏；未知时为空。 */
    std::optional<UiRect> mCommandBar;
};

/**
 * 底栏贴画面左缘与下缘、无外边距：右缘到侧栏面板左缘，面板未显示时到画面右缘。
 * 高度取内容高度与“画面下缘到命令栏上沿”中的较大者，从而完整盖住原生命令栏；结果取整并夹在画面内。
 */
UiRect GetFooterRect(const FooterLayoutInput& input) noexcept;

struct UiPoint
{
    float mX = 0.0f;
    float mY = 0.0f;
};

/**
 * 游戏画布坐标到客户区坐标的线性映射，供画布内的点（超武落点）使用：客户区 = 图像原点 + 画布坐标 × 图像尺寸 / 画布尺寸，
 * 非等比拉伸时两个轴各自缩放。规则与 MapNativeRect 的“宿主确认的图像区域”一致。
 */
struct NativePointMapper
{
    /** 游戏图像在客户区中的矩形。 */
    UiRect mImage;
    float mCanvasWidth = 1.0f;
    float mCanvasHeight = 1.0f;
    /** 图像位置是否确定（宿主确认，或拉伸与等比留边两种解释一致）；不确定时只是最可能的解释。 */
    bool mIsExact = false;

    UiPoint MapPoint(float canvasX, float canvasY) const noexcept;
    UiRect MapRect(const viewer::NativeRect& rect) const noexcept;
};

/**
 * 画布或显示尺寸无效时返回空。图像位置不确定时（例如 cnc-ddraw D3D9 的顶点留边）优先取宿主给出的参考矩形，
 * 没有参考时按整块拉伸（cnc-ddraw 默认 maintas=false）；此时 mIsExact 为假。
 */
std::optional<NativePointMapper> MakeNativePointMapper(int canvasWidth, int canvasHeight, float displayWidth,
    float displayHeight, const GameImagePlacement& image) noexcept;

/**
 * 雷达事件提示栏的几何，均按 HUD 缩放换算成整数像素（100% 档：宽 260、标题行 30、每条 44、内边距 6）。
 */
struct RadarBarMetrics
{
    float mWidth = 260.0f;
    float mTitleHeight = 30.0f;
    float mRowHeight = 44.0f;
    float mPadding = 6.0f;
    /** 没有事件时的提示行高度。 */
    float mHintHeight = 26.0f;
    /** 事件图标框，与原生 cameo 同为 5:4（60×48 的一半）。 */
    float mIconWidth = 30.0f;
    float mIconHeight = 24.0f;
    /** 左侧玩家颜色条的宽度。 */
    float mStripeWidth = 3.0f;
};
RadarBarMetrics GetRadarBarMetrics(float scale) noexcept;

/** “历史”模式一屏可见的条数（其余在列表内滚动）。 */
constexpr int RADAR_BAR_HISTORY_ROWS = 10;

/** 提示栏布局输入，均为主视口客户区坐标。 */
struct RadarBarLayoutInput
{
    float mDisplayWidth = 0.0f;
    float mDisplayHeight = 0.0f;
    /** 侧栏面板左缘；面板未显示时为空，地图区一直到画面右缘。 */
    std::optional<float> mPanelLeft;
    /** 底栏上沿；提示栏在它之上。 */
    float mFooterTop = 0.0f;
    float mScale = 1.0f;
    /** 折叠后只占标题行。 */
    bool mIsCollapsed = false;
    /** 历史模式：标题行下是可滚动的全部历史，高度固定为 RADAR_BAR_HISTORY_ROWS 条（放不下时收到可用高度）。 */
    bool mShowHistory = false;
    /** 最近模式当前显示的条数（0 到 5）；0 时显示一行提示。 */
    std::size_t mRowCount = 0;
};

/**
 * 提示栏矩形：贴画面左缘（HUD_EDGE_GAP），宽 260（夹在地图区内），竖直方向按“最多 5 条”的整块高度
 * 在 FPS 卡下沿与底栏上沿之间居中——上沿与条数无关，标题行不会随事件增减跳动。
 * 高度随模式变化，空间足够时始终不盖住 FPS 卡与底栏；结果夹在画面内，整数像素。
 */
UiRect GetRadarBarRect(const RadarBarLayoutInput& input) noexcept;

/**
 * 最近模式在当前画面里放得下的条数（0 到 5）。画面小到 FPS 卡与底栏之间放不下 5 条时少于 5，
 * 调用方只显示最新的这几条，而不是让多出的行越出窗口。折叠与历史模式不使用它。
 */
std::size_t GetRadarBarRowCapacity(const RadarBarLayoutInput& input) noexcept;

/** 经济大窗口的布局输入，均为主视口客户区坐标。 */
struct EconomyWindowLayoutInput
{
    float mDisplayWidth = 0.0f;
    float mDisplayHeight = 0.0f;
    /** 侧栏面板左缘；面板未显示时为空，地图区一直到画面右缘。 */
    std::optional<float> mPanelLeft;
    /** 底栏上沿；地图区在它之上。 */
    float mFooterTop = 0.0f;
    /** HUD 缩放（1 为 100%），用于最小尺寸。 */
    float mScale = 1.0f;
};

/** 大窗口占地图区的比例。 */
constexpr float ECONOMY_WINDOW_AREA_RATIO = 0.75f;
/** 100% 档下大窗口的最小尺寸，保证图表与图例可读；地图区比它小时窗口不超出地图区。 */
constexpr float ECONOMY_WINDOW_MIN_WIDTH = 460.0f;
constexpr float ECONOMY_WINDOW_MIN_HEIGHT = 330.0f;

/**
 * 经济大窗口的矩形。地图区是画面除去右侧侧栏面板与底部底栏的部分；窗口大小为地图区的 75%，
 * 不小于按缩放换算的最小尺寸（但不超过地图区），整数像素。position 为空时居中放在地图区，
 * 否则以它为左上角，并夹在画面内——拖动后的位置不能出画面，画面变小时窗口也被推回画面内。
 */
UiRect GetEconomyWindowRect(const EconomyWindowLayoutInput& input, std::optional<UiPoint> position) noexcept;

/** 把宽高不变的矩形平移到画面内；矩形比画面大时贴左上。 */
UiRect ClampRectToDisplay(const UiRect& rect, float displayWidth, float displayHeight) noexcept;
}
