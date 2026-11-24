#include "ScalePixels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace rareplay::zoom
{
namespace
{
/**
 * 打包累加可容纳的最大区间盒像素数。打包把三个通道放进一个 32 位数的三条互不进位的“通道带”，
 * 带宽见各格式的注释；32 个像素的通道和恰好不溢出最窄的带。全部档位的盒子不超过 3×3，都走打包路径。
 */
constexpr std::uint32_t MAX_PACKED_BOX = 32;
/** 打包路径用乘法代替除法：q = x · ceil(2^20 / n) >> 20。 */
constexpr int RECIPROCAL_SHIFT = 20;
/** 绿色在打包值中的起点，两种格式相同。 */
constexpr int GREEN_LANE_SHIFT = 21;
/** 绿色在像素中的起点，两种格式相同。 */
constexpr int GREEN_PIXEL_SHIFT = 5;

// x = sum + n/2 不超过 63·32 + 16 < 2^11，误差项 ceil(2^20/n)·n − 2^20 < n ≤ 32，
// 二者之积小于 2^16 ≤ 2^20，所以对全部可能的 x 都与 x / n 精确相等（离线测试穷举验证）。
constexpr std::array<std::uint32_t, MAX_PACKED_BOX + 1> RECIPROCALS = []
{
    std::array<std::uint32_t, MAX_PACKED_BOX + 1> values{};
    for (std::uint32_t count = 1; count <= MAX_PACKED_BOX; ++count)
        values[count] = ((1u << RECIPROCAL_SHIFT) + count - 1) / count;
    return values;
}();

template <PixelFormat16 FORMAT>
struct FormatTraits;

// 565：B 0–4、G 5–10、R 11–15。打包后 B 带 0–10（11 位）、R 带 11–20（10 位）、G 带 21–31（11 位）；
// R 带最窄：31 × 32 < 2^10。
template <>
struct FormatTraits<PixelFormat16::Rgb565>
{
    static constexpr std::uint32_t SPREAD_MASK = 0x07E0F81Fu;
    static constexpr std::uint32_t BLUE_LANE_MASK = 0x7FFu;
    static constexpr std::uint32_t RED_LANE_MASK = 0x3FFu;
    static constexpr int RED_SHIFT = 11;
    static constexpr std::uint32_t GREEN_MAX = 63;
    static constexpr std::uint16_t PIXEL_MASK = 0xFFFFu;
};

// 555：最高位丢弃，B 0–4、G 5–9、R 10–14。打包后 B 带 0–9（10 位）、R 带 10–20（11 位）、G 带 21–31（11 位）；
// B 带最窄：31 × 32 < 2^10。
template <>
struct FormatTraits<PixelFormat16::Rgb555>
{
    static constexpr std::uint32_t SPREAD_MASK = 0x03E07C1Fu;
    static constexpr std::uint32_t BLUE_LANE_MASK = 0x3FFu;
    static constexpr std::uint32_t RED_LANE_MASK = 0x7FFu;
    static constexpr int RED_SHIFT = 10;
    static constexpr std::uint32_t GREEN_MAX = 31;
    static constexpr std::uint16_t PIXEL_MASK = 0x7FFFu;
};

constexpr std::uint32_t CHANNEL_MAX_5 = 31;

template <PixelFormat16 FORMAT>
std::uint32_t Spread(std::uint16_t pixel) noexcept
{
    // 低半字留下 B 与 R，高半字留下 G（相当于把 G 左移 16 位），三条带之间留出进位空间。
    return (pixel | (static_cast<std::uint32_t>(pixel) << 16)) & FormatTraits<FORMAT>::SPREAD_MASK;
}

template <PixelFormat16 FORMAT>
std::uint16_t FinalizePacked(std::uint32_t sum, std::uint32_t count) noexcept
{
    using Traits = FormatTraits<FORMAT>;
    const std::uint32_t reciprocal = RECIPROCALS[count];
    const std::uint32_t half = count / 2;
    const std::uint32_t blue = (((sum & Traits::BLUE_LANE_MASK) + half) * reciprocal) >> RECIPROCAL_SHIFT;
    const std::uint32_t red = ((((sum >> Traits::RED_SHIFT) & Traits::RED_LANE_MASK) + half) * reciprocal) >> RECIPROCAL_SHIFT;
    const std::uint32_t green = (((sum >> GREEN_LANE_SHIFT) + half) * reciprocal) >> RECIPROCAL_SHIFT;
    return static_cast<std::uint16_t>((red << Traits::RED_SHIFT) | (green << GREEN_PIXEL_SHIFT) | blue);
}

struct ScaleContext
{
    const std::uint8_t* mSource = nullptr;
    std::size_t mSourcePitch = 0;
    std::uint8_t* mTarget = nullptr;
    std::size_t mTargetPitch = 0;
    int mPhysicalWidth = 0;
    int mPhysicalHeight = 0;
    int mLogicalWidth = 0;
    const SpanTable* mColumns = nullptr;
    const SpanTable* mRows = nullptr;

    const std::uint16_t* SourceRow(int row) const noexcept
    {
        return reinterpret_cast<const std::uint16_t*>(mSource + static_cast<std::size_t>(row) * mSourcePitch);
    }

    std::uint16_t* TargetRow(int row) const noexcept
    {
        return reinterpret_cast<std::uint16_t*>(mTarget + static_cast<std::size_t>(row) * mTargetPitch);
    }

    // 行区间与上一目标行相同（放大时相邻物理行共用同一逻辑行）时结果必然相同，直接复制。
    bool TryRepeatPreviousRow(int row) const noexcept
    {
        if (row == 0 || !((*mRows)[row] == (*mRows)[row - 1]))
            return false;
        std::memcpy(TargetRow(row), TargetRow(row - 1), static_cast<std::size_t>(mPhysicalWidth) * sizeof(std::uint16_t));
        return true;
    }
};

// 两轴区间长度都为 1：逐像素取 s0，即最近邻。
template <PixelFormat16 FORMAT>
void ScaleNearest(const ScaleContext& context) noexcept
{
    const SourceSpan* columns = context.mColumns->GetSpans().data();
    for (int row = 0; row < context.mPhysicalHeight; ++row)
    {
        if (context.TryRepeatPreviousRow(row))
            continue;
        const std::uint16_t* source = context.SourceRow((*context.mRows)[row].mBegin);
        std::uint16_t* target = context.TargetRow(row);
        for (int column = 0; column < context.mPhysicalWidth; ++column)
            target[column] = static_cast<std::uint16_t>(source[columns[column].mBegin] & FormatTraits<FORMAT>::PIXEL_MASK);
    }
}

// 区间盒不超过 MAX_PACKED_BOX：纵向逐列累加打包值，再横向按列区间求和并取平均。
template <PixelFormat16 FORMAT>
void ScalePacked(const ScaleContext& context, std::uint32_t* sums) noexcept
{
    const SourceSpan* columns = context.mColumns->GetSpans().data();
    const int logicalWidth = context.mLogicalWidth;
    for (int row = 0; row < context.mPhysicalHeight; ++row)
    {
        if (context.TryRepeatPreviousRow(row))
            continue;
        const SourceSpan rowSpan = (*context.mRows)[row];
        const std::uint16_t* source = context.SourceRow(rowSpan.mBegin);
        for (int x = 0; x < logicalWidth; ++x)
            sums[x] = Spread<FORMAT>(source[x]);
        for (int sourceRow = rowSpan.mBegin + 1; sourceRow < rowSpan.mEnd; ++sourceRow)
        {
            source = context.SourceRow(sourceRow);
            for (int x = 0; x < logicalWidth; ++x)
                sums[x] += Spread<FORMAT>(source[x]);
        }
        const auto rowLength = static_cast<std::uint32_t>(rowSpan.mEnd - rowSpan.mBegin);
        std::uint16_t* target = context.TargetRow(row);
        for (int column = 0; column < context.mPhysicalWidth; ++column)
        {
            const SourceSpan span = columns[column];
            std::uint32_t sum = 0;
            for (int x = span.mBegin; x < span.mEnd; ++x)
                sum += sums[x];
            target[column] = FinalizePacked<FORMAT>(sum, static_cast<std::uint32_t>(span.mEnd - span.mBegin) * rowLength);
        }
    }
}

// 区间盒超过 MAX_PACKED_BOX（只在档位表以外的极端 (P, L) 出现）：各通道分开累加，纵向 32 位、横向 64 位。
// 纵向和不超过 32767 × 63，横向和不超过 32767² × 63，都不会溢出。
template <PixelFormat16 FORMAT>
void ScaleWide(const ScaleContext& context, std::uint32_t* sums) noexcept
{
    using Traits = FormatTraits<FORMAT>;
    const SourceSpan* columns = context.mColumns->GetSpans().data();
    const int logicalWidth = context.mLogicalWidth;
    for (int row = 0; row < context.mPhysicalHeight; ++row)
    {
        if (context.TryRepeatPreviousRow(row))
            continue;
        const SourceSpan rowSpan = (*context.mRows)[row];
        for (int sourceRow = rowSpan.mBegin; sourceRow < rowSpan.mEnd; ++sourceRow)
        {
            const std::uint16_t* source = context.SourceRow(sourceRow);
            const bool isFirst = sourceRow == rowSpan.mBegin;
            for (int x = 0; x < logicalWidth; ++x)
            {
                const std::uint32_t pixel = source[x];
                std::uint32_t* channels = sums + static_cast<std::size_t>(x) * 3;
                const std::uint32_t red = (pixel >> Traits::RED_SHIFT) & CHANNEL_MAX_5;
                const std::uint32_t green = (pixel >> GREEN_PIXEL_SHIFT) & Traits::GREEN_MAX;
                const std::uint32_t blue = pixel & CHANNEL_MAX_5;
                channels[0] = isFirst ? red : channels[0] + red;
                channels[1] = isFirst ? green : channels[1] + green;
                channels[2] = isFirst ? blue : channels[2] + blue;
            }
        }
        const auto rowLength = static_cast<std::uint64_t>(rowSpan.mEnd - rowSpan.mBegin);
        std::uint16_t* target = context.TargetRow(row);
        for (int column = 0; column < context.mPhysicalWidth; ++column)
        {
            const SourceSpan span = columns[column];
            std::uint64_t red = 0;
            std::uint64_t green = 0;
            std::uint64_t blue = 0;
            for (int x = span.mBegin; x < span.mEnd; ++x)
            {
                const std::uint32_t* channels = sums + static_cast<std::size_t>(x) * 3;
                red += channels[0];
                green += channels[1];
                blue += channels[2];
            }
            const std::uint64_t count = static_cast<std::uint64_t>(span.mEnd - span.mBegin) * rowLength;
            const std::uint64_t half = count / 2;
            const auto averageRed = static_cast<std::uint32_t>((red + half) / count);
            const auto averageGreen = static_cast<std::uint32_t>((green + half) / count);
            const auto averageBlue = static_cast<std::uint32_t>((blue + half) / count);
            target[column] = static_cast<std::uint16_t>(
                (averageRed << Traits::RED_SHIFT) | (averageGreen << GREEN_PIXEL_SHIFT) | averageBlue);
        }
    }
}

bool IsUsableView(const void* data, int width, int height, int pitchBytes, int requiredWidth, int requiredHeight) noexcept
{
    if (!data || reinterpret_cast<std::uintptr_t>(data) % alignof(std::uint16_t) != 0)
        return false;
    if (width < requiredWidth || height < requiredHeight || pitchBytes % 2 != 0 ||
        static_cast<std::int64_t>(pitchBytes) < static_cast<std::int64_t>(width) * 2)
        return false;
    // 整个视图的字节跨度必须能用指针运算表示（32 位进程中尤其要防止行号 × 行距回绕）。
    const std::uint64_t extent = static_cast<std::uint64_t>(height - 1) * static_cast<std::uint64_t>(pitchBytes) +
        static_cast<std::uint64_t>(width) * 2;
    return extent <= static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
}

enum class ScalePath
{
    Nearest,
    Packed,
    Wide
};

template <PixelFormat16 FORMAT>
void RunPath(ScalePath path, const ScaleContext& context, std::uint32_t* sums) noexcept
{
    switch (path)
    {
    case ScalePath::Nearest:
        ScaleNearest<FORMAT>(context);
        break;
    case ScalePath::Packed:
        ScalePacked<FORMAT>(context, sums);
        break;
    case ScalePath::Wide:
        ScaleWide<FORMAT>(context, sums);
        break;
    }
}
}

void ScaleScratch::Release() noexcept
{
    std::vector<std::uint32_t>().swap(mSums);
}

bool ScaleBattlefield(const ConstPixelView16& source, const PixelView16& target, const ZoomTransform& transform,
    PixelFormat16 format, const SpanTable& columnSpans, const SpanTable& rowSpans, ScaleScratch& scratch) noexcept
{
    if (format != PixelFormat16::Rgb565 && format != PixelFormat16::Rgb555)
        return false;
    if (!transform.IsValid() || !columnSpans.IsBuiltFor(transform.mPhysicalWidth, transform.mLogicalWidth) ||
        !rowSpans.IsBuiltFor(transform.mPhysicalHeight, transform.mLogicalHeight))
        return false;
    if (!IsUsableView(source.mData, source.mWidth, source.mHeight, source.mPitchBytes,
            transform.mLogicalWidth, transform.mLogicalHeight) ||
        !IsUsableView(target.mData, target.mWidth, target.mHeight, target.mPitchBytes,
            transform.mPhysicalWidth, transform.mPhysicalHeight))
        return false;

    const std::int64_t maxBox = static_cast<std::int64_t>(columnSpans.GetMaxSpanLength()) * rowSpans.GetMaxSpanLength();
    const ScalePath path = maxBox == 1 ? ScalePath::Nearest :
        maxBox <= static_cast<std::int64_t>(MAX_PACKED_BOX) ? ScalePath::Packed : ScalePath::Wide;
    const std::size_t sumsPerColumn = path == ScalePath::Wide ? 3 : path == ScalePath::Packed ? 1 : 0;
    const std::size_t requiredSums = sumsPerColumn * static_cast<std::size_t>(transform.mLogicalWidth);
    if (scratch.mSums.size() < requiredSums)
    {
        try
        {
            scratch.mSums.resize(requiredSums);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }

    ScaleContext context;
    context.mSource = static_cast<const std::uint8_t*>(source.mData);
    context.mSourcePitch = static_cast<std::size_t>(source.mPitchBytes);
    context.mTarget = static_cast<std::uint8_t*>(target.mData);
    context.mTargetPitch = static_cast<std::size_t>(target.mPitchBytes);
    context.mPhysicalWidth = transform.mPhysicalWidth;
    context.mPhysicalHeight = transform.mPhysicalHeight;
    context.mLogicalWidth = transform.mLogicalWidth;
    context.mColumns = &columnSpans;
    context.mRows = &rowSpans;
    std::uint32_t* sums = scratch.mSums.data();
    if (format == PixelFormat16::Rgb565)
        RunPath<PixelFormat16::Rgb565>(path, context, sums);
    else
        RunPath<PixelFormat16::Rgb555>(path, context, sums);
    return true;
}

bool BattlefieldScaler::Scale(const ConstPixelView16& source, const PixelView16& target, const ZoomTransform& transform,
    PixelFormat16 format) noexcept
{
    if (!transform.IsValid())
        return false;
    if (!mColumnSpans.IsBuiltFor(transform.mPhysicalWidth, transform.mLogicalWidth) ||
        !mRowSpans.IsBuiltFor(transform.mPhysicalHeight, transform.mLogicalHeight))
    {
        try
        {
            mColumnSpans = BuildSpanTable(transform.mPhysicalWidth, transform.mLogicalWidth);
            mRowSpans = BuildSpanTable(transform.mPhysicalHeight, transform.mLogicalHeight);
        }
        catch (const std::bad_alloc&)
        {
            Release();
            return false;
        }
    }
    return ScaleBattlefield(source, target, transform, format, mColumnSpans, mRowSpans, mScratch);
}

void BattlefieldScaler::Release() noexcept
{
    mColumnSpans = SpanTable();
    mRowSpans = SpanTable();
    mScratch.Release();
}
}
