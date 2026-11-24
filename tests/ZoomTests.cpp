#include "../src/zoom/ScalePixels.h"
#include "../src/zoom/ZoomAnchor.h"
#include "../src/zoom/ZoomLevels.h"
#include "../src/zoom/ZoomMapping.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
using namespace rareplay::zoom;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// 热循环用：只在失败时才拼接说明。
void Require(bool condition, const char* message, const std::string& where = std::string())
{
    if (!condition)
        throw std::runtime_error(message + where);
}

std::string Describe(int physicalSize, int logicalSize)
{
    return " (P=" + std::to_string(physicalSize) + ", L=" + std::to_string(logicalSize) + ")";
}

// 独立参照：双精度 floor(a / b)。|a| < 2^53 时商的舍入误差小于 1/b，不会跨过整数。
std::int64_t ReferenceFloor(std::int64_t numerator, std::int64_t denominator)
{
    return static_cast<std::int64_t>(std::floor(static_cast<double>(numerator) / static_cast<double>(denominator)));
}

SourceSpan ReferenceSpan(std::int64_t pixel, int physicalSize, int logicalSize)
{
    const std::int64_t begin = ReferenceFloor(pixel * logicalSize, physicalSize);
    const std::int64_t end = std::max(begin + 1, ReferenceFloor((pixel + 1) * logicalSize, physicalSize));
    return {static_cast<int>(begin), static_cast<int>(end)};
}

bool Contains(const SourceSpan& span, int value)
{
    return span.mBegin <= value && value < span.mEnd;
}

// 固定边界组合 + 固定种子的随机组合：含 P 或 L 为 1、互质、L 远大于 P、上限尺寸与实际战术区尺寸。
std::vector<std::pair<int, int>> SizePairs()
{
    std::vector<std::pair<int, int>> pairs{
        {1, 1}, {1, 2}, {2, 1}, {1, 7}, {7, 1}, {1, MAX_ZOOM_EXTENT}, {MAX_ZOOM_EXTENT, 1},
        {MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT}, {2, MAX_ZOOM_EXTENT}, {MAX_ZOOM_EXTENT - 1, MAX_ZOOM_EXTENT},
        {MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT - 1}, {3, 2}, {2, 3}, {7, 5}, {5, 7}, {13, 8}, {8, 13}, {97, 1000},
        {1000, 97}, {3, 30000}, {30000, 3}, {1752, 1752}, {1752, 876}, {1752, 3504}, {1752, 5256}, {1048, 3144},
        {2392, 598}, {2392, 4784}, {2392, 5980}, {2392, 7176}, {1408, 4224}, {1751, 1751}};
    std::mt19937 random(20261025u);
    std::uniform_int_distribution<int> small(1, 300);
    std::uniform_int_distribution<int> medium(1, 5000);
    for (int index = 0; index < 150; ++index)
        pairs.emplace_back(small(random), small(random));
    for (int index = 0; index < 60; ++index)
        pairs.emplace_back(medium(random), medium(random));
    return pairs;
}

void TestFloorDivide()
{
    Require(FloorDivide(7, 2) == 3 && FloorDivide(-7, 2) == -4 && FloorDivide(-8, 2) == -4 && FloorDivide(0, 5) == 0 &&
            FloorDivide(-1, 32767) == -1 && FloorDivide(-32767, 32767) == -1 && FloorDivide(-32768, 32767) == -2,
        "FloorDivide rounds toward negative infinity");
}

// 区间表：覆盖 [0, L)、首尾相接或相同（相同只在放大时）、单调；放大时长度为 1；1× 恒等；与逐个计算及参照一致。
void TestSpanTables()
{
    for (const auto& [physical, logical] : SizePairs())
    {
        const std::string where = Describe(physical, logical);
        const SpanTable table = BuildSpanTable(physical, logical);
        Require(table.IsBuiltFor(physical, logical) && table.GetPhysicalSize() == physical &&
                table.GetLogicalSize() == logical && table.GetSpans().size() == static_cast<std::size_t>(physical),
            "table is built for its sizes", where);
        Require(table[0].mBegin == 0 && table[physical - 1].mEnd == logical, "table covers [0, L)", where);
        int maxLength = 0;
        for (int pixel = 0; pixel < physical; ++pixel)
        {
            const SourceSpan span = table[pixel];
            Require(span == SpanForPhysicalPixel(pixel, physical, logical) && span == ReferenceSpan(pixel, physical, logical),
                "table matches formula", where);
            Require(span.mBegin >= 0 && span.mBegin < span.mEnd && span.mEnd <= logical, "span inside [0, L)", where);
            maxLength = std::max(maxLength, span.mEnd - span.mBegin);
            if (logical <= physical)
                Require(span.mEnd - span.mBegin == 1, "magnified spans have length 1", where);
            if (logical == physical)
                Require(span == SourceSpan{pixel, pixel + 1}, "identity spans", where);
            if (pixel + 1 < physical)
            {
                const SourceSpan next = table[pixel + 1];
                if (next == span)
                    Require(logical < physical, "only magnification repeats a span", where);
                else
                    Require(next.mBegin == span.mEnd, "distinct spans are contiguous and never overlap", where);
            }
        }
        Require(table.GetMaxSpanLength() == maxLength, "max span length", where);
    }
    for (const auto& [physical, logical] : std::vector<std::pair<int, int>>{
             {0, 5}, {5, 0}, {-1, 5}, {5, -1}, {MAX_ZOOM_EXTENT + 1, 5}, {5, MAX_ZOOM_EXTENT + 1}})
    {
        const SpanTable table = BuildSpanTable(physical, logical);
        Require(!table.IsBuiltFor(physical, logical) && table.GetSpans().empty() && table.GetMaxSpanLength() == 0,
            "invalid sizes build an empty table" + Describe(physical, logical));
    }
}

