#pragma once

#include "../../stats/InfiltrationObservation.h"
#include "../../stats/SuperWeaponLaunch.h"

namespace rareplay::game_api
{
/**
 * Hook 回调里的只读观察：入参是 Hook 寄存器与栈上的原生对象地址，只复制整数与坐标到值类型，
 * 不分配游戏对象、不调用有副作用的函数、不解析名称与图标（那些在采样点由 RadarEventResolver 做）。
 * 只在游戏线程调用。任何指针为空、对象字段不合理时返回 false，调用方忽略这次观察。
 */

/**
 * Fire_SW 调用 ClickFire 之前（Hook C1）：house = EBX，super = EDI，prerequisiteSuper = ESI（可为空），
 * targetCell = [ESP+0x18] 处的 CellStruct 指针（立即复制，AI 路径的格位于 Ares 栈上）。
 */
bool ReadSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
    const void* targetCell, stats::SuperWeaponFireBegin& out) noexcept;

/** ClickFire 返回之后（Hook C2）：super = EDI。 */
bool ReadSuperWeaponFireEnd(const void* super, stats::SuperWeaponFireEnd& out) noexcept;

/** 间谍调用 BuildingClass::Infiltrate 之前（Hook I1）：spy = ESI（InfantryClass*），building = EDI（BuildingClass*）。 */
bool ReadBuildingInfiltration(const void* spy, const void* building,
    stats::BuildingInfiltrationObservation& out) noexcept;
}
