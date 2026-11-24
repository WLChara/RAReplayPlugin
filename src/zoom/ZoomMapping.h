#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rareplay::zoom
{
/**
 * 物理尺寸与逻辑尺寸每轴的上限（像素）。取 16 位有符号上限 32767：整面面积小于 2^30，
 * 2 字节像素的整面字节数小于 2^31，都在 32 位有符号整数内；区间与坐标的 64 位中间量远离溢出。
 * 实际的战术区与逻辑视口（最坏约 7176×4224）远小于此。
 */
constexpr int MAX_ZOOM_EXTENT = 32767;

/**
 * 向负无穷取整的整数除法（C++ 的 / 向零截断，负数结果会差 1）。
 *
 * @param denominator 必须为正。
 */
constexpr std::int64_t FloorDivide(std::int64_t numerator, std::int64_t denominator) noexcept
{
    const std::int64_t quotient = numerator / denominator;
    return (numerator % denominator != 0 && numerator < 0) ? quotient - 1 : quotient;
}

/** 二维整数点；单位由使用处说明（物理像素、逻辑像素或等距像素）。 */
struct ZoomPoint
{
    int mX = 0;
    int mY = 0;

    friend constexpr bool operator==(const ZoomPoint&, const ZoomPoint&) = default;
};

/** 一个物理像素显示的逻辑像素半开区间 [mBegin, mEnd)，单位为逻辑像素；总有 mEnd > mBegin。 */
struct SourceSpan
{
    int mBegin = 0;
    int mEnd = 1;

    friend constexpr bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

/**
 * 单轴：物理像素 p 显示的逻辑源区间。
 * s0 = floor(p·L/P)，s1 = max(s0+1, floor((p+1)·L/P))，返回 [s0, s1)。
 * 缩小（L > P）时相邻物理像素的区间首尾相接；放大（L ≤ P）时区间长度恒为 1，相邻物理像素可共用同一区间。
 * p 取遍 [0, P) 时区间并集恰为 [0, L)。
 *
 * @param physicalPixel 相对物理战术区起点的物理像素；可在 [0, P) 之外（输入外推），公式不变。
 * @param physicalSize P，前置条件 1..MAX_ZOOM_EXTENT。
 * @param logicalSize L，前置条件 1..MAX_ZOOM_EXTENT。
 * @return 尺寸不合法时按恒等映射返回 [p, p+1)（防御路径，调用方应先用 ZoomTransform::IsValid 检查）；
 *         结果超出 int 时饱和到 int 边界并保持 mEnd > mBegin（只在 |p| 远超任何屏幕坐标时发生）。
 */
SourceSpan SpanForPhysicalPixel(int physicalPixel, int physicalSize, int logicalSize) noexcept;

/**
 * 单轴：物理点到逻辑点，取区间的整数中点 s0 + (s1 − s0 − 1) / 2，必定落在该物理像素显示的逻辑像素内。
 * L == P 时恒等。前置条件与越界行为同 SpanForPhysicalPixel。
 */
int MapPhysicalToLogical(int physicalPixel, int physicalSize, int logicalSize) noexcept;

/**
 * 单轴：逻辑像素 x 到物理像素，只用于叠加图标定位。返回显示 x 的第一个物理像素，
 * 即 min(ceil(x·P/L), ceil((x+1)·P/L) − 1)：它的源区间必定包含 x，且与连续值 x·P/L 相差小于 1 个物理像素。
 * L == P 时恒等；x 可为任意整数（屏幕外的图标按同一公式外推）。
 * 前置条件同 SpanForPhysicalPixel；尺寸不合法时恒等返回 x，结果超出 int 时饱和。
 */
int MapLogicalToPhysical(int logicalPixel, int physicalSize, int logicalSize) noexcept;

/**
 * 物理战术区 P 与逻辑视口 L 的二维映射，各轴独立。尺寸单位为像素，宽高都以战术区左上角为原点。
 * 纯值类型，不持有任何资源。
 */
struct ZoomTransform
{
    int mPhysicalWidth = 0;
    int mPhysicalHeight = 0;
    int mLogicalWidth = 0;
    int mLogicalHeight = 0;

    /** 四个尺寸都在 1..MAX_ZOOM_EXTENT 内。 */
    bool IsValid() const noexcept;
    /** 合法且每轴 L == P；此时两个方向的映射都恒等。 */
    bool IsIdentity() const noexcept;
    /** 各轴调用 zoom::MapPhysicalToLogical；契约同该函数。 */
    ZoomPoint MapPhysicalToLogical(ZoomPoint physicalPoint) const noexcept;
    /** 各轴调用 zoom::MapLogicalToPhysical；契约同该函数。 */
    ZoomPoint MapLogicalToPhysical(ZoomPoint logicalPoint) const noexcept;

    friend constexpr bool operator==(const ZoomTransform&, const ZoomTransform&) = default;
};

/**
 * 单轴区间表：[0, P) 每个物理像素的源区间，供缩放核使用。只能由 BuildSpanTable 构造，
 * 因此表内每个区间都满足 0 ≤ mBegin < mEnd ≤ L，缩放核据此免去逐像素越界检查。
 */
class SpanTable final
{
public:
    SpanTable() = default;

    int GetPhysicalSize() const noexcept { return mPhysicalSize; }
    int GetLogicalSize() const noexcept { return mLogicalSize; }
    /** 表内最长区间的长度；空表为 0。 */
    int GetMaxSpanLength() const noexcept { return mMaxSpanLength; }
    /** 表按给定尺寸构建且非空。 */
    bool IsBuiltFor(int physicalSize, int logicalSize) const noexcept;
    /** 前置条件 0 ≤ physicalPixel < GetPhysicalSize()。 */
    const SourceSpan& operator[](int physicalPixel) const noexcept { return mSpans[static_cast<std::size_t>(physicalPixel)]; }
    const std::vector<SourceSpan>& GetSpans() const noexcept { return mSpans; }

    friend SpanTable BuildSpanTable(int physicalSize, int logicalSize);

private:
    int mPhysicalSize = 0;
    int mLogicalSize = 0;
    int mMaxSpanLength = 0;
    std::vector<SourceSpan> mSpans;
};

/**
 * 预计算 [0, P) 每个物理像素的区间（内容与逐个调用 SpanForPhysicalPixel 相同）。
 * 只在 (P, L) 变化时调用：会分配 P 个元素，可能抛出 std::bad_alloc。
 *
 * @return 尺寸不在 1..MAX_ZOOM_EXTENT 内时返回空表（IsBuiltFor 恒为假）。
 */
SpanTable BuildSpanTable(int physicalSize, int logicalSize);
}
