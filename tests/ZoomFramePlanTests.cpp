#include "../src/game_api/zoom/ZoomFramePlan.h"
#include "../src/target/TargetAbi.h"
#include "../src/zoom/ZoomLevels.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::game_api::zoom;
using rareplay::zoom::FARTHEST_SHRINK_ZOOM_INDEX;
using rareplay::zoom::IDENTITY_ZOOM_INDEX;
using rareplay::zoom::MOST_MAGNIFIED_ZOOM_INDEX;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// 下标 3 为 2×，9 为 1/2。1920×1080 窗口的战术区 1752×1048：2× 时 L=876×524。
constexpr int TWO_X = 3;
constexpr int HALF_X = 9;

ZoomGateState OpenGate()
{
    ZoomGateState gate;
    gate.mHasTactical = true;
    gate.mIsZoomInFactorIdentity = true;
    gate.mIsSidebarOnRight = true;
    gate.mIsArmageddonMode = false;
    gate.mIsViewOriginZero = true;
    gate.mIsFrameSurfaceSupported = true;
    return gate;
}

// 1× 未激活、门控全开、无手势的基准输入。
ZoomFrameInput IdleInput()
{
    ZoomFrameInput input;
    input.mIsEnabled = true;
    input.mGate = OpenGate();
    input.mAreResourcesReady = false;
    input.mPhysicalWidth = 1752;
    input.mPhysicalHeight = 1048;
    input.mViewportWidth = 1752;
    input.mViewportHeight = 1048;
    input.mAppliedIndex = IDENTITY_ZOOM_INDEX;
    input.mRequestedIndex = IDENTITY_ZOOM_INDEX;
    input.mMostMagnifiedIndex = MOST_MAGNIFIED_ZOOM_INDEX;
    input.mFarthestShrinkIndex = FARTHEST_SHRINK_ZOOM_INDEX;
    return input;
}

// 已在 2× 稳定运行：视口常驻 L，资源就绪，请求与已应用一致。
ZoomFrameInput ActiveInput()
{
    ZoomFrameInput input = IdleInput();
    input.mAreResourcesReady = true;
    input.mViewportWidth = 876;
    input.mViewportHeight = 524;
    input.mAppliedIndex = TWO_X;
    input.mAppliedPhysicalWidth = 1752;
    input.mAppliedPhysicalHeight = 1048;
    input.mAppliedLogicalWidth = 876;
    input.mAppliedLogicalHeight = 524;
    input.mRequestedIndex = TWO_X;
    return input;
}

ZoomFramePlan Plan(ZoomFrameAction action, int target, bool isFullRedraw, bool isLevelChange)
{
    ZoomFramePlan plan;
    plan.mAction = action;
    plan.mTargetIndex = target;
    plan.mIsFullRedraw = isFullRedraw;
    plan.mIsLevelChange = isLevelChange;
    return plan;
}

const ZoomFramePlan KEEP_NATIVE = Plan(ZoomFrameAction::KeepNative, IDENTITY_ZOOM_INDEX, false, false);
const ZoomFramePlan FORCED_EXIT = Plan(ZoomFrameAction::ExitToIdentity, IDENTITY_ZOOM_INDEX, true, false);
const ZoomFramePlan STEADY_ENTER = Plan(ZoomFrameAction::Enter, TWO_X, false, false);

