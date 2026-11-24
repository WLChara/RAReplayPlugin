#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/**
 * 经济曲线界面的纯逻辑：玩家显示筛选、时间范围截取、纵轴缩放、悬停取点、绘制抽稀和窗口的会话状态。
 * 只含值与标准库，不碰 ImGui 与引擎，便于离线测试；这些状态都是只在本次回放会话内保持的界面本地状态，
 * 不进入 viewer 快照，也不会发给游戏线程。
 */
namespace rareplay::ui
{
/** 大窗口曲线的时间范围；纵轴只按该范围内、可见玩家的数据缩放。 */
enum class EconomyTimeRange { All, LastTenMinutes, LastFiveMinutes };

/** 范围对应的跨度（毫秒）；“全部”为空。 */
std::optional<std::int64_t> GetEconomyRangeSpanMs(EconomyTimeRange range) noexcept;

/** 横轴的最小跨度：曲线没有点或只有一个点时坐标仍然有意义。 */
constexpr std::int64_t ECONOMY_MIN_SPAN_MS = 10'000;

/** 横轴窗口：起点与跨度，跨度不小于 ECONOMY_MIN_SPAN_MS。 */
struct EconomyTimeWindow
{
    std::int64_t mStartMs = 0;
    std::int64_t mSpanMs = ECONOMY_MIN_SPAN_MS;

    std::int64_t GetEndMs() const noexcept { return mStartMs + mSpanMs; }
};

struct EconomyTimeBounds
{
    std::int64_t mStartMs = 0;
    std::int64_t mEndMs = 0;
};

/** 所有曲线的数据时间范围（最早首点到最晚末点）；一个点也没有时为空。空指针的曲线被忽略。 */
std::optional<EconomyTimeBounds> GetEconomyTimeBounds(std::span<const viewer::EconomyCurve* const> curves) noexcept;

/**
 * 横轴窗口：“全部”取数据的全部时间；“最近 N 分钟”以数据末点为终点向前取 N 分钟，数据不足 N 分钟时从数据起点开始。
 * 没有数据（dataStartMs > dataEndMs 也按没有）时从 0 起取最小跨度。
 */
EconomyTimeWindow ResolveEconomyTimeWindow(EconomyTimeRange range, std::optional<EconomyTimeBounds> dataBounds) noexcept;

/**
 * 曲线落在 [startMs, endMs] 内的部分：下标区间 [mFirst, mEnd)；窗口边界两侧各有一个样本时，
 * mLeadIn / mLeadOut 是边界处线性插值得到的点，用来让折线从窗口边缘进入、离开，而不是从窗口外的点拉线。
 * 曲线点按时间升序。
 */
struct EconomySlice
{
    std::size_t mFirst = 0;
    std::size_t mEnd = 0;
    std::optional<viewer::EconomyPoint> mLeadIn;
    std::optional<viewer::EconomyPoint> mLeadOut;
};
EconomySlice SliceEconomyCurve(const viewer::EconomyCurve& curve, std::int64_t startMs, std::int64_t endMs) noexcept;

struct EconomyValueBounds
{
    std::int64_t mMin = 0;
    std::int64_t mMax = 0;
};

/** 这些曲线在窗口内（含边界插值点）的最小与最大资金；窗口内没有任何点时为空。空指针的曲线被忽略。 */
std::optional<EconomyValueBounds> GetEconomyValueBounds(std::span<const viewer::EconomyCurve* const> curves,
                                                        std::int64_t startMs, std::int64_t endMs) noexcept;

/** 纵轴：从 0 起（出现负值时向下延伸），刻度取整齐的 1、2、5 × 10^n，上沿向上取整到刻度。 */
struct EconomyYAxis
{
    std::int64_t mMin = 0;
    std::int64_t mMax = 1000;
    std::int64_t mStep = 500;
};

/**
 * 按数据范围自动缩放：上沿至少 1000（没有数据时也有可读的坐标），目标刻度数为 targetTicks（至少 1）。
 * 没有数据时返回只有默认上沿的坐标。
 */
EconomyYAxis ComputeEconomyYAxis(std::optional<EconomyValueBounds> bounds, int targetTicks) noexcept;

/** 时间最接近 timeMs 的点（距离相同取较早者）；曲线没有点时为空。 */
std::optional<viewer::EconomyPoint> FindNearestEconomyPoint(const viewer::EconomyCurve& curve,
                                                            std::int64_t timeMs) noexcept;

/**
 * 绘制前的抽稀：点数不超过 maxPoints（至少按 4 处理）时原样返回；否则把中间的点按下标均分成若干桶，
 * 每桶只保留最小值与最大值（按时间先后），并总是保留首尾点，所以尖峰不会被抽掉。结果仍按时间升序且不超过 maxPoints。
 */
std::vector<viewer::EconomyPoint> DecimateEconomyPoints(std::span<const viewer::EconomyPoint> points,
                                                        std::size_t maxPoints);

/**
 * 曲线颜色：感知亮度（0.299 R + 0.587 G + 0.114 B）低于 0.35 的玩家色向白色混合到 0.35，
 * 使过暗的颜色在深色图表背景上仍可辨认；足够亮的颜色保持不变。rgb 与 alpha 无关，为 0xRRGGBB。
 */
std::uint32_t EnsureChartLineColor(std::uint32_t rgb) noexcept;

/**
 * 玩家显示筛选。内部只记录被隐藏的房屋下标，所以后来出现的玩家默认可见。
 * 所有需要“当前有哪些玩家”的操作都由调用方传入候选列表（有资金曲线的玩家的房屋下标，互不重复）。
 *
 * 取舍：至少保留一位可见玩家。取消勾选最后一位可见玩家被拒绝（SetVisible 返回 false，界面上该勾选框置灰），
 * 而不是自动回到全部显示——这样勾选框的状态始终与曲线一致，不会出现点了却“全部又亮起来”的跳变。
 * 候选列表变化导致没有任何可见者时（例如被隐藏的玩家集合恰好覆盖了当前全部玩家），Normalize 恢复全部显示。
 */
class EconomyPlayerFilter final
{
public:
    void Reset() noexcept { mHidden.clear(); }

