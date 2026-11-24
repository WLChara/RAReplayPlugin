#pragma once

#include "../../viewer/SuperWeaponMarks.h"

#include <optional>

namespace rareplay::game_api
{
/** 战术视图（地图区）在游戏画布中的几何；坐标原点为 WindowBounds 左上角，与 NativeUiLayout 同一坐标系。 */
struct TacticalViewGeometry
{
    int mCanvasWidth = 0;
    int mCanvasHeight = 0;
    /** DSurface::ViewBounds 减去 WindowBounds 原点。 */
    viewer::NativeRect mView;
};

/**
 * 读取战术视图几何；画布或视图尺寸无效、视图不在画布内时返回空。只在游戏线程调用，只读。
 */
std::optional<TacticalViewGeometry> ReadTacticalViewGeometry() noexcept;

/**
 * 世界坐标换算到游戏画布坐标：调用游戏自己的 TacticalClass::CoordsToClient（0x6D2140），
 * 结果是相对战术视口左上角的坐标，再加上视图在画布中的起点。结果可以落在视图之外（调用方负责裁剪）。
 * 战局缩放激活时先按屏幕上那一帧的映射把逻辑坐标换算为物理坐标，所以返回值始终是物理画布坐标；
 * geometry 须在缩放窗口外读取（窗口内 g_ViewBounds 的宽高是逻辑尺寸）。
 *
 * IDA 核实：该函数只读入参坐标、TacticalPos
 * 与两个视口尺寸全局量，只写调用方给的输出点，内部调用的 Game::F2I 是纯换算，没有任何副作用。
 * TacticalClass 尚未创建时返回空。只在游戏线程调用。
 */
std::optional<viewer::CanvasPoint> ProjectWorldToCanvas(const viewer::WorldPoint& world,
    const TacticalViewGeometry& geometry) noexcept;

/**
 * 把本地镜头移到该世界坐标（视口中心对准它，并按地图可见范围夹紧）。调用 TacticalClass::SetTacticalPosition
 * （0x6D6070），与雷达点击、View1–4 热键使用同一函数：只改变 TacticalClass 的镜头字段，
 * 不产生事件、不影响模拟。TacticalClass 尚未创建时返回 false。只在游戏线程、不在模拟更新中途调用。
 */
bool CenterTacticalViewOn(const viewer::WorldPoint& world) noexcept;
}