void TestGateConditions()
{
    Require(IsZoomGateOpen(OpenGate()), "all gate conditions open");
    using Mutator = void (*)(ZoomGateState&);
    const Mutator closers[] = {
        [](ZoomGateState& gate) { gate.mHasTactical = false; },
        [](ZoomGateState& gate) { gate.mIsZoomInFactorIdentity = false; },
        [](ZoomGateState& gate) { gate.mIsSidebarOnRight = false; },
        [](ZoomGateState& gate) { gate.mIsArmageddonMode = true; },
        [](ZoomGateState& gate) { gate.mIsViewOriginZero = false; },
        [](ZoomGateState& gate) { gate.mIsFrameSurfaceSupported = false; },
    };
    int index = 0;
    for (const Mutator close : closers)
    {
        const std::string which = " (gate condition " + std::to_string(index++) + ")";
        ZoomGateState gate = OpenGate();
        close(gate);
        Require(!IsZoomGateOpen(gate), "single closed condition closes the gate" + which);

        // 未激活：请求放大也不进入。
        ZoomFrameInput idle = IdleInput();
        idle.mGate = gate;
        idle.mRequestedIndex = TWO_X;
        Require(PlanZoomFrame(idle) == KEEP_NATIVE, "closed gate keeps 1x native" + which);

        // 激活：完整退回 1× 并全重绘，不按锚点（不是换档）。
        ZoomFrameInput active = ActiveInput();
        active.mGate = gate;
        Require(PlanZoomFrame(active) == FORCED_EXIT, "closed gate while zoomed exits to 1x" + which);

        // 门控失败的退出不因手势或平移动画推迟。
        active.mIsGestureActive = true;
        active.mIsCameraPanActive = true;
        Require(PlanZoomFrame(active) == FORCED_EXIT, "closed gate exit is never deferred" + which);
    }

    ZoomFrameInput invalidPhysical = ActiveInput();
    invalidPhysical.mPhysicalWidth = 0;
    Require(PlanZoomFrame(invalidPhysical) == FORCED_EXIT, "invalid physical size exits to 1x");
    invalidPhysical = IdleInput();
    invalidPhysical.mPhysicalHeight = -5;
    invalidPhysical.mRequestedIndex = TWO_X;
    Require(PlanZoomFrame(invalidPhysical) == KEEP_NATIVE, "invalid physical size keeps native when idle");

    // 门控恢复后在下一次 H1 重新进入：此时已退回 1×，请求仍是 2×。
    ZoomFrameInput reopened = IdleInput();
    reopened.mRequestedIndex = TWO_X;
    Require(PlanZoomFrame(reopened) == Plan(ZoomFrameAction::ApplyLevelThenEnter, TWO_X, true, true),
        "reopened gate re-enters the requested level");
}

void TestDisabled()
{
    ZoomFrameInput idle = IdleInput();
    idle.mIsEnabled = false;
    idle.mRequestedIndex = TWO_X;
    Require(PlanZoomFrame(idle) == KEEP_NATIVE, "disabled adapter never enters");

    ZoomFrameInput active = ActiveInput();
    active.mIsEnabled = false;
    Require(PlanZoomFrame(active) == FORCED_EXIT, "disabled while zoomed exits to 1x");
    active.mIsGestureActive = true;
    Require(PlanZoomFrame(active) == FORCED_EXIT, "disable exit is not deferred by gestures");

    // 退回 1× 之后（已应用 1×）停用状态下保持原生。
    ZoomFrameInput afterExit = IdleInput();
    afterExit.mIsEnabled = false;
    afterExit.mRequestedIndex = HALF_X;
    afterExit.mVisibleCellCount = rareplay::target::VISIBLE_CELL_LIMIT;
    Require(PlanZoomFrame(afterExit) == KEEP_NATIVE, "disabled adapter stays native after exiting");
}

