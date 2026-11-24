#include "../game_api/zoom/BattlefieldZoomAdapter.h"
#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"

#include <Syringe.h>

// 回放战局缩放的全部 Syringe Hook（地址、字节与寄存器见 TargetAbi.h）。
// 回调只读写寄存器与栈槽、调用 game_api::zoom 的入口，返回 0 或续点。缩放未激活（含录制）时一律返回 0，
// 由 Syringe 重放原指令，与原版逐位相同；规划模式的两处只看回放观看门控，1× 时同样生效。

// DEFINE_HOOK 的回调签名固定带 REGISTERS* R，只转交通知的回调不读寄存器。
#pragma warning(disable : 4100)

namespace
{
namespace zoom = rareplay::game_api::zoom;
namespace target = rareplay::target;
}

// ---------------- 缩放窗口（DrawOnTop 与 Render 阶段 2）----------------

// H1：门控、换档与进入窗口；需要全重绘时置 [ESP+8]（Render 三个阶段共用的重绘标志）。
DEFINE_HOOK(0x4F44CB, BattlefieldZoomDrawEnterHook, 5)
{
    if (zoom::OnDrawEnter())
        R->Stack8(target::DRAW_ON_TOP_REDRAW_FLAG_STACK, 1);
    return 0;
}

// E：EDI 只有低字节是 Render(0) 的重绘标志（高字节未初始化）。
DEFINE_HOOK(0x4F44E4, BattlefieldZoomTileExposedHook, 6)
{
    zoom::OnTileExposed((R->EDI() & 0xFFu) != 0);
    return 0;
}

// H2：恢复物理值，交给 TabClass::Draw。
DEFINE_HOOK(0x4F44F9, BattlefieldZoomTabDrawBeginHook, 6)
{
    zoom::OnTabDrawBegin();
    return 0;
}

// H3：再次换入逻辑值，交给 Render(2)。
DEFINE_HOOK(0x4F4505, BattlefieldZoomTabDrawEndHook, 6)
{
    zoom::OnTabDrawEnd();
    return 0;
}

// H4：EBX = 本帧 Render(2) 的 pSurface（已解锁）；缩放到 Pframe 并恢复物理值。
DEFINE_HOOK(0x6D4941, BattlefieldZoomWorldDrawnHook, 6)
{
    zoom::OnWorldLayerDrawn(reinterpret_cast<const void*>(R->EBX()));
    return 0;
}

// D：H4 未到达时的保险收尾。
DEFINE_HOOK(0x4F451B, BattlefieldZoomDrawTailHook, 5)
{
    zoom::OnDrawTail();
    return 0;
}

// ---------------- 窗口外的代入值 ----------------

// R2a：sub_6D2360 的宽度阈值恒为物理 P。
DEFINE_HOOK(0x6D2368, BattlefieldZoomSimulationWidthHook, 6)
{
    const auto width = zoom::GetSimulationViewportWidth();
    if (!width)
        return 0;
    R->ESI(*width);
    return static_cast<DWORD>(target::ZOOM_SIM_VIEWPORT_WIDTH_CONTINUE);
}

// R2b：sub_6D2360 的高度阈值恒为物理 P。
DEFINE_HOOK(0x6D237B, BattlefieldZoomSimulationHeightHook, 5)
{
    const auto height = zoom::GetSimulationViewportHeight();
    if (!height)
        return 0;
    R->EAX(*height);
    return static_cast<DWORD>(target::ZOOM_SIM_VIEWPORT_HEIGHT_CONTINUE);
}

// RV-a：RegisterCellAsVisible 按逻辑视图宽度登记可见格。
DEFINE_HOOK(0x6DA916, BattlefieldZoomVisibleCellWidthHook, 6)
{
    const auto width = zoom::GetVisibleCellViewWidth();
    if (!width)
        return 0;
    R->EDX(*width);
    return static_cast<DWORD>(target::ZOOM_VISIBLE_CELL_WIDTH_CONTINUE);
}

// RV-b：RegisterCellAsVisible 按逻辑视图高度登记可见格。
DEFINE_HOOK(0x6DA92A, BattlefieldZoomVisibleCellHeightHook, 6)
{
    const auto height = zoom::GetVisibleCellViewHeight();
    if (!height)
        return 0;
    R->ECX(*height);
    return static_cast<DWORD>(target::ZOOM_VISIBLE_CELL_HEIGHT_CONTINUE);
}

// CW：镜头 clamp 的宽度取 min(战术视口, P)。
DEFINE_HOOK(0x6D864E, BattlefieldZoomCameraClampWidthHook, 6)
{
    const auto width = zoom::GetCameraClampWidth();
    if (!width)
        return 0;
    R->EBX(*width);
    return static_cast<DWORD>(target::ZOOM_CAMERA_CLAMP_WIDTH_CONTINUE);
}

