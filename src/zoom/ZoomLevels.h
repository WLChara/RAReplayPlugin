#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace rareplay::zoom
{
/** 缩放倍数 z = 物理/逻辑 = mNumerator/mDenominator；z > 1 放大（L < P），z < 1 缩小（L > P）。 */
struct ZoomLevel
{
    int mNumerator = 1;
    int mDenominator = 1;

    friend constexpr bool operator==(const ZoomLevel&, const ZoomLevel&) = default;
};

/** 档位表，从最大放大到最远缩小排列；每格滚轮切换一档。 */
constexpr std::array<ZoomLevel, 12> ZOOM_LEVELS{{
    {4, 1}, {3, 1}, {5, 2}, {2, 1}, {3, 2}, {5, 4},
    {1, 1},
    {4, 5}, {2, 3}, {1, 2}, {2, 5}, {1, 3},
}};
/** 最大放大档 4×；放大上限固定，不受内存预算影响。 */
constexpr int MOST_MAGNIFIED_ZOOM_INDEX = 0;
/** 1× 档：默认档与“复原”目标。 */
constexpr int IDENTITY_ZOOM_INDEX = 6;
/** 表内最远的缩小档 1/3。 */
constexpr int FARTHEST_SHRINK_ZOOM_INDEX = static_cast<int>(ZOOM_LEVELS.size()) - 1;
static_assert(ZOOM_LEVELS[IDENTITY_ZOOM_INDEX] == ZoomLevel{1, 1});

/**
 * 非 1× 档的逻辑尺寸每轴下限（像素）。64 至少容纳一个等距格的宽度（60 像素）且为偶数；
 * 只在战术区小于约 256 像素时才会生效，游戏的最低分辨率 640×480 不会触发。
 */
constexpr int MIN_LOGICAL_EXTENT = 64;
/**
 * 逻辑资源的默认内存预算：128 MiB。gamemd.exe 没有大地址感知标志，用户态只有 2 GB 地址空间，
 * 逻辑 Composite/Tile 的系统内存面与 Z/A 缓冲都占用其中。按此预算 1920×1080 窗口最远到 1/3，2560×1440 最远到 1/2。
 */
constexpr std::uint64_t DEFAULT_ZOOM_MEMORY_BUDGET_BYTES = 128ull * 1024 * 1024;

/** 逻辑视口尺寸（像素）；{0, 0} 表示无法计算。 */
struct LogicalSize
{
    int mWidth = 0;
    int mHeight = 0;

    friend constexpr bool operator==(const LogicalSize&, const LogicalSize&) = default;
};

/**
 * 某档的逻辑尺寸：各轴 L = P·den/num，四舍五入到最近的偶数（恰在两个偶数正中时取较大者），再不小于 MIN_LOGICAL_EXTENT。
 * 偶数保证 L/2 是整数，锚点换算（ZoomAnchor）与镜头中心才能精确对应。
 * 例外：1×（num == den）时每轴精确等于 P，P 为奇数或小于下限也保持 P，因为 1× 必须与原版逐位相同。
 * 结果可能超过 MAX_ZOOM_EXTENT，由调用方用 ZoomTransform::IsValid 判定能否使用。
 *
 * @param physicalWidth、physicalHeight 物理战术区尺寸，前置条件 1..MAX_ZOOM_EXTENT。
 * @param level 前置条件 mNumerator、mDenominator 都为正。
 * @return 前置条件不满足时返回 {0, 0}。
 */
LogicalSize ComputeLogicalSize(int physicalWidth, int physicalHeight, ZoomLevel level) noexcept;

/**
 * 逻辑资源的内存估算（字节），每像素 2 字节（16 位表面）：
 * Composite 与 Tile 按最远档一次分配，Z 与 A 按当前档，即 2 × (farthest 面积 × 2) + 2 × (current 面积 × 2)。
 * 全程 64 位无符号运算。
 *
 * @return 尺寸为负或结果超出 64 位时返回 std::nullopt。
 */
std::optional<std::uint64_t> EstimateLogicalResourceBytes(LogicalSize current, LogicalSize farthest) noexcept;

/**
 * 某个物理战术区尺寸下允许的档位范围。纯值对象，P 变化时重新构造；只做计算，不持有资源。
 *
 * 最远缩小档从配置值向 1× 方向逐档后退，直到该档的四块缓冲都按该档计算（EstimateLogicalResourceBytes(L, L)）
 * 不超过预算，且逻辑尺寸合法；一档都不满足时只能停在 1×。离最远档更近 1× 的档位逻辑尺寸更小，因此同样满足预算。
 * 放大上限固定为 4×；仅当物理尺寸任一轴小于 MIN_LOGICAL_EXTENT 时不提供放大（否则下限会让“放大档”实际缩小）。
 * 物理尺寸不合法时只允许 1×。
 */
class ZoomLevelPolicy final
{
public:
    /**
     * @param memoryBudgetBytes 逻辑资源预算（字节）。
     * @param configuredFarthestShrinkIndex 配置允许的最远缩小档在 ZOOM_LEVELS 中的下标；
     *        小于 IDENTITY_ZOOM_INDEX 按 1× 处理，大于 FARTHEST_SHRINK_ZOOM_INDEX 按 1/3 处理。
     */
    ZoomLevelPolicy(int physicalWidth, int physicalHeight,
        std::uint64_t memoryBudgetBytes = DEFAULT_ZOOM_MEMORY_BUDGET_BYTES,
        int configuredFarthestShrinkIndex = FARTHEST_SHRINK_ZOOM_INDEX) noexcept;

    /** 允许的最大放大档下标（通常为 MOST_MAGNIFIED_ZOOM_INDEX）。 */
    int GetMostMagnifiedIndex() const noexcept { return mMostMagnifiedIndex; }
    /** 实际允许的最远缩小档下标，范围 IDENTITY_ZOOM_INDEX..FARTHEST_SHRINK_ZOOM_INDEX。 */
    int GetFarthestShrinkIndex() const noexcept { return mFarthestShrinkIndex; }

    /**
     * 从 currentIndex 移动 steps 档：正数向放大方向（下标减小），负数向缩小方向，结果夹在允许范围内。
     * currentIndex 超出允许范围时先夹到范围内；steps 为任意 int 都不会溢出。
     */
    int Step(int currentIndex, int steps) const noexcept;
    /** 复原目标：1× 档。 */
    static constexpr int ResetIndex() noexcept { return IDENTITY_ZOOM_INDEX; }

    /** 某档在本物理尺寸下的逻辑尺寸；index 先夹到允许范围内。物理尺寸不合法时返回 {0, 0}。 */
    LogicalSize GetLogicalSize(int index) const noexcept;

private:
    int ClampIndex(int index) const noexcept;

    int mPhysicalWidth = 0;
    int mPhysicalHeight = 0;
    int mMostMagnifiedIndex = IDENTITY_ZOOM_INDEX;
    int mFarthestShrinkIndex = IDENTITY_ZOOM_INDEX;
};
}