void TestLevelChanges()
{
    Require(PlanZoomFrame(IdleInput()) == KEEP_NATIVE, "1x request at 1x keeps native");

    ZoomFrameInput enter = IdleInput();
    enter.mRequestedIndex = HALF_X;
    Require(PlanZoomFrame(enter) == Plan(ZoomFrameAction::ApplyLevelThenEnter, HALF_X, true, true),
        "first entry applies the requested level with an anchor");

    ZoomFrameInput change = ActiveInput();
    change.mRequestedIndex = HALF_X;
    Require(PlanZoomFrame(change) == Plan(ZoomFrameAction::ApplyLevelThenEnter, HALF_X, true, true),
        "changing level while zoomed reapplies with an anchor");

    ZoomFrameInput reset = ActiveInput();
    reset.mRequestedIndex = IDENTITY_ZOOM_INDEX;
    Require(PlanZoomFrame(reset) == Plan(ZoomFrameAction::ExitToIdentity, IDENTITY_ZOOM_INDEX, true, true),
        "requesting 1x exits with an anchor");

    Require(PlanZoomFrame(ActiveInput()) == STEADY_ENTER, "steady zoom enters without a full redraw");

    // 请求超出允许范围时夹到范围内；P 变小时已应用档位也夹到范围内。
    ZoomFrameInput clamped = IdleInput();
    clamped.mFarthestShrinkIndex = HALF_X;
    clamped.mRequestedIndex = FARTHEST_SHRINK_ZOOM_INDEX;
    Require(PlanZoomFrame(clamped).mTargetIndex == HALF_X, "request is clamped to the farthest allowed level");
    clamped.mRequestedIndex = -7;
    Require(PlanZoomFrame(clamped).mTargetIndex == MOST_MAGNIFIED_ZOOM_INDEX, "request is clamped to the most magnified level");

    ZoomFrameInput noMagnify = IdleInput();
    noMagnify.mMostMagnifiedIndex = IDENTITY_ZOOM_INDEX;
    noMagnify.mRequestedIndex = TWO_X;
    Require(PlanZoomFrame(noMagnify) == KEEP_NATIVE, "magnification outside the allowed range stays at 1x");
}

void TestDeferral()
{
    using Mutator = void (*)(ZoomFrameInput&);
    const Mutator deferrers[] = {
        [](ZoomFrameInput& input) { input.mIsGestureActive = true; },
        [](ZoomFrameInput& input) { input.mIsCameraPanActive = true; },
        [](ZoomFrameInput& input) { input.mIsNestedOffscreenRedraw = true; },
    };
    int index = 0;
    for (const Mutator defer : deferrers)
    {
        const std::string which = " (deferral " + std::to_string(index++) + ")";
        ZoomFrameInput idle = IdleInput();
        idle.mRequestedIndex = TWO_X;
        defer(idle);
        Require(PlanZoomFrame(idle) == KEEP_NATIVE, "zooming in from 1x is deferred" + which);

        ZoomFrameInput change = ActiveInput();
        change.mRequestedIndex = HALF_X;
        defer(change);
        Require(PlanZoomFrame(change) == STEADY_ENTER, "level change keeps the applied level while deferred" + which);

        ZoomFrameInput reset = ActiveInput();
        reset.mRequestedIndex = IDENTITY_ZOOM_INDEX;
        defer(reset);
        Require(PlanZoomFrame(reset) == STEADY_ENTER, "reset to 1x is deferred too" + which);

        // 条件消失后应用累计的请求。
        ZoomFrameInput released = ActiveInput();
        released.mRequestedIndex = HALF_X;
        Require(PlanZoomFrame(released) == Plan(ZoomFrameAction::ApplyLevelThenEnter, HALF_X, true, true),
            "deferred request applies once the condition clears" + which);
    }
}

void TestPhysicalAndViewportChanges()
{
    ZoomFrameInput resized = ActiveInput();
    resized.mPhysicalWidth = 2392;
    resized.mPhysicalHeight = 1408;
    Require(PlanZoomFrame(resized) == Plan(ZoomFrameAction::ApplyLevelThenEnter, TWO_X, true, false),
        "physical size change reapplies the level without an anchor");
    // 原生复位视口也会发生在 P 变化时；手势进行中仍需重新应用（旧映射已失效）。
    resized.mViewportWidth = 2392;
    resized.mViewportHeight = 1408;
    resized.mIsGestureActive = true;
    Require(PlanZoomFrame(resized) == Plan(ZoomFrameAction::ApplyLevelThenEnter, TWO_X, true, false),
        "physical change reapplies even during a gesture");

    ZoomFrameInput reset = ActiveInput();
    reset.mViewportWidth = 1752;
    reset.mViewportHeight = 1048;
    Require(PlanZoomFrame(reset) == Plan(ZoomFrameAction::ApplyLevelThenEnter, TWO_X, true, false),
        "viewport reset by the engine is detected and the level reapplied");
    reset = ActiveInput();
    reset.mViewportHeight = 525;
    Require(PlanZoomFrame(reset).mAction == ZoomFrameAction::ApplyLevelThenEnter, "any viewport axis mismatch reapplies");

    ZoomFrameInput missing = ActiveInput();
    missing.mAreResourcesReady = false;
    Require(PlanZoomFrame(missing) == Plan(ZoomFrameAction::ApplyLevelThenEnter, TWO_X, true, false),
        "missing resources are rebuilt before entering");

    // 范围收窄使已应用档位越界：按夹紧后的档位重新应用（视为换档）。
    ZoomFrameInput narrowed = ActiveInput();
    narrowed.mAppliedIndex = FARTHEST_SHRINK_ZOOM_INDEX;
    narrowed.mRequestedIndex = FARTHEST_SHRINK_ZOOM_INDEX;
    narrowed.mFarthestShrinkIndex = HALF_X;
    narrowed.mIsGestureActive = true;
    Require(PlanZoomFrame(narrowed) == Plan(ZoomFrameAction::ApplyLevelThenEnter, HALF_X, true, true),
        "applied level outside a narrowed range is clamped");
}