// CH：镜头 clamp 的高度取 min(战术视口, P)。
DEFINE_HOOK(0x6D868A, BattlefieldZoomCameraClampHeightHook, 6)
{
    const auto height = zoom::GetCameraClampHeight();
    if (!height)
        return 0;
    R->EBX(*height);
    return static_cast<DWORD>(target::ZOOM_CAMERA_CLAMP_HEIGHT_CONTINUE);
}

// RC1：雷达点击预夹紧的宽度取 min(战术视口, P)。
DEFINE_HOOK(0x653D92, BattlefieldZoomRadarClickWidthHook, 6)
{
    const auto width = zoom::GetCameraClampWidth();
    if (!width)
        return 0;
    R->ECX(*width);
    return static_cast<DWORD>(target::ZOOM_RADAR_CLICK_WIDTH_CONTINUE);
}

// RC2：雷达点击预夹紧的高度取 min(战术视口, P)；只换 EBX，其余中间值保持。
DEFINE_HOOK(0x653DAC, BattlefieldZoomRadarClickHeightHook, 6)
{
    const auto height = zoom::GetCameraClampHeight();
    if (!height)
        return 0;
    R->EBX(*height);
    return static_cast<DWORD>(target::ZOOM_RADAR_CLICK_HEIGHT_CONTINUE);
}

// RF1：雷达视框读取区间的入口，临时把 g_ViewBounds 宽高换成战术视口宽高。
DEFINE_HOOK(0x656F12, BattlefieldZoomRadarFrameBeginHook, 5)
{
    zoom::OnRadarFrameBegin();
    return 0;
}

// RF2：出口恢复；此处 x87 栈有 1 项，回调链只有整数运算。
DEFINE_HOOK(0x65707D, BattlefieldZoomRadarFrameEndHook, 5)
{
    zoom::OnRadarFrameEnd();
    return 0;
}

// AU：把栈上的声场半宽、半高（float）改写为战术视口的一半；只写位模式，不碰 x87。
DEFINE_HOOK(0x750B00, BattlefieldZoomSoundFieldHook, 6)
{
    zoom::AdjustSoundFieldHalfExtents(R->lea_Stack<void*>(target::SOUND_FIELD_HALF_WIDTH_STACK),
        R->lea_Stack<void*>(target::SOUND_FIELD_HALF_HEIGHT_STACK));
    return 0;
}

// ---------------- 三个战场输入入口：只替换 [ESP+4] 的指针，调用方的物理点不变 ----------------

DEFINE_HOOK(0x692300, BattlefieldZoomProcessClickHook, 7)
{
    if (const void* logical = zoom::MapClickPoint(R->Stack<const void*>(target::ZOOM_INPUT_POINT_STACK)))
        R->Stack(target::ZOOM_INPUT_POINT_STACK, logical);
    return 0;
}

DEFINE_HOOK(0x6D9F80, BattlefieldZoomDragBandBeginHook, 6)
{
    if (const void* logical = zoom::MapDragBandStart(R->Stack<const void*>(target::ZOOM_INPUT_POINT_STACK)))
        R->Stack(target::ZOOM_INPUT_POINT_STACK, logical);
    return 0;
}

DEFINE_HOOK(0x6D9FC0, BattlefieldZoomDragBandEndHook, 6)
{
    if (const void* logical = zoom::MapDragBandEnd(R->Stack<const void*>(target::ZOOM_INPUT_POINT_STACK)))
        R->Stack(target::ZOOM_INPUT_POINT_STACK, logical);
    return 0;
}

// ---------------- 规划模式：回放观看期间借用原生观察者分支 ----------------

// PL1：进入规划模式之前读观察者标志；EAX=1 时原生直接返回，不进入规划模式。
DEFINE_HOOK(0x731A50, ReplayPlanningModeEnterHook, 5)
{
    if (!rareplay::plugin::ShouldBlockPlayerCommands())
        return 0;
    R->EAX(1);
    return static_cast<DWORD>(target::PLANNING_MODE_ENTER_CONTINUE);
}

// PL2：退出规划模式之后读观察者标志；EAX=1 时跳过 Intro3 提示。
DEFINE_HOOK(0x731A78, ReplayPlanningModeExitNoticeHook, 5)
{
    if (!rareplay::plugin::ShouldBlockPlayerCommands())
        return 0;
    R->EAX(1);
    return static_cast<DWORD>(target::PLANNING_MODE_EXIT_NOTICE_CONTINUE);
}