// 映射：中点在区间内、单调、1× 恒等、外推与公式一致；逻辑到物理的结果其区间包含 x，且是第一个这样的物理像素。
void TestMapping()
{
    for (const auto& [physical, logical] : SizePairs())
    {
        const std::string where = Describe(physical, logical);
        const int step = std::max(1, physical / 400);
        int previous = INT_MIN;
        for (int pixel = -2 * physical - 3; pixel < 3 * physical + 3; pixel += step)
        {
            const SourceSpan span = SpanForPhysicalPixel(pixel, physical, logical);
            Require(span == ReferenceSpan(pixel, physical, logical), "extrapolated span matches formula", where);
            const int mapped = MapPhysicalToLogical(pixel, physical, logical);
            Require(Contains(span, mapped) && mapped == span.mBegin + (span.mEnd - span.mBegin - 1) / 2,
                "midpoint lies inside its span", where);
            Require(mapped >= previous, "physical to logical is monotone", where);
            previous = mapped;
            if (logical == physical)
                Require(mapped == pixel, "identity at 1x", where);
        }
        const int logicalStep = std::max(1, logical / 400);
        for (int x = -2 * logical - 3; x < 3 * logical + 3; x += logicalStep)
        {
            const int pixel = MapLogicalToPhysical(x, physical, logical);
            Require(Contains(SpanForPhysicalPixel(pixel, physical, logical), x), "logical to physical lands on a pixel showing x", where);
            Require(SpanForPhysicalPixel(pixel - 1, physical, logical).mEnd <= x, "logical to physical picks the first such pixel", where);
            const double continuous = static_cast<double>(x) * physical / logical;
            Require(std::abs(pixel - continuous) < 1.0, "logical to physical stays within one physical pixel", where);
            if (logical == physical)
                Require(pixel == x, "logical to physical identity at 1x", where);
        }
    }

    // 恒等与饱和：任意 int 都不触发未定义行为，1× 时对全部 int 恒等。
    for (const int pixel : {INT_MIN, INT_MIN + 1, -1, 0, 1, INT_MAX - 1, INT_MAX})
    {
        Require(MapPhysicalToLogical(pixel, 1752, 1752) == pixel && MapLogicalToPhysical(pixel, 1752, 1752) == pixel,
            "identity holds for every int");
        for (const auto& [physical, logical] : std::vector<std::pair<int, int>>{{1, MAX_ZOOM_EXTENT}, {MAX_ZOOM_EXTENT, 1}})
        {
            const SourceSpan span = SpanForPhysicalPixel(pixel, physical, logical);
            Require(span.mBegin < span.mEnd, "saturated span stays non-empty");
            static_cast<void>(MapPhysicalToLogical(pixel, physical, logical));
            static_cast<void>(MapLogicalToPhysical(pixel, physical, logical));
        }
    }
    Require(SpanForPhysicalPixel(INT_MAX, 1, MAX_ZOOM_EXTENT) == SourceSpan{INT_MAX - 1, INT_MAX} &&
            SpanForPhysicalPixel(INT_MIN, 1, MAX_ZOOM_EXTENT) == SourceSpan{INT_MIN, INT_MIN + 1},
        "out-of-range spans saturate");

    // 尺寸不合法时按恒等防御。
    for (const auto& [physical, logical] : std::vector<std::pair<int, int>>{{0, 10}, {10, 0}, {-5, 10}, {MAX_ZOOM_EXTENT + 1, 10}})
    {
        Require(SpanForPhysicalPixel(17, physical, logical) == SourceSpan{17, 18} &&
                MapPhysicalToLogical(17, physical, logical) == 17 && MapLogicalToPhysical(17, physical, logical) == 17,
            "invalid sizes fall back to identity" + Describe(physical, logical));
    }

    // 直接取 floor(x·P/L) 的反例：P=3、L=2 时逻辑像素 1 只显示在物理像素 2。
    Require(SpanForPhysicalPixel(1, 3, 2) == SourceSpan{0, 1} && MapLogicalToPhysical(1, 3, 2) == 2,
        "logical to physical follows spans rather than floor(x*P/L)");
}

void TestTransform()
{
    Require(!ZoomTransform{}.IsValid() && !ZoomTransform{}.IsIdentity(), "default transform is invalid");
    Require(ZoomTransform{1, 1, 1, 1}.IsValid() && ZoomTransform{1, 1, 1, 1}.IsIdentity(), "1x1 identity");
    Require(ZoomTransform{MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT}.IsValid(), "upper limit is valid");
    Require(!ZoomTransform{MAX_ZOOM_EXTENT + 1, 10, 10, 10}.IsValid() && !ZoomTransform{10, 10, 10, MAX_ZOOM_EXTENT + 1}.IsValid() &&
            !ZoomTransform{10, 0, 10, 10}.IsValid() && !ZoomTransform{10, 10, -10, 10}.IsValid(),
        "out-of-range transforms are invalid");
    Require(!ZoomTransform{1752, 1048, 876, 1048}.IsIdentity() && ZoomTransform{1752, 1048, 1752, 1048}.IsIdentity(),
        "identity requires both axes equal");
    const ZoomTransform transform{1752, 1048, 4380, 2620};
    for (const ZoomPoint point : {ZoomPoint{0, 0}, ZoomPoint{1751, 1047}, ZoomPoint{-30, 2000}, ZoomPoint{903, 517}})
    {
        Require(transform.MapPhysicalToLogical(point) ==
                    ZoomPoint{MapPhysicalToLogical(point.mX, 1752, 4380), MapPhysicalToLogical(point.mY, 1048, 2620)} &&
                transform.MapLogicalToPhysical(point) ==
                    ZoomPoint{MapLogicalToPhysical(point.mX, 1752, 4380), MapLogicalToPhysical(point.mY, 1048, 2620)},
            "2D mapping applies each axis independently");
    }
}