    bool IsVisible(int houseIndex) const noexcept;
    void ShowAll() noexcept { mHidden.clear(); }

    /** 只显示 houseIndex；它不在候选列表中时无效，改为显示全部。 */
    void ShowOnly(int houseIndex, std::span<const int> candidates);

    /** 设置一位玩家是否显示。隐藏会使候选中没有任何可见者时拒绝并返回 false，状态不变。 */
    bool SetVisible(int houseIndex, bool isVisible, std::span<const int> candidates);

    std::size_t CountVisible(std::span<const int> candidates) const noexcept;
    std::size_t CountHidden(std::span<const int> candidates) const noexcept;

    /** 候选不止一位且恰好一位可见时返回它。 */
    std::optional<int> GetOnlyVisible(std::span<const int> candidates) const noexcept;

    /** 候选非空而没有任何可见者时恢复显示全部。 */
    void Normalize(std::span<const int> candidates) noexcept;

private:
    // 升序、无重复。
    std::vector<int> mHidden;
};

/** 经济大窗口的位置与拖动状态；位置未确定时（mHasPosition 为假）窗口居中放在地图区。 */
struct EconomyWindowState
{
    bool mIsOpen = false;
    bool mHasPosition = false;
    float mLeft = 0.0f;
    float mTop = 0.0f;
    bool mIsDragging = false;
    /** 拖动开始时鼠标相对窗口左上角的偏移。 */
    float mDragOffsetX = 0.0f;
    float mDragOffsetY = 0.0f;
};

/** 侧栏小图与大窗口共用的界面本地状态，会话变化或 Reset 时清空。 */
struct EconomyUiState
{
    EconomyPlayerFilter mFilter;
    EconomyTimeRange mRange = EconomyTimeRange::All;
    EconomyWindowState mWindow;
};

/** 图例放在图表右侧还是下方：右侧放得下图例，且图表仍不窄于 minPlotWidth 时放右侧。 */
bool ShouldPlaceEconomyLegendBeside(float contentWidth, float legendWidth, float minPlotWidth, float gap) noexcept;
}
