#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstdint>

namespace rareplay::stats
{
/**
 * 一次间谍渗透敌方建筑（Hook I1，InfantryClass::UpdatePosition 调用 BuildingClass::Infiltrate 之前）的观察值。
 * 在 MO 中只有 Ares 判定为“间谍渗透敌方或中立建筑”时才会到达该点，所以天然排除工程师占领与 C4 破坏者。
 * 适配层在 Hook 回调里只复制这些整数与坐标，名称与图标之后在采样点解析。
 */
struct BuildingInfiltrationObservation
{
    std::uint32_t mFrame = 0;
    /** 间谍所属房屋；读不到时为 -1。 */
    int mSpyHouseIndex = -1;
    /** 被渗透建筑所属房屋；无主为 -1。 */
    int mBuildingHouseIndex = -1;
    /** 建筑类型在 BuildingTypeClass::Array 中的下标。 */
    int mBuildingTypeIndex = -1;
    viewer::WorldPoint mLocation;
};
}