// 档位：1× 精确等于 P（奇数也是）；其他档偶数化且不小于下限；表与手算值一致。
void TestLevels()
{
    for (std::size_t index = 0; index + 1 < ZOOM_LEVELS.size(); ++index)
    {
        const ZoomLevel larger = ZOOM_LEVELS[index];
        const ZoomLevel smaller = ZOOM_LEVELS[index + 1];
        Require(static_cast<std::int64_t>(larger.mNumerator) * smaller.mDenominator >
                    static_cast<std::int64_t>(smaller.mNumerator) * larger.mDenominator,
            "levels go from most magnified to farthest shrink");
    }
    Require(ZOOM_LEVELS[MOST_MAGNIFIED_ZOOM_INDEX] == ZoomLevel{4, 1} && ZOOM_LEVELS[IDENTITY_ZOOM_INDEX] == ZoomLevel{1, 1} &&
            ZOOM_LEVELS[FARTHEST_SHRINK_ZOOM_INDEX] == ZoomLevel{1, 3},
        "level table endpoints");

    // 1920×1080 窗口的战术区 1752×1048，逐档手算（四舍五入到最近偶数）。
    const LogicalSize expected[] = {{438, 262}, {584, 350}, {700, 420}, {876, 524}, {1168, 698}, {1402, 838},
        {1752, 1048}, {2190, 1310}, {2628, 1572}, {3504, 2096}, {4380, 2620}, {5256, 3144}};
    for (std::size_t index = 0; index < ZOOM_LEVELS.size(); ++index)
        Require(ComputeLogicalSize(1752, 1048, ZOOM_LEVELS[index]) == expected[index], "hand computed logical size " + std::to_string(index));

    Require(ComputeLogicalSize(1751, 1047, {1, 1}) == LogicalSize{1751, 1047} && ComputeLogicalSize(33, 17, {1, 1}) == LogicalSize{33, 17} &&
            ComputeLogicalSize(1751, 1047, {3, 3}) == LogicalSize{1751, 1047},
        "1x keeps odd and tiny physical sizes exactly");
    Require(ComputeLogicalSize(100, 200, {4, 1}) == LogicalSize{MIN_LOGICAL_EXTENT, MIN_LOGICAL_EXTENT}, "lower bound");
    Require(ComputeLogicalSize(MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT, {1, 3}) == LogicalSize{98302, 98302} &&
            !ZoomTransform{MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT, 98302, 98302}.IsValid(),
        "results above the extent limit are returned and rejected by the transform");
    for (const auto& [width, height, level] : std::vector<std::tuple<int, int, ZoomLevel>>{
             {0, 100, {1, 2}}, {100, -1, {1, 2}}, {MAX_ZOOM_EXTENT + 1, 100, {1, 2}}, {100, 100, {0, 1}}, {100, 100, {1, 0}}, {100, 100, {-1, 2}}})
        Require(ComputeLogicalSize(width, height, level) == LogicalSize{}, "invalid input gives {0, 0}");

    // 独立参照：在两个相邻偶数候选中取离 P·den/num 更近的一个，恰在中间取较大者，再套下限。
    for (int physical = 1; physical <= 3000; ++physical)
    {
        for (std::size_t index = 0; index < ZOOM_LEVELS.size(); ++index)
        {
            if (index == static_cast<std::size_t>(IDENTITY_ZOOM_INDEX))
                continue;
            const ZoomLevel level = ZOOM_LEVELS[index];
            const std::int64_t scaled = static_cast<std::int64_t>(physical) * level.mDenominator;
            const std::int64_t lower = 2 * (scaled / (2 * level.mNumerator));
            const std::int64_t upper = lower + 2;
            const std::int64_t chosen = std::llabs(upper * level.mNumerator - scaled) <= std::llabs(lower * level.mNumerator - scaled) ? upper : lower;
            const LogicalSize size = ComputeLogicalSize(physical, physical, level);
            Require(size.mWidth == std::max<std::int64_t>(chosen, MIN_LOGICAL_EXTENT) && size.mWidth % 2 == 0 && size.mWidth == size.mHeight,
                "nearest even rounding for P=", std::to_string(physical));
        }
    }
}

void TestResourceEstimate()
{
    Require(EstimateLogicalResourceBytes({3504, 2096}, {3504, 2096}) == 58755072ull, "four buffers at 1/2 of 1752x1048");
    Require(EstimateLogicalResourceBytes({876, 524}, {5256, 3144}) == 67935552ull, "Z/A at the current level, Composite/Tile at the farthest");
    Require(EstimateLogicalResourceBytes({0, 0}, {0, 0}) == 0ull, "empty sizes");
    Require(!EstimateLogicalResourceBytes({-1, 10}, {10, 10}) && !EstimateLogicalResourceBytes({10, 10}, {10, -1}), "negative sizes");
    const std::uint64_t maxFarthest = 4ull * static_cast<std::uint64_t>(INT_MAX) * static_cast<std::uint64_t>(INT_MAX);
    Require(EstimateLogicalResourceBytes({0, 0}, {INT_MAX, INT_MAX}) == maxFarthest, "largest representable estimate");
    Require(EstimateLogicalResourceBytes({65535, 65536}, {INT_MAX, INT_MAX}) == maxFarthest + 4ull * 65535 * 65536,
        "sum just below the 64-bit limit");
    Require(!EstimateLogicalResourceBytes({65536, 65536}, {INT_MAX, INT_MAX}) && !EstimateLogicalResourceBytes({INT_MAX, INT_MAX}, {INT_MAX, INT_MAX}),
        "64-bit overflow is detected");
}

