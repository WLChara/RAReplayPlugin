#include "RadarEventResolver.h"

#include "../HouseRoster.h"
#include "../names/NativeDisplayNames.h"

using byte = unsigned char;

#include <BuildingTypeClass.h>
#include <CCINIClass.h>
#include <CellClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <SuperWeaponTypeClass.h>

#include <cstddef>
#include <string>
#include <utility>

namespace rareplay::game_api
{
namespace
{
// 一格的莱普顿数与格中心的次级坐标：与原生 Launch（0x6CC3D6）和 Ares 的 Activate（0x10033027）一致，
// 世界坐标 X = 格X × 256 + 128，Y 同理，Z = GetFloorHeight(格中心)，有桥时再加 BridgeHeight（4 层 × 104 = 416）。
constexpr int LEPTONS_PER_CELL = 256;
constexpr int CELL_CENTER_OFFSET = 128;
static_assert(CellClass::BridgeHeight == 416);
static_assert(offsetof(CellClass, Flags) == 0x140);

const SuperWeaponTypeClass* GetSuperWeaponType(int index) noexcept
{
    const auto& array = SuperWeaponTypeClass::Array;
    if (index < 0 || index >= array.Count || !array.Items)
        return nullptr;
    return array.Items[index];
}

const BuildingTypeClass* GetBuildingType(int index) noexcept
{
    const auto& array = BuildingTypeClass::Array;
    if (index < 0 || index >= array.Count || !array.Items)
        return nullptr;
    return array.Items[index];
}

const HouseClass* FindHouse(int houseIndex)
{
    if (houseIndex < 0)
        return nullptr;
    for (const HouseClass* house : GetValidatedHouseArray())
    {
        if (house && house->ArrayIndex == houseIndex)
            return house;
    }
    return nullptr;
}

// 非参战房屋（中立、特殊等 MultiplayPassive）与无主一样记为“中立”。
viewer::RadarPlayerRef MakePlayerRef(int houseIndex)
{
    const HouseClass* house = FindHouse(houseIndex);
    if (!house || !IsParticipatingHouse(house))
        return stats::MakeNeutralPlayerRef();
    viewer::RadarPlayerRef player;
    player.mHouseIndex = house->ArrayIndex;
    player.mName = ReadHouseDisplayName(house);
    player.mColorRgb = ReadHouseColorRgb(house);
    return player;
}

// 格不在地图内时仍给出格中心的 X、Y，Z 取 0；这样事件仍可显示，只是跳转与落点贴不住地面。
viewer::WorldPoint CellToWorld(const stats::CellPoint& cell)
{
    viewer::WorldPoint world;
    world.mX = cell.mX * LEPTONS_PER_CELL + CELL_CENTER_OFFSET;
    world.mY = cell.mY * LEPTONS_PER_CELL + CELL_CENTER_OFFSET;
    world.mZ = 0;
    const CellStruct mapCoords{static_cast<short>(cell.mX), static_cast<short>(cell.mY)};
    // 必须用 TryGetCellAt：MapClass::GetCellAt 在越界时会写入 MapClass_InvalidCell 的坐标。
    const CellClass* mapCell = MapClass::Instance.TryGetCellAt(mapCoords);
    if (mapCell)
    {
        world.mZ = mapCell->GetFloorHeight(Point2D{CELL_CENTER_OFFSET, CELL_CENTER_OFFSET});
        if (mapCell->ContainsBridge())
            world.mZ += CellClass::BridgeHeight;
    }
    return world;
}
}

void RadarEventResolver::Reset() noexcept
{
    mIcons.Clear();
    mShowCameo.clear();
}

void RadarEventResolver::BeginSample() noexcept
{
    mIcons.BeginSample();
}

bool RadarEventResolver::IsShownInSidebar(int typeIndex)
{
    if (const auto found = mShowCameo.find(typeIndex); found != mShowCameo.end())
        return found->second;
    const SuperWeaponTypeClass* type = GetSuperWeaponType(typeIndex);
    // 地图 INI 对超武节的覆盖在 INI_Rules 中不可见；键缺失按显示处理。
    const bool showCameo = type && CCINIClass::INI_Rules
        ? CCINIClass::INI_Rules->ReadBool(type->ID, "SW.ShowCameo", true) : true;
    const bool isShown = stats::IsSuperWeaponReportable(showCameo);
    mShowCameo.emplace(typeIndex, isShown);
    return isShown;
}

std::optional<stats::RadarEventDraft> RadarEventResolver::ResolveLaunch(const stats::SuperWeaponLaunch& launch)
{
    const SuperWeaponTypeClass* type = GetSuperWeaponType(launch.mTypeIndex);
    if (!type || !FindHouse(launch.mHouseIndex) || !IsShownInSidebar(launch.mTypeIndex))
        return std::nullopt;
    const SuperWeaponTypeClass* iconType = GetSuperWeaponType(launch.mIconTypeIndex);
    if (!iconType)
        iconType = type;
    // 名称：自己的 UIName；PostClick 类型在 MO 中可能没有 UIName，此时取前置超武的，最后才回退内部 ID。
    std::string name = ReadTypeUiName(type);
    if (name.empty() && iconType != type)
        name = ReadTypeUiName(iconType);
    if (name.empty())
        name = ReadTypeDisplayName(type);
    auto icon = mIcons.ResolveSuperWeapon(iconType);
    return stats::MakeSuperWeaponLaunchDraft(MakePlayerRef(launch.mHouseIndex), std::move(name), std::move(icon),
        CellToWorld(launch.mTargetCell));
}

std::optional<stats::RadarEventDraft> RadarEventResolver::ResolveInfiltration(
    const stats::BuildingInfiltrationObservation& observation)
{
    const BuildingTypeClass* buildingType = GetBuildingType(observation.mBuildingTypeIndex);
    if (!buildingType)
        return std::nullopt;
    return stats::MakeInfiltrationDraft(MakePlayerRef(observation.mSpyHouseIndex),
        MakePlayerRef(observation.mBuildingHouseIndex), ReadTypeDisplayName(buildingType), observation.mLocation);
}
}