void TestVisibleCells()
{
    ZoomFrameInput input = ActiveInput();
    input.mVisibleCellCount = rareplay::target::VISIBLE_CELL_LIMIT - 1;
    Require(PlanZoomFrame(input) == STEADY_ENTER, "798 visible cells enter incrementally");
    input.mVisibleCellCount = rareplay::target::VISIBLE_CELL_LIMIT;
    Require(PlanZoomFrame(input) == Plan(ZoomFrameAction::Enter, TWO_X, true, false), "799 visible cells force a full redraw");
    input.mVisibleCellCount = 5000;
    Require(PlanZoomFrame(input).mIsFullRedraw, "counts above the limit force a full redraw");

    ZoomFrameInput idle = IdleInput();
    idle.mVisibleCellCount = rareplay::target::VISIBLE_CELL_LIMIT;
    Require(PlanZoomFrame(idle) == KEEP_NATIVE, "visible cell overflow at 1x stays native");
}

void TestNestedOffscreenRedraw()
{
    ZoomFrameInput nested = ActiveInput();
    nested.mIsNestedOffscreenRedraw = true;
    Require(PlanZoomFrame(nested) == STEADY_ENTER, "nested offscreen redraw enters the window as usual");
    nested.mVisibleCellCount = rareplay::target::VISIBLE_CELL_LIMIT;
    Require(PlanZoomFrame(nested) == Plan(ZoomFrameAction::Enter, TWO_X, true, false),
        "nested redraw still honours the visible cell limit");
    nested = ActiveInput();
    nested.mIsNestedOffscreenRedraw = true;
    nested.mViewportWidth = 1752;
    Require(PlanZoomFrame(nested).mAction == ZoomFrameAction::ApplyLevelThenEnter, "nested redraw reapplies a reset viewport");
}

void TestStepTowardIdentity()
{
    Require(StepTowardIdentity(0) == 1 && StepTowardIdentity(5) == IDENTITY_ZOOM_INDEX &&
            StepTowardIdentity(IDENTITY_ZOOM_INDEX) == IDENTITY_ZOOM_INDEX && StepTowardIdentity(7) == IDENTITY_ZOOM_INDEX &&
            StepTowardIdentity(FARTHEST_SHRINK_ZOOM_INDEX) == FARTHEST_SHRINK_ZOOM_INDEX - 1,
        "fallback steps one level toward 1x");
}