// 预算推导（默认 128 MiB = 134217728 字节，四块缓冲都按该档计，每像素 8 字节）：
// 1920×1080 窗口（P=1752×1048）1/3 档 L=5256×3144 需 132198912 字节，可到 1/3；
// 2560×1440 窗口（P=2392×1408）1/3 需 242491392、2/5（5980×3520）需 168396800，1/2（4784×2816）需 107773952，最远 1/2；
// 3840×2160 窗口按同样的侧栏宽 168、上下边共 32 计为 3672×2128：1/2 需 250048512、2/3（5508×3192）需 140652288，
// 4/5（4590×2660）需 97675200，最远 4/5。
void TestPolicy()
{
    static_assert(DEFAULT_ZOOM_MEMORY_BUDGET_BYTES == 134217728ull);
    const ZoomLevelPolicy hd(1752, 1048);
    Require(hd.GetMostMagnifiedIndex() == MOST_MAGNIFIED_ZOOM_INDEX && hd.GetFarthestShrinkIndex() == FARTHEST_SHRINK_ZOOM_INDEX,
        "1920x1080 at 128 MiB reaches 1/3 (126 MiB)");
    Require(*EstimateLogicalResourceBytes({5256, 3144}, {5256, 3144}) == 132198912ull, "1920x1080 at 1/3 needs 126 MiB");
    const ZoomLevelPolicy qhd(2392, 1408);
    Require(qhd.GetFarthestShrinkIndex() == 9 && ZOOM_LEVELS[9] == ZoomLevel{1, 2}, "2560x1440 at 128 MiB stops at 1/2 (103 MiB)");
    Require(*EstimateLogicalResourceBytes({5980, 3520}, {5980, 3520}) == 168396800ull &&
            *EstimateLogicalResourceBytes({4784, 2816}, {4784, 2816}) == 107773952ull,
        "2560x1440 needs 161 MiB at 2/5 and 103 MiB at 1/2");
    const ZoomLevelPolicy uhd(3672, 2128);
    Require(uhd.GetFarthestShrinkIndex() == 7 && ZOOM_LEVELS[7] == ZoomLevel{4, 5}, "3840x2160 at 128 MiB stops at 4/5");
    Require(ZoomLevelPolicy(2392, 1408, 256ull * 1024 * 1024).GetFarthestShrinkIndex() == FARTHEST_SHRINK_ZOOM_INDEX &&
            ZoomLevelPolicy(3672, 2128, 256ull * 1024 * 1024).GetFarthestShrinkIndex() == 9,
        "a 256 MiB budget lets 2560x1440 reach 1/3 and 3840x2160 reach 1/2");

    const LogicalSize farthest = ComputeLogicalSize(2392, 1408, ZOOM_LEVELS[FARTHEST_SHRINK_ZOOM_INDEX]);
    Require(farthest == LogicalSize{7176, 4224}, "2560x1440 farthest logical size");
    const std::uint64_t exact = *EstimateLogicalResourceBytes(farthest, farthest);
    Require(exact == 242491392ull && ZoomLevelPolicy(2392, 1408, exact).GetFarthestShrinkIndex() == FARTHEST_SHRINK_ZOOM_INDEX &&
            ZoomLevelPolicy(2392, 1408, exact - 1).GetFarthestShrinkIndex() == 10,
        "budget boundary is inclusive");
    Require(ZoomLevelPolicy(1752, 1048, DEFAULT_ZOOM_MEMORY_BUDGET_BYTES, 8).GetFarthestShrinkIndex() == 8, "configured limit wins");
    Require(ZoomLevelPolicy(1752, 1048, DEFAULT_ZOOM_MEMORY_BUDGET_BYTES, 3).GetFarthestShrinkIndex() == IDENTITY_ZOOM_INDEX &&
            ZoomLevelPolicy(1752, 1048, DEFAULT_ZOOM_MEMORY_BUDGET_BYTES, -100).GetFarthestShrinkIndex() == IDENTITY_ZOOM_INDEX,
        "configured magnification means no shrinking");
    Require(ZoomLevelPolicy(1752, 1048, DEFAULT_ZOOM_MEMORY_BUDGET_BYTES, 99).GetFarthestShrinkIndex() == FARTHEST_SHRINK_ZOOM_INDEX,
        "configured index is clamped to the table");
    Require(ZoomLevelPolicy(1752, 1048, 0).GetFarthestShrinkIndex() == IDENTITY_ZOOM_INDEX &&
            ZoomLevelPolicy(1752, 1048, 0).GetMostMagnifiedIndex() == MOST_MAGNIFIED_ZOOM_INDEX,
        "zero budget still allows magnification");
    Require(ZoomLevelPolicy(MAX_ZOOM_EXTENT, MAX_ZOOM_EXTENT, UINT64_MAX).GetFarthestShrinkIndex() == IDENTITY_ZOOM_INDEX,
        "levels exceeding the extent limit are not allowed");
    const ZoomLevelPolicy tiny(50, 600);
    Require(tiny.GetMostMagnifiedIndex() == IDENTITY_ZOOM_INDEX && tiny.GetFarthestShrinkIndex() == FARTHEST_SHRINK_ZOOM_INDEX,
        "physical sizes below the lower bound do not magnify");
    const ZoomLevelPolicy invalid(0, 600);
    Require(invalid.GetMostMagnifiedIndex() == IDENTITY_ZOOM_INDEX && invalid.GetFarthestShrinkIndex() == IDENTITY_ZOOM_INDEX &&
            invalid.Step(IDENTITY_ZOOM_INDEX, 3) == IDENTITY_ZOOM_INDEX && invalid.GetLogicalSize(0) == LogicalSize{},
        "invalid physical size only allows 1x");

    Require(hd.Step(6, 1) == 5 && hd.Step(6, -1) == 7 && hd.Step(6, 0) == 6 && hd.Step(6, 100) == 0 && hd.Step(6, -100) == 11 &&
            hd.Step(0, 1) == 0 && hd.Step(11, -1) == 11 && hd.Step(6, INT_MAX) == 0 && hd.Step(6, INT_MIN) == 11 &&
            hd.Step(-50, 0) == 0 && hd.Step(50, 0) == 11 && hd.Step(INT_MIN, INT_MIN) == 11 && hd.Step(INT_MAX, INT_MAX) == 0,
        "step is clamped to the allowed range without overflow");
    const ZoomLevelPolicy limited(2392, 1408, 128ull * 1024 * 1024);
    Require(limited.Step(6, -100) == 9 && limited.Step(11, 0) == 9 && limited.Step(11, 1) == 8, "step respects the budget limit");
    Require(ZoomLevelPolicy::ResetIndex() == IDENTITY_ZOOM_INDEX && hd.GetLogicalSize(ZoomLevelPolicy::ResetIndex()) == LogicalSize{1752, 1048} &&
            ZoomLevelPolicy(1751, 1047).GetLogicalSize(ZoomLevelPolicy::ResetIndex()) == LogicalSize{1751, 1047},
        "reset returns to exact 1x");
    Require(limited.GetLogicalSize(FARTHEST_SHRINK_ZOOM_INDEX) == limited.GetLogicalSize(9), "logical size index is clamped");
}

ZoomTransform TransformFor(int physicalWidth, int physicalHeight, int index)
{
    const LogicalSize size = ComputeLogicalSize(physicalWidth, physicalHeight, ZOOM_LEVELS[static_cast<std::size_t>(index)]);
    return {physicalWidth, physicalHeight, size.mWidth, size.mHeight};
}

