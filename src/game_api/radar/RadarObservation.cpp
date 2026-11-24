#include "RadarObservation.h"

using byte = unsigned char;

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <CellClass.h>
#include <HouseClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <TechnoClass.h>
#include <Unsorted.h>

#include <cstddef>
#include <cstdint>

namespace rareplay::game_api
{
namespace
{
// 以下偏移由 IDA 对 HouseClass::Fire_SW 0x4FAE50、SuperClass::ClickFire 0x6CB920、Ares 的 ClickFire 替换实现与
// SuperWeaponTypeClass::LoadFromINI 0x6CEA20 核对；YRpp 声明漂移时必须在编译期失败。
static_assert(offsetof(SuperClass, Type) == 0x28);
static_assert(offsetof(SuperClass, ChronoMapCoords) == 0x62);
static_assert(offsetof(SuperClass, IsReady) == 0x6F);
static_assert(offsetof(SuperClass, ChargeDrainState) == 0x7C);
static_assert(offsetof(SuperWeaponTypeClass, ArrayIndex) == 0x98);
static_assert(offsetof(SuperWeaponTypeClass, SidebarImage) == 0xB8);
static_assert(offsetof(SuperWeaponTypeClass, UseChargeDrain) == 0xE5);
static_assert(offsetof(SuperWeaponTypeClass, PreClick) == 0xED);
static_assert(offsetof(SuperWeaponTypeClass, PostClick) == 0xEE);
static_assert(offsetof(SuperWeaponTypeClass, PreDependent) == 0xF0);
static_assert(offsetof(HouseClass, ArrayIndex) == 0x30);
static_assert(sizeof(CellStruct) == 4);
// 渗透观察点 0x51A002：IDA 核对 [ESI+0x21C] 为间谍的所属房屋，[EDI+0x21C] 与 [EDI+0x520] 为建筑的所属房屋与类型。
static_assert(offsetof(TechnoClass, Owner) == 0x21C);
static_assert(offsetof(BuildingClass, Type) == 0x520);

// 房屋数组的上限远小于它；超过说明读到了损坏的数据。
constexpr int MAX_HOUSE_INDEX = 64;

int ReadHouseIndex(const HouseClass* house) noexcept
{
    if (!house)
        return -1;
    const int index = house->ArrayIndex;
    return index >= 0 && index < MAX_HOUSE_INDEX ? index : -1;
}

// 类型对象在整局中不会移动；按指针在类型数组里找下标，不依赖 ArrayIndex 字段的语义。
template <typename TType>
int FindTypeIndex(const DynamicVectorClass<TType*>& array, const TType* type) noexcept
{
    if (!type || array.Count < 0 || array.Count > 100000 || (array.Count > 0 && !array.Items))
        return -1;
    for (int index = 0; index < array.Count; ++index)
    {
        if (array.Items[index] == type)
            return index;
    }
    return -1;
}

stats::SuperWeaponClickMode GetClickMode(const SuperWeaponTypeClass& type) noexcept
{
    // 与 Ares 的 ClickFire 替换实现的判断顺序一致：持续放能类先处理；PostClick 与 PreClick 并存时按 PostClick。
    if (type.UseChargeDrain)
        return stats::SuperWeaponClickMode::ChargeDrain;
    if (type.PostClick)
        return stats::SuperWeaponClickMode::PostClick;
    if (type.PreClick)
        return stats::SuperWeaponClickMode::PreClick;
    return stats::SuperWeaponClickMode::Normal;
}
}

bool ReadSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
    const void* targetCell, stats::SuperWeaponFireBegin& out) noexcept
{
    const auto* owner = static_cast<const HouseClass*>(house);
    const auto* firedSuper = static_cast<const SuperClass*>(super);
    const auto* cell = static_cast<const CellStruct*>(targetCell);
    if (!owner || !firedSuper || !cell || !firedSuper->Type)
        return false;
    const int houseIndex = ReadHouseIndex(owner);
    const int typeIndex = FindTypeIndex(SuperWeaponTypeClass::Array, firedSuper->Type);
    if (houseIndex < 0 || typeIndex < 0)
        return false;

    stats::SuperWeaponFireBegin begin;
    begin.mSuperToken = reinterpret_cast<std::uintptr_t>(firedSuper);
    begin.mFrame = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    begin.mHouseIndex = houseIndex;
    begin.mTypeIndex = typeIndex;
    begin.mMode = GetClickMode(*firedSuper->Type);
    begin.mIsReady = firedSuper->IsReady;
    begin.mChargeDrainState = static_cast<int>(firedSuper->ChargeDrainState);
    begin.mTargetCell = {cell->X, cell->Y};
    const auto* prerequisite = static_cast<const SuperClass*>(prerequisiteSuper);
    if (prerequisite && prerequisite->Type)
    {
        const int prerequisiteIndex = FindTypeIndex(SuperWeaponTypeClass::Array, prerequisite->Type);
        if (prerequisiteIndex >= 0)
        {
            begin.mHasPrerequisite = true;
            begin.mPrerequisiteTypeIndex = prerequisiteIndex;
            begin.mPrerequisiteIsReady = prerequisite->IsReady;
        }
    }
    out = begin;
    return true;
}

bool ReadSuperWeaponFireEnd(const void* super, stats::SuperWeaponFireEnd& out) noexcept
{
    const auto* firedSuper = static_cast<const SuperClass*>(super);
    if (!firedSuper)
        return false;
    out.mSuperToken = reinterpret_cast<std::uintptr_t>(firedSuper);
    out.mIsReady = firedSuper->IsReady;
    out.mChargeDrainState = static_cast<int>(firedSuper->ChargeDrainState);
    return true;
}

bool ReadBuildingInfiltration(const void* spy, const void* building,
    stats::BuildingInfiltrationObservation& out) noexcept
{
    const auto* spyUnit = static_cast<const TechnoClass*>(spy);
    const auto* target = static_cast<const BuildingClass*>(building);
    if (!spyUnit || !target || !target->Type)
        return false;
    const int typeIndex = FindTypeIndex(BuildingTypeClass::Array, target->Type);
    if (typeIndex < 0)
        return false;
    out.mFrame = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    out.mSpyHouseIndex = ReadHouseIndex(spyUnit->Owner);
    out.mBuildingHouseIndex = ReadHouseIndex(target->Owner);
    out.mBuildingTypeIndex = typeIndex;
    out.mLocation = {target->Location.X, target->Location.Y, target->Location.Z};
    return true;
}
}