void TestExposedBands()
{
    const auto full = ComputeExposedBands(5, 3, 3504, 2096, true);
    Require(full.mCount == 1 && full.mRects[0] == ZoomRect{0, 0, 3504, 2096}, "full redraw blacks out the whole logical view");
    Require(ComputeExposedBands(0, 0, 3504, 2096, false).mCount == 0, "no scroll exposes nothing");

    const auto right = ComputeExposedBands(12, 0, 3504, 2096, false);
    Require(right.mCount == 1 && right.mRects[0] == ZoomRect{0, 0, 12, 2096}, "dx > 0 exposes the left band");
    const auto left = ComputeExposedBands(-12, 0, 3504, 2096, false);
    Require(left.mCount == 1 && left.mRects[0] == ZoomRect{3492, 0, 12, 2096}, "dx < 0 exposes the right band");
    const auto top = ComputeExposedBands(0, 7, 3504, 2096, false);
    Require(top.mCount == 1 && top.mRects[0] == ZoomRect{0, 0, 3504, 7}, "dy > 0 exposes the top band");
    const auto bottom = ComputeExposedBands(0, -7, 3504, 2096, false);
    Require(bottom.mCount == 1 && bottom.mRects[0] == ZoomRect{0, 2089, 3504, 7}, "dy < 0 exposes the bottom band");
    const auto both = ComputeExposedBands(-30, 15, 876, 524, false);
    Require(both.mCount == 2 && both.mRects[0] == ZoomRect{846, 0, 30, 524} && both.mRects[1] == ZoomRect{0, 0, 876, 15},
        "diagonal scroll exposes a column and a row");

    Require(ComputeExposedBands(876, 0, 876, 524, false).mRects[0] == ZoomRect{0, 0, 876, 524} &&
            ComputeExposedBands(0, -524, 876, 524, false).mCount == 1 &&
            ComputeExposedBands(std::numeric_limits<int>::min(), 0, 876, 524, false).mRects[0] == ZoomRect{0, 0, 876, 524},
        "a scroll of at least the view size is a full redraw");
    Require(ComputeExposedBands(5, 5, 0, 524, true).mCount == 0 && ComputeExposedBands(5, 5, 876, -1, false).mCount == 0,
        "invalid logical sizes expose nothing");
}

void TestHalfExtentFloatBits()
{
    for (int extent = 0; extent <= 200000; ++extent)
    {
        const float expected = static_cast<float>(extent) * 0.5f;
        Require(HalfExtentFloatBits(extent) == std::bit_cast<std::uint32_t>(expected),
            "half extent bits match float arithmetic for " + std::to_string(extent));
    }
    for (const int extent : {(1 << 24) - 1, (1 << 23) + 1, 16777215, 8388607, 32767, 7176, 4224})
        Require(HalfExtentFloatBits(extent) == std::bit_cast<std::uint32_t>(static_cast<float>(extent) * 0.5f),
            "half extent bits match for large extent " + std::to_string(extent));
    Require(HalfExtentFloatBits(-1) == 0 && HalfExtentFloatBits(std::numeric_limits<int>::min()) == 0, "negative extents give +0");
    Require(HalfExtentFloatBits(std::numeric_limits<int>::max()) == HalfExtentFloatBits((1 << 24) - 1),
        "extents above 2^24 saturate");
}

void TestFloatBits()
{
    Require(!IsNonZeroFloatBits(std::bit_cast<std::uint32_t>(0.0f)) && !IsNonZeroFloatBits(std::bit_cast<std::uint32_t>(-0.0f)),
        "both zeros mean no camera pan");
    Require(IsNonZeroFloatBits(std::bit_cast<std::uint32_t>(1.0f)) && IsNonZeroFloatBits(std::bit_cast<std::uint32_t>(-0.25f)) &&
            IsNonZeroFloatBits(std::bit_cast<std::uint32_t>(std::numeric_limits<float>::denorm_min())) &&
            IsNonZeroFloatBits(std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN())),
        "any non-zero value means a camera pan, as != 0.0f in the engine");
}
}

int main()
{
    try
    {
        TestGateConditions();
        TestDisabled();
        TestLevelChanges();
        TestDeferral();
        TestPhysicalAndViewportChanges();
        TestVisibleCells();
        TestNestedOffscreenRedraw();
        TestStepTowardIdentity();
        TestExposedBands();
        TestHalfExtentFloatBits();
        TestFloatBits();
    }
    catch (const std::exception& error)
    {
        std::cerr << "ZoomFramePlanTests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ZoomFramePlanTests passed\n";
    return 0;
}