// 锚点：任意锚点、T1 与档位组合下，锚点下的逻辑点 m(a) + TacticalPos 不变；锚点在战术区外时按 P/2。
void TestAnchor()
{
    std::mt19937 random(7u);
    std::uniform_int_distribution<int> tactical(-100000, 100000);
    for (const auto& [width, height] : std::vector<std::pair<int, int>>{{1752, 1048}, {2392, 1408}, {1751, 1047}})
    {
        std::uniform_int_distribution<int> anchorX(0, width - 1);
        std::uniform_int_distribution<int> anchorY(0, height - 1);
        std::vector<ZoomPoint> anchors{{0, 0}, {width - 1, height - 1}, {width / 2, height / 2}, {-1, 5}, {width, height + 10},
            {INT_MIN, INT_MAX}, {17, -3}};
        for (int index = 0; index < 8; ++index)
            anchors.push_back({anchorX(random), anchorY(random)});
        for (int from = 0; from < static_cast<int>(ZOOM_LEVELS.size()); ++from)
        {
            for (int to = 0; to < static_cast<int>(ZOOM_LEVELS.size()); ++to)
            {
                const ZoomTransform oldTransform = TransformFor(width, height, from);
                const ZoomTransform newTransform = TransformFor(width, height, to);
                for (const ZoomPoint anchor : anchors)
                {
                    const ZoomPoint oldTacticalPos{tactical(random), tactical(random)};
                    const auto view = ComputeAnchoredView(oldTransform, newTransform, anchor, oldTacticalPos);
                    Require(view.has_value(), "anchored view is computed");
                    const ZoomPoint effective{anchor.mX >= 0 && anchor.mX < width ? anchor.mX : width / 2,
                        anchor.mY >= 0 && anchor.mY < height ? anchor.mY : height / 2};
                    const ZoomPoint before = oldTransform.MapPhysicalToLogical(effective);
                    const ZoomPoint after = newTransform.MapPhysicalToLogical(effective);
                    Require(before.mX + oldTacticalPos.mX == after.mX + view->mTacticalPos.mX &&
                            before.mY + oldTacticalPos.mY == after.mY + view->mTacticalPos.mY,
                        "the logical point under the anchor does not move");
                    Require(view->mCenter == ZoomPoint{view->mTacticalPos.mX + newTransform.mLogicalWidth / 2,
                                                 view->mTacticalPos.mY + newTransform.mLogicalHeight / 2},
                        "center is TacticalPos + L/2");
                    if (!(effective == anchor))
                        Require(view == ComputeAnchoredView(oldTransform, newTransform, effective, oldTacticalPos),
                            "anchor axes outside the tactical area use P/2");
                }
            }
        }
    }

    // 手算：1× → 2× 以战术区中心为锚点时镜头中心不动。
    const auto centered = ComputeAnchoredView(TransformFor(1752, 1048, IDENTITY_ZOOM_INDEX), TransformFor(1752, 1048, 3), {876, 524}, {1000, 2000});
    Require(centered && centered->mTacticalPos == ZoomPoint{1438, 2262} && centered->mCenter == ZoomPoint{1876, 2524},
        "zooming at the center keeps the camera center");

    const ZoomTransform valid = TransformFor(1752, 1048, IDENTITY_ZOOM_INDEX);
    Require(!ComputeAnchoredView(valid, TransformFor(1751, 1048, 3), {0, 0}, {0, 0}) && !ComputeAnchoredView(ZoomTransform{}, valid, {0, 0}, {0, 0}) &&
            !ComputeAnchoredView(valid, ZoomTransform{1752, 1048, 0, 10}, {0, 0}, {0, 0}),
        "mismatched or invalid transforms are rejected");
    Require(!ComputeAnchoredView(TransformFor(1752, 1048, FARTHEST_SHRINK_ZOOM_INDEX), TransformFor(1752, 1048, 0), {1751, 1047}, {INT_MAX, INT_MAX}),
        "int overflow is rejected");
}

// ---------------- 像素缩放 ----------------

struct Image
{
    int mWidth = 0;
    int mHeight = 0;
    int mPitchPixels = 0;
    std::vector<std::uint16_t> mPixels;

    Image(int width, int height, int pitchPixels, std::uint16_t fill)
        : mWidth(width), mHeight(height), mPitchPixels(pitchPixels),
          mPixels(static_cast<std::size_t>(pitchPixels) * static_cast<std::size_t>(height), fill)
    {
    }

    std::uint16_t& At(int x, int y) { return mPixels[static_cast<std::size_t>(y) * mPitchPixels + x]; }
    std::uint16_t At(int x, int y) const { return mPixels[static_cast<std::size_t>(y) * mPitchPixels + x]; }
    ConstPixelView16 View() const { return {mPixels.data(), mWidth, mHeight, mPitchPixels * 2}; }
    PixelView16 MutableView() { return {mPixels.data(), mWidth, mHeight, mPitchPixels * 2}; }
};

struct Channels
{
    std::uint32_t mRed = 0;
    std::uint32_t mGreen = 0;
    std::uint32_t mBlue = 0;
};

Channels Decode(std::uint16_t pixel, PixelFormat16 format)
{
    if (format == PixelFormat16::Rgb565)
        return {static_cast<std::uint32_t>(pixel >> 11) & 31u, static_cast<std::uint32_t>(pixel >> 5) & 63u, pixel & 31u};
    return {static_cast<std::uint32_t>(pixel >> 10) & 31u, static_cast<std::uint32_t>(pixel >> 5) & 31u, pixel & 31u};
}

std::uint16_t Encode(Channels channels, PixelFormat16 format)
{
    const int redShift = format == PixelFormat16::Rgb565 ? 11 : 10;
    return static_cast<std::uint16_t>((channels.mRed << redShift) | (channels.mGreen << 5) | channels.mBlue);
}

// 独立参照：逐个目标像素取区间盒，按通道 64 位求和后四舍五入取平均。
Image ReferenceScale(const Image& source, const ZoomTransform& transform, PixelFormat16 format)
{
    Image target(transform.mPhysicalWidth, transform.mPhysicalHeight, transform.mPhysicalWidth, 0);
    for (int y = 0; y < transform.mPhysicalHeight; ++y)
    {
        const SourceSpan rows = ReferenceSpan(y, transform.mPhysicalHeight, transform.mLogicalHeight);
        for (int x = 0; x < transform.mPhysicalWidth; ++x)
        {
            const SourceSpan columns = ReferenceSpan(x, transform.mPhysicalWidth, transform.mLogicalWidth);
            std::uint64_t red = 0;
            std::uint64_t green = 0;
            std::uint64_t blue = 0;
            for (int sy = rows.mBegin; sy < rows.mEnd; ++sy)
            {
                for (int sx = columns.mBegin; sx < columns.mEnd; ++sx)
                {
                    const Channels channels = Decode(source.At(sx, sy), format);
                    red += channels.mRed;
                    green += channels.mGreen;
                    blue += channels.mBlue;
                }
            }
            const std::uint64_t count = static_cast<std::uint64_t>(rows.mEnd - rows.mBegin) * (columns.mEnd - columns.mBegin);
            target.At(x, y) = Encode({static_cast<std::uint32_t>((red + count / 2) / count),
                                         static_cast<std::uint32_t>((green + count / 2) / count),
                                         static_cast<std::uint32_t>((blue + count / 2) / count)},
                format);
        }
    }
    return target;
}

bool SameRegion(const Image& actual, const Image& expected, int width, int height)
{
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (actual.At(x, y) != expected.At(x, y))
                return false;
    return true;
}

void FillRandom(Image& image, std::mt19937& random)
{
    std::uniform_int_distribution<int> value(0, 0xFFFF);
    for (std::uint16_t& pixel : image.mPixels)
        pixel = static_cast<std::uint16_t>(value(random));
}

const char* FormatName(PixelFormat16 format)
{
    return format == PixelFormat16::Rgb565 ? "565" : "555";
}

constexpr PixelFormat16 FORMATS[] = {PixelFormat16::Rgb565, PixelFormat16::Rgb555};

