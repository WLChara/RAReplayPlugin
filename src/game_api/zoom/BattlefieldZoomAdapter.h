#pragma once

#include "../../zoom/ZoomLevels.h"
#include "../../zoom/ZoomMapping.h"

#include <optional>
#include <string>
#include <vector>

// 回放战局缩放的游戏适配器。
// 进程级单例，所有状态由游戏线程独占：逻辑 Composite/Tile/Z/A、窗口内保存的物理指针与 ViewBounds 宽高、常驻逻辑视口、
// 档位策略与缩放核、三个输入入口的逻辑坐标副本、雷达视框保存的 ViewBounds 与诊断计数。YRpp 只出现在实现文件里。
// 未启用（录制、正常对局）且未激活时，全部 Hook 入口直接返回，Hook 原样执行原指令。
// 全部函数 noexcept：任何异常只停用缩放（下一次 H1 退回 1×）并记一行日志，不影响回放。
namespace rareplay::game_api::zoom
{
/** 缩放不可用的原因。 */
enum class ZoomDisableReason
{
    None,
    /** 尚未 Enable，或插件已调用 Disable（录制、回放结束、失败或会话结束）。 */
    NotEnabled,
    /** 逻辑资源在所有更近的档位都创建失败。 */
    ResourceUnavailable,
    /** 缩放核拒绝参数，或像素格式在 H1 与 H4 之间改变。 */
    ScaleFailed,
    /** 窗口内全局指针与预期不符（例如逻辑面被别的代码换走）。 */
    UnexpectedEngineState,
    /** 适配器内部抛出异常。 */
    Exception
};

/** 给插件与 HUD 的缩放状态；档位都是 rareplay::zoom::ZOOM_LEVELS 的下标。 */
struct BattlefieldZoomStatus
{
    bool mIsEnabled = false;
    /** 已应用档位不是 1×：战术视口常驻逻辑尺寸，屏幕上是缩放后的画面。 */
    bool mIsActive = false;
    int mAppliedIndex = rareplay::zoom::IDENTITY_ZOOM_INDEX;
    int mRequestedIndex = rareplay::zoom::IDENTITY_ZOOM_INDEX;
    /** 当前物理战术区下允许的范围，含两端；物理尺寸尚未读到时都是 1×。 */
    int mMostMagnifiedIndex = rareplay::zoom::IDENTITY_ZOOM_INDEX;
    int mFarthestShrinkIndex = rareplay::zoom::IDENTITY_ZOOM_INDEX;
    ZoomDisableReason mDisableReason = ZoomDisableReason::NotEnabled;
    /**
     * 最近一次读取门控的 H1 发现门控关闭（侧栏在左、Armageddon、触发器演出改写了 ZoomInFactor、像素格式不支持等，
     * 见 IsZoomGateOpen）。只读，不影响缩放行为。H1 只在缩放激活或有换档请求时读取门控，
     * 1× 且没有请求时保持上一次的结果；Enable 时清为假。
     */
    bool mIsGateClosed = false;
    /**
     * 屏幕上当前那一帧的映射：物理战术区 P 与逻辑视口 L。未激活时 L == P（恒等）；物理尺寸尚未读到时全为 0（不合法）。
     * 叠加图标用它的 MapLogicalToPhysical 把战术区内的逻辑坐标换算为物理坐标。
     */
    rareplay::zoom::ZoomTransform mTransform;
};

// ---------------- 插件接口：只在游戏线程、DrawOnTop 之外调用 ----------------

/** 回放开始时调用：允许缩放，请求档位复原为 1×，清空停用原因与诊断计数。资源在首次离开 1× 时惰性创建。 */
void Enable() noexcept;

/**
 * 回放结束、失败或会话结束时调用：写回物理视口（视口仍为本适配器写入的 L 时）、释放全部逻辑资源，
 * 并令下一次 H1 全重绘（物理 Tile 在缩放期间没有更新）；之后全部入口直接返回。重复调用无副作用。
 * 若意外在缩放窗口内被调用，则只标记停用，由下一次 H1 完成同样的收尾。
 */
void Disable() noexcept;

/** 换档时锚点（缩放前后保持不动的地面点）的取法。 */
enum class ZoomStepAnchor
{
    /** 应用时鼠标指着的地面点（Ctrl+滚轮）；鼠标不在物理战术区内时退回战术区中心。 */
    Pointer,
    /** 战术区中心（HUD 的－/＋按钮：点击时鼠标在底栏上，不能作锚点）。 */
    Center
};

/**
 * 换档（steps 正数放大）；结果夹在允许范围内，在下一次 H1 按 anchor 取锚点应用。
 * 多次请求在同一次 H1 之前累计档数，锚点取最后一次请求的。未启用时忽略。
 */
void RequestStep(int steps, ZoomStepAnchor anchor) noexcept;

/** 请求复原到 1×，在下一次 H1 以战术区中心为锚点应用。未启用时忽略。 */
void RequestReset() noexcept;

BattlefieldZoomStatus GetStatus() noexcept;

/** 取出待写的技术日志行（英文），由插件写入日志；最多保留 64 行，多出的只计数，在汇总行中报告。 */
std::vector<std::string> TakeLogLines() noexcept;

/**
 * 叠加图标：把 TacticalClass::CoordsToClient 得到的战术区内逻辑坐标换算为物理坐标（逻辑到物理的正向映射）。
 * 未激活或战术视口已被原生复位（不再等于本适配器写入的 L）时原样返回。
 */
rareplay::zoom::ZoomPoint MapViewPointToPhysical(rareplay::zoom::ZoomPoint logicalPoint) noexcept;

// ---------------- Hook 入口：只由 src/hooks/BattlefieldZoomHooks.cpp 在游戏线程同步调用 ----------------

/** H1：门控、换档（含锚点）、进入窗口；门控失败而缩放激活时完整退回 1×。返回 true 时 Hook 置栈上的全重绘标志。 */
bool OnDrawEnter() noexcept;
/** E：isRedrawRequested 为 EDI 的低字节是否非 0；把当前 g_TileSurfaceSlot 的暴露带或整面填黑。 */
void OnTileExposed(bool isRedrawRequested) noexcept;
/** H2：记下当前逻辑 Composite/Tile（Render(0) 可能已交换），恢复全部物理值。 */
void OnTabDrawBegin() noexcept;
/** H3：再次换入逻辑值。 */
void OnTabDrawEnd() noexcept;
/** H4：renderSurface 为 EBX（本帧 Render(2) 的 pSurface）；逻辑 Composite 缩放到 Pframe，恢复物理值，退出窗口。 */
void OnWorldLayerDrawn(const void* renderSurface) noexcept;
/** D：窗口仍激活（H4 未到达）时做与 H4 相同的收尾。 */
void OnDrawTail() noexcept;

/** R2：缩放激活时返回物理 P 的宽或高（代入 sub_6D2360 的阈值），否则为空（Hook 返回 0）。 */
std::optional<int> GetSimulationViewportWidth() noexcept;
std::optional<int> GetSimulationViewportHeight() noexcept;
/** RV：缩放激活时返回当时的战术视口宽或高（RegisterCellAsVisible 覆盖逻辑视图），否则为空。 */
std::optional<int> GetVisibleCellViewWidth() noexcept;
std::optional<int> GetVisibleCellViewHeight() noexcept;
/** CW/CH 与 RC1/RC2：缩放激活时返回 min(当时的战术视口, 物理 P) 的宽或高，否则为空。 */
std::optional<int> GetCameraClampWidth() noexcept;
std::optional<int> GetCameraClampHeight() noexcept;
/** RF1：缩放激活时保存 g_ViewBounds 宽高并换成战术视口宽高；RF2 恢复。只用整数，不碰 x87。 */
void OnRadarFrameBegin() noexcept;
void OnRadarFrameEnd() noexcept;
/**
 * AU：缩放激活时把两个栈槽（float）改写为战术视口宽、高 × 0.5f，只用整数位运算，不碰 x87。
 * 参数为 Hook 处 [ESP+0x20]、[ESP+0x24] 的地址，不得为空。
 */
void AdjustSoundFieldHalfExtents(void* halfWidthSlot, void* halfHeightSlot) noexcept;
/**
 * PC/BS/BE：physicalPoint 指向战术区相对物理点（Point2D）。缩放激活且视口未被复位时，返回本入口专用的逻辑坐标副本地址
 * （有效到下一次调用同一入口），Hook 用它替换 [ESP+4]；否则返回空（Hook 不改栈）。不改写 physicalPoint 指向的值。
 */
const void* MapClickPoint(const void* physicalPoint) noexcept;
const void* MapDragBandStart(const void* physicalPoint) noexcept;
const void* MapDragBandEnd(const void* physicalPoint) noexcept;
}
