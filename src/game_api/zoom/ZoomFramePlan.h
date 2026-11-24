#pragma once

#include <array>
#include <cstdint>

namespace rareplay::game_api::zoom
{
/**
 * H1 读到的门控条件，由适配器在游戏线程读取后传入。全部满足才允许缩放窗口；
 * 第七项“资源就绪”不在这里，由 ZoomFrameInput::mAreResourcesReady 单独表达（不就绪时先重建资源）。
 */
struct ZoomGateState
{
    bool mHasTactical = false;
    /** TacticalClass::ZoomInFactor 按位等于 1.0；触发器演出改写它时不缩放。 */
    bool mIsZoomInFactorIdentity = false;
    bool mIsSidebarOnRight = false;
    bool mIsArmageddonMode = false;
    /** g_ViewBounds 的原点为 (0,0)。 */
    bool mIsViewOriginZero = false;
    /** GetRGBMode() 为 0（555）或 2（565），且本帧 Pframe 每像素 2 字节、宽高不小于 P。 */
    bool mIsFrameSurfaceSupported = false;
};

/** 门控全部满足（Armageddon 为假，其余为真）。 */
bool IsZoomGateOpen(const ZoomGateState& gate) noexcept;

/**
 * 一次 H1 的全部输入，只含值；由适配器在游戏线程按当前状态填写，判定本身不读任何全局状态，便于离线测试。
 * 档位都是 rareplay::zoom::ZOOM_LEVELS 的下标。
 */
struct ZoomFrameInput
{
    /** 适配器已启用且未因失败停用。 */
    bool mIsEnabled = false;
    ZoomGateState mGate;
    /** 现有逻辑资源（不小于 L 的 Composite/Tile、宽高恰为已应用 L 的 Z/A）能承载已应用档位；未激活时不看。 */
    bool mAreResourcesReady = false;
    /** 鼠标手势进行中（捕获、左键按下或拖框），换档推迟。 */
    bool mIsGestureActive = false;
    /** 原生镜头平移动画进行中（Tactical +0xD8 非 0），换档推迟。 */
    bool mIsCameraPanActive = false;
    /** 时空特效结束时的嵌套离屏重绘（byte 0xA9FA94 为 1）：照常进入窗口，换档推迟。 */
    bool mIsNestedOffscreenRedraw = false;
    /** 本帧物理战术区 P（窗口外的 g_ViewBounds 宽高）。 */
    int mPhysicalWidth = 0;
    int mPhysicalHeight = 0;
    /** 实时战术视口宽高（0xB0CE30/34）。 */
    int mViewportWidth = 0;
    int mViewportHeight = 0;
    /** 已应用档位；非 1× 即“激活”，此时战术视口应常驻 mAppliedLogical*。 */
    int mAppliedIndex = 0;
    /** 应用该档位时的 P 与写入视口的 L；未激活时不看。 */
    int mAppliedPhysicalWidth = 0;
    int mAppliedPhysicalHeight = 0;
    int mAppliedLogicalWidth = 0;
    int mAppliedLogicalHeight = 0;
    /** 观看者请求的档位。 */
    int mRequestedIndex = 0;
    /** 当前 P 下允许的档位范围（rareplay::zoom::ZoomLevelPolicy），含两端。 */
    int mMostMagnifiedIndex = 0;
    int mFarthestShrinkIndex = 0;
    /** Tactical +0xE0 VisibleCells 计数。 */
    int mVisibleCellCount = 0;
};

/** 本次 H1 的动作。 */
enum class ZoomFrameAction
{
    /** 不进入窗口，Hook 原样返回（未激活且无需激活）。 */
    KeepNative,
    /** 完整退回 1×：写回物理视口、全重绘，不进入窗口。 */
    ExitToIdentity,
    /** 先应用 mTargetIndex（换档、或因 P 变化、视口被原生复位、资源缺失而重新应用），再进入窗口，全重绘。 */
    ApplyLevelThenEnter,
    /** 已应用档位不变，直接进入窗口。 */
    Enter
};

struct ZoomFramePlan
{
    ZoomFrameAction mAction = ZoomFrameAction::KeepNative;
    /** 本帧之后的已应用档位。 */
    int mTargetIndex = 0;
    /** 是否置 H1 栈上的全重绘标志。 */
    bool mIsFullRedraw = false;
    /** 档位确实改变（需要按锚点重算镜头）；只因 P、视口或资源而重新应用时为假。 */
    bool mIsLevelChange = false;

    friend constexpr bool operator==(const ZoomFramePlan&, const ZoomFramePlan&) = default;
};

/**
 * 决定一次 H1 的动作：
 * - 未启用或门控失败：激活时完整退回 1×（不推迟），否则保持原生；
 * - 换档（含复原到 1×）在手势、镜头平移动画或嵌套离屏重绘期间推迟，沿用已应用档位；
 * - 激活时 P 变化、实时视口不等于已应用的 L（原生复位了视口）或资源不就绪：重新应用并全重绘；
 * - 直接进入时，VisibleCells 计数达到 VISIBLE_CELL_LIMIT 则全重绘。
 * 两个下标先夹到允许范围内（P 变小时范围可能收窄）。
 */
ZoomFramePlan PlanZoomFrame(const ZoomFrameInput& input) noexcept;

/** 从 index 向 1× 退一档；已是 1× 时不变。资源创建失败时据此退回更近的一档。 */
int StepTowardIdentity(int index) noexcept;

/** 战术区内的整数矩形 {X, Y, W, H}，单位为逻辑像素。 */
struct ZoomRect
{
    int mX = 0;
    int mY = 0;
    int mWidth = 0;
    int mHeight = 0;

    friend constexpr bool operator==(const ZoomRect&, const ZoomRect&) = default;
};

/** Hook E 本帧要在逻辑 Tile 上填黑的矩形，最多两块（竖带与横带）。 */
struct ExposedBands
{
    std::array<ZoomRect, 2> mRects{};
    int mCount = 0;
};

/**
 * Render(1) 本帧要重画的区域：全重绘时为整个逻辑视口 {0,0,L}；否则按滚屏量求暴露带（与 Render(1) 0x6D412F–0x6D443E 同式）：
 * dx = LastTacticalPos.X − TacticalPos.X，dx > 0 取左侧宽 |dx| 的竖带，dx < 0 取右侧；dy 同理取上方或下方的整宽横带
 * （角上与竖带重叠，重复填黑无副作用）。|dx| 或 |dy| 不小于 L 时按全重绘处理；宽高不为正时返回空。
 */
ExposedBands ComputeExposedBands(int scrollX, int scrollY, int logicalWidth, int logicalHeight, bool isFullRedraw) noexcept;

/**
 * extent × 0.5f 的单精度位模式，只用整数运算（位置音效 Hook 改写栈上的 float，不得触碰 x87 栈）。
 * 0 ≤ extent < 2^24 时与 static_cast<float>(extent) * 0.5f 逐位相同；负数按 0 处理，更大的值饱和到 2^24 − 1。
 */
std::uint32_t HalfExtentFloatBits(int extent) noexcept;

/** Tactical +0xD8（float）的位模式是否表示非 0（正负 0 都算 0）；与原生 DraggingInProgress 的 != 0.0f 判定一致。 */
constexpr bool IsNonZeroFloatBits(std::uint32_t bits) noexcept
{
    return (bits & 0x7FFFFFFFu) != 0;
}
}