// 手算金样：两个 2×2 盒（1/2 档）。
void TestGoldenPixels()
{
    const ZoomTransform transform{2, 1, 4, 2};
    BattlefieldScaler scaler;
    {
        // 565 盒 1：R {31,0,0,0}=31 → (31+2)/4=8；G {63,63,0,1}=127 → (127+2)/4=32；B {1,2,3,4}=10 → (10+2)/4=3 → 0x4403。
        Image source(4, 2, 4, 0xFFFF);
        source.At(0, 0) = 0xFFE1;
        source.At(1, 0) = 0x07E2;
        source.At(0, 1) = 0x0003;
        source.At(1, 1) = 0x0024;
        Image target(2, 1, 2, 0);
        Require(scaler.Scale(source.View(), target.MutableView(), transform, PixelFormat16::Rgb565) &&
                target.At(0, 0) == 0x4403 && target.At(1, 0) == 0xFFFF,
            "565 golden box average");
    }
    {
        // 555 盒 1（最高位全为 1）：R {31,31,0,0}=62 → 16；G {31,0,0,0}=31 → 8；B {0,0,0,1}=1 → (1+2)/4=0 → 0x4100。
        // 盒 2 全 0xFFFF → 各通道 31，最高位写回 0 → 0x7FFF。
        Image source(4, 2, 4, 0xFFFF);
        source.At(0, 0) = 0xFFE0;
        source.At(1, 0) = 0xFC00;
        source.At(0, 1) = 0x8000;
        source.At(1, 1) = 0x8001;
        Image target(2, 1, 2, 0);
        Require(scaler.Scale(source.View(), target.MutableView(), transform, PixelFormat16::Rgb555) &&
                target.At(0, 0) == 0x4100 && target.At(1, 0) == 0x7FFF,
            "555 golden box average clears the top bit");
    }
    {
        // 盒长 3 与 2 的四舍五入：B {1,1,0}=2 → (2+1)/3=1；{1,0,0}=1 → (1+1)/3=0；{1,0}=1 → (1+1)/2=1（恰半进位）。
        Image three(6, 1, 6, 0);
        three.At(0, 0) = 1;
        three.At(1, 0) = 1;
        three.At(3, 0) = 1;
        Image target(2, 1, 2, 0xFFFF);
        Require(scaler.Scale(three.View(), target.MutableView(), {2, 1, 6, 1}, PixelFormat16::Rgb565) &&
                target.At(0, 0) == 1 && target.At(1, 0) == 0,
            "rounding with a box of 3");
        Image two(2, 1, 2, 0);
        two.At(0, 0) = 1;
        Image one(1, 1, 1, 0xFFFF);
        Require(scaler.Scale(two.View(), one.MutableView(), {1, 1, 2, 1}, PixelFormat16::Rgb565) && one.At(0, 0) == 1,
            "half rounds up");
    }
}

void TestUniformAndCheckerboard()
{
    std::mt19937 random(11u);
    std::uniform_int_distribution<int> value(0, 0xFFFF);
    BattlefieldScaler scaler;
    const std::vector<ZoomTransform> transforms{{40, 30, 120, 90}, {40, 30, 80, 60}, {40, 30, 100, 76}, {40, 30, 10, 8},
        {37, 23, 37, 23}, {3, 2, 200, 100}, {1, 1, 33, 1}};
    for (const ZoomTransform& transform : transforms)
    {
        for (const PixelFormat16 format : FORMATS)
        {
            const auto color = static_cast<std::uint16_t>(value(random));
            const std::uint16_t expected = format == PixelFormat16::Rgb555 ? static_cast<std::uint16_t>(color & 0x7FFF) : color;
            Image source(transform.mLogicalWidth, transform.mLogicalHeight, transform.mLogicalWidth, color);
            Image target(transform.mPhysicalWidth, transform.mPhysicalHeight, transform.mPhysicalWidth, 0);
            Require(scaler.Scale(source.View(), target.MutableView(), transform, format) &&
                    std::all_of(target.mPixels.begin(), target.mPixels.end(), [&](std::uint16_t pixel) { return pixel == expected; }),
                std::string("uniform color stays uniform ") + FormatName(format));
        }
    }

    // 1 像素棋盘：1/2 档每盒两白两黑；1/3 档盒起点为白时五白四黑，否则四白五黑。
    const auto checker = [](int width, int height, std::uint16_t white) {
        Image image(width, height, width, 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                image.At(x, y) = (x + y) % 2 == 0 ? white : 0;
        return image;
    };
    {
        Image target(4, 3, 4, 0);
        Require(scaler.Scale(checker(8, 6, 0xFFFF).View(), target.MutableView(), {4, 3, 8, 6}, PixelFormat16::Rgb565) &&
                std::all_of(target.mPixels.begin(), target.mPixels.end(), [](std::uint16_t pixel) { return pixel == 0x8410; }),
            "565 checkerboard at 1/2 averages to (16, 32, 16)");
        Require(scaler.Scale(checker(8, 6, 0x7FFF).View(), target.MutableView(), {4, 3, 8, 6}, PixelFormat16::Rgb555) &&
                std::all_of(target.mPixels.begin(), target.mPixels.end(), [](std::uint16_t pixel) { return pixel == 0x4210; }),
            "555 checkerboard at 1/2 averages to (16, 16, 16)");
    }
    {
        // 五白：R (155+4)/9=17，G (315+4)/9=35，B 17 → 0x8C71；四白：R (124+4)/9=14，G (252+4)/9=28，B 14 → 0x738E。
        Image target(4, 3, 4, 0);
        Require(scaler.Scale(checker(12, 9, 0xFFFF).View(), target.MutableView(), {4, 3, 12, 9}, PixelFormat16::Rgb565),
            "565 checkerboard at 1/3 scales");
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                Require(target.At(x, y) == ((x + y) % 2 == 0 ? 0x8C71 : 0x738E), "565 checkerboard at 1/3 averages over 3x3");
    }
}

// 打包路径与宽路径的平均对全部可能的通道和都精确：盒长 n = 1..33（33 落入宽路径），逐个通道和穷举。
void TestExhaustiveRounding()
{
    BattlefieldScaler scaler;
    for (const PixelFormat16 format : FORMATS)
    {
        const std::uint32_t greenMax = format == PixelFormat16::Rgb565 ? 63 : 31;
        for (int count = 1; count <= 33; ++count)
        {
            const std::uint32_t greenTotal = greenMax * static_cast<std::uint32_t>(count);
            const std::uint32_t fiveTotal = 31u * static_cast<std::uint32_t>(count);
            Image source(count, 1, count, 0);
            Image target(1, 1, 1, 0);
            for (std::uint32_t sum = 0; sum <= greenTotal; ++sum)
            {
                const Channels totals{sum % (fiveTotal + 1), sum, fiveTotal - sum % (fiveTotal + 1)};
                Channels remaining = totals;
                for (int x = 0; x < count; ++x)
                {
                    const Channels pixel{std::min(remaining.mRed, 31u), std::min(remaining.mGreen, greenMax), std::min(remaining.mBlue, 31u)};
                    remaining = {remaining.mRed - pixel.mRed, remaining.mGreen - pixel.mGreen, remaining.mBlue - pixel.mBlue};
                    source.At(x, 0) = static_cast<std::uint16_t>(Encode(pixel, format) | (format == PixelFormat16::Rgb555 ? 0x8000 : 0));
                }
                const std::uint32_t half = static_cast<std::uint32_t>(count) / 2;
                const Channels expected{(totals.mRed + half) / count, (totals.mGreen + half) / count, (totals.mBlue + half) / count};
                if (!scaler.Scale(source.View(), target.MutableView(), {1, 1, count, 1}, format) ||
                    target.At(0, 0) != Encode(expected, format))
                    Require(false, std::string("exact rounded average ") + FormatName(format) + " n=" + std::to_string(count) +
                            " sum=" + std::to_string(sum));
            }
        }
    }
}

// 随机图像与独立参照逐像素一致：覆盖最近邻、打包、宽路径、单轴缩小另一轴放大、恒等与行距大于宽度。
void TestRandomAgainstReference()
{
    std::mt19937 random(424242u);
    std::vector<ZoomTransform> transforms{{7, 5, 13, 11}, {13, 11, 7, 5}, {16, 9, 48, 27}, {20, 10, 50, 25}, {30, 20, 24, 16},
        {10, 10, 10, 10}, {9, 7, 40, 3}, {9, 7, 3, 40}, {2, 3, 40, 50}, {1, 1, 6, 6}, {3, 2, 100, 1}, {64, 48, 16, 12},
        {97, 61, 24, 15}, {1, 1, 1, 1}, {5, 1, 1, 5}};
    std::uniform_int_distribution<int> physical(1, 40);
    std::uniform_int_distribution<int> logical(1, 130);
    for (int index = 0; index < 80; ++index)
        transforms.push_back({physical(random), physical(random), logical(random), logical(random)});

    BattlefieldScaler scaler;
    ScaleScratch scratch;
    std::uniform_int_distribution<int> padding(0, 5);
    for (const ZoomTransform& transform : transforms)
    {
        const SpanTable columns = BuildSpanTable(transform.mPhysicalWidth, transform.mLogicalWidth);
        const SpanTable rows = BuildSpanTable(transform.mPhysicalHeight, transform.mLogicalHeight);
        for (const PixelFormat16 format : FORMATS)
        {
            // 源与目标都可大于 L、P，行距可大于宽 × 2；多出的部分填入垃圾或哨兵。
            const int sourceWidth = transform.mLogicalWidth + padding(random);
            const int sourceHeight = transform.mLogicalHeight + padding(random);
            Image source(sourceWidth, sourceHeight, sourceWidth + padding(random), 0);
            FillRandom(source, random);
            const Image expected = ReferenceScale(source, transform, format);
            const int targetWidth = transform.mPhysicalWidth + padding(random);
            const int targetHeight = transform.mPhysicalHeight + padding(random);
            Image target(targetWidth, targetHeight, targetWidth + padding(random), 0xABCD);
            Require(scaler.Scale(source.View(), target.MutableView(), transform, format), "scaler accepts a valid call");
            Require(SameRegion(target, expected, transform.mPhysicalWidth, transform.mPhysicalHeight),
                std::string("matches reference ") + FormatName(format) + Describe(transform.mPhysicalWidth, transform.mLogicalWidth) +
                    Describe(transform.mPhysicalHeight, transform.mLogicalHeight));
            for (int y = 0; y < target.mHeight; ++y)
                for (int x = 0; x < target.mPitchPixels; ++x)
                    if (x >= transform.mPhysicalWidth || y >= transform.mPhysicalHeight)
                        Require(target.At(x, y) == 0xABCD, "pixels outside {0,0,P} and pitch padding are untouched");

            Image direct(transform.mPhysicalWidth, transform.mPhysicalHeight, transform.mPhysicalWidth, 0);
            Require(ScaleBattlefield(source.View(), direct.MutableView(), transform, format, columns, rows, scratch) &&
                    SameRegion(direct, expected, transform.mPhysicalWidth, transform.mPhysicalHeight),
                "free function with caller-owned tables matches reference");
        }
    }
}

// 放大即最近邻：每个物理像素等于其区间起点的逻辑像素（555 去掉最高位）。
void TestNearestMagnification()
{
    std::mt19937 random(5u);
    BattlefieldScaler scaler;
    for (const ZoomTransform& transform : {ZoomTransform{200, 120, 100, 60}, TransformFor(1752, 1048, 0), TransformFor(1752, 1048, 2),
             TransformFor(1752, 1048, 5)})
    {
        Image source(transform.mLogicalWidth, transform.mLogicalHeight, transform.mLogicalWidth, 0);
        FillRandom(source, random);
        for (const PixelFormat16 format : FORMATS)
        {
            Image target(transform.mPhysicalWidth, transform.mPhysicalHeight, transform.mPhysicalWidth, 0);
            Require(scaler.Scale(source.View(), target.MutableView(), transform, format), "magnification scales");
            const std::uint16_t mask = format == PixelFormat16::Rgb555 ? 0x7FFF : 0xFFFF;
            for (int y = 0; y < transform.mPhysicalHeight; ++y)
            {
                const int sourceY = SpanForPhysicalPixel(y, transform.mPhysicalHeight, transform.mLogicalHeight).mBegin;
                for (int x = 0; x < transform.mPhysicalWidth; ++x)
                {
                    const int sourceX = SpanForPhysicalPixel(x, transform.mPhysicalWidth, transform.mLogicalWidth).mBegin;
                    Require(target.At(x, y) == (source.At(sourceX, sourceY) & mask), "nearest neighbour ", FormatName(format));
                }
            }
        }
    }
}

// 参数不合法时返回 false，且目标逐字节不变。
void TestInvalidArguments()
{
    const ZoomTransform transform{4, 3, 12, 9};
    Image source(12, 9, 12, 0x1234);
    Image target(4, 3, 4, 0xABCD);
    const std::vector<std::uint16_t> original = target.mPixels;
    const SpanTable columns = BuildSpanTable(4, 12);
    const SpanTable rows = BuildSpanTable(3, 9);
    ScaleScratch scratch;
    const auto expectRejected = [&](const ConstPixelView16& sourceView, const PixelView16& targetView, const ZoomTransform& usedTransform,
                                    PixelFormat16 format, const SpanTable& usedColumns, const SpanTable& usedRows, const std::string& what) {
        Require(!ScaleBattlefield(sourceView, targetView, usedTransform, format, usedColumns, usedRows, scratch) &&
                target.mPixels == original,
            "rejects " + what);
    };
    const ConstPixelView16 good = source.View();
    const PixelView16 out = target.MutableView();
    expectRejected({nullptr, 12, 9, 24}, out, transform, PixelFormat16::Rgb565, columns, rows, "null source");
    expectRejected(good, {nullptr, 4, 3, 8}, transform, PixelFormat16::Rgb565, columns, rows, "null target");
    expectRejected({good.mData, 11, 9, 24}, out, transform, PixelFormat16::Rgb565, columns, rows, "source narrower than L");
    expectRejected({good.mData, 12, 8, 24}, out, transform, PixelFormat16::Rgb565, columns, rows, "source shorter than L");
    expectRejected(good, {out.mData, 3, 3, 8}, transform, PixelFormat16::Rgb565, columns, rows, "target narrower than P");
    expectRejected(good, {out.mData, 4, 2, 8}, transform, PixelFormat16::Rgb565, columns, rows, "target shorter than P");
    expectRejected({good.mData, 12, 9, 22}, out, transform, PixelFormat16::Rgb565, columns, rows, "pitch below width x 2");
    expectRejected(good, {out.mData, 4, 3, 9}, transform, PixelFormat16::Rgb565, columns, rows, "odd pitch");
    expectRejected(good, {out.mData, 4, 3, -8}, transform, PixelFormat16::Rgb565, columns, rows, "negative pitch");
    // 错位视图另用加宽的缓冲，即使误判为合法也不会越界读取。
    const Image wideSource(13, 9, 13, 0x1234);
    expectRejected({static_cast<const std::uint8_t*>(wideSource.View().mData) + 1, 12, 9, 26}, out, transform, PixelFormat16::Rgb565,
        columns, rows, "misaligned source");
    expectRejected(good, {static_cast<std::uint8_t*>(out.mData) + 1, 3, 3, 8}, {3, 3, 12, 9}, PixelFormat16::Rgb565,
        BuildSpanTable(3, 12), rows, "misaligned target");
    expectRejected(good, {out.mData, 4, INT_MAX, INT_MAX - 1}, transform, PixelFormat16::Rgb565, columns, rows, "view larger than the address space");
    expectRejected(good, out, {4, 3, 12, 0}, PixelFormat16::Rgb565, columns, rows, "invalid transform");
    expectRejected(good, out, {4, 3, 12, 8}, PixelFormat16::Rgb565, columns, rows, "row table built for another L");
    expectRejected(good, out, transform, PixelFormat16::Rgb565, BuildSpanTable(4, 11), rows, "column table built for another L");
    expectRejected(good, out, transform, PixelFormat16::Rgb565, SpanTable(), rows, "empty table");
    expectRejected(good, out, transform, static_cast<PixelFormat16>(7), columns, rows, "unknown pixel format");

    BattlefieldScaler scaler;
    Require(!scaler.Scale(good, out, ZoomTransform{}, PixelFormat16::Rgb565) && !scaler.Scale({nullptr, 12, 9, 24}, out, transform, PixelFormat16::Rgb565) &&
            target.mPixels == original,
        "scaler rejects invalid calls without writing");
    Require(scaler.Scale(good, out, transform, PixelFormat16::Rgb565) && target.At(0, 0) == 0x1234, "scaler recovers after a rejected call");
    scaler.Release();
    Require(scaler.Scale(good, out, transform, PixelFormat16::Rgb565), "scaler is reusable after release");
}

// 性能：只报告，不作为失败条件。2560×1440 窗口的战术区 2392×1408。
void RunPerformance()
{
#ifdef NDEBUG
    const char* configuration = "Release";
#else
    const char* configuration = "Debug";
#endif
    constexpr int FRAMES = 10;
    constexpr int PHYSICAL_WIDTH = 2392;
    constexpr int PHYSICAL_HEIGHT = 1408;
    std::cout << "Battlefield scale timing (" << configuration << ", RGB565, P=" << PHYSICAL_WIDTH << "x" << PHYSICAL_HEIGHT << ", "
              << FRAMES << " frames)\n";
    BattlefieldScaler scaler;
    for (const int index : {9, 10, FARTHEST_SHRINK_ZOOM_INDEX, 3})
    {
        const ZoomTransform transform = TransformFor(PHYSICAL_WIDTH, PHYSICAL_HEIGHT, index);
        Image source(transform.mLogicalWidth, transform.mLogicalHeight, transform.mLogicalWidth, 0);
        std::uint32_t seed = 12345u;
        for (std::uint16_t& pixel : source.mPixels)
        {
            seed = seed * 1664525u + 1013904223u;
            pixel = static_cast<std::uint16_t>(seed >> 16);
        }
        Image target(PHYSICAL_WIDTH, PHYSICAL_HEIGHT, PHYSICAL_WIDTH, 0);
        Require(scaler.Scale(source.View(), target.MutableView(), transform, PixelFormat16::Rgb565), "warm-up frame scales");
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < FRAMES; ++frame)
            Require(scaler.Scale(source.View(), target.MutableView(), transform, PixelFormat16::Rgb565), "timed frame scales");
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
        const ZoomLevel level = ZOOM_LEVELS[static_cast<std::size_t>(index)];
        std::cout << "  level " << level.mNumerator << "/" << level.mDenominator << " L=" << transform.mLogicalWidth << "x"
                  << transform.mLogicalHeight << ": " << std::fixed << std::setprecision(2) << elapsed.count() / FRAMES
                  << " ms/frame\n";
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--performance")
        {
            RunPerformance();
            return 0;
        }
        TestFloorDivide();
        TestSpanTables();
        TestMapping();
        TestTransform();
        TestLevels();
        TestResourceEstimate();
        TestPolicy();
        TestAnchor();
        TestGoldenPixels();
        TestUniformAndCheckerboard();
        TestExhaustiveRounding();
        TestRandomAgainstReference();
        TestNearestMagnification();
        TestInvalidArguments();
        std::cout << "Zoom tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
