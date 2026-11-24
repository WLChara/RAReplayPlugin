#include "StatsReader.h"

#include "../HouseRoster.h"
#include "../../stats/OperationEvents.h"

// 此版 YRpp 的若干声明仍使用 Win32 的小写 byte 基类型。
using byte = unsigned char;

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <EventClass.h>
#include <FactoryClass.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <SessionClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rareplay::game_api
{
namespace
{
// 以下偏移由 IDA 的 HouseClass、FactoryClass、BuildingTypeClass、TechnoClass 类型与反汇编核对；
// YRpp 声明漂移时必须在编译期失败。
static_assert(offsetof(HouseClass, ArrayIndex) == 0x30);
static_assert(offsetof(HouseClass, Buildings) == 0x68);
static_assert(offsetof(HouseClass, BarracksInfiltrated) == 0x2BF);
static_assert(offsetof(HouseClass, WarFactoryInfiltrated) == 0x2C0);
static_assert(offsetof(HouseClass, PowerOutput) == 0x53A4);
static_assert(offsetof(HouseClass, PowerDrain) == 0x53A8);
static_assert(offsetof(HouseClass, KilledUnitsOfHouses) == 0x53E4);
static_assert(offsetof(HouseClass, KilledBuildingsOfHouses) == 0x5438);
static_assert(offsetof(HouseClass, PointTotal) == 0x54E8);
static_assert(offsetof(HouseClass, FactoryProducedBuildingTypes) == 0x55A0);
static_assert(offsetof(HouseClass, FactoryProducedUnitTypes) == 0x55B4);
static_assert(offsetof(HouseClass, FactoryProducedInfantryTypes) == 0x55C8);
static_assert(offsetof(HouseClass, FactoryProducedAircraftTypes) == 0x55DC);
// 盟友位掩码在 +0x5788（IsAlliedWith 0x4F9A50 的 test [eax+5788h]）；IDB 的 HouseClass 类型把它标成 DamageDelayTimer.StartTime，
// 而 Allies 标在 0x5784 是错位，以 YRpp 与反汇编为准。
static_assert(offsetof(HouseClass, Allies) == 0x5788);
static_assert(sizeof(HouseClass::KilledUnitsOfHouses) == sizeof(int) * stats::KILL_MATRIX_SIZE);
static_assert(sizeof(HouseClass::KilledBuildingsOfHouses) == sizeof(int) * stats::KILL_MATRIX_SIZE);
static_assert(offsetof(CounterClass, Total) == 0x10);
static_assert(offsetof(FactoryClass, Production) == 0x24);
static_assert(offsetof(StageClass, Value) == 0);
static_assert(offsetof(FactoryClass, QueuedObjects) == 0x40);
static_assert(offsetof(FactoryClass, Object) == 0x58);
static_assert(offsetof(FactoryClass, OnHold) == 0x5C);
static_assert(offsetof(FactoryClass, Owner) == 0x6C);
static_assert(offsetof(FactoryClass, IsSuspended) == 0x70);
static_assert(offsetof(FactoryClass, IsManual) == 0x71);
// ObjectClass/MissionClass 字段（IDB 类型）：Health、IsOnMap、InLimbo、IsAlive 与 CurrentMission；Mission::Construction 为 18。
static_assert(offsetof(BuildingClass, Health) == 0x6C);
static_assert(offsetof(BuildingClass, IsOnMap) == 0x74);
static_assert(offsetof(BuildingClass, InLimbo) == 0x81);
static_assert(offsetof(BuildingClass, IsAlive) == 0x90);
static_assert(offsetof(BuildingClass, CurrentMission) == 0xAC);
static_assert(static_cast<int>(Mission::Construction) == 18);
static_assert(offsetof(BuildingClass, Type) == 0x520);
static_assert(offsetof(BuildingClass, Factory) == 0x524);
static_assert(offsetof(BuildingTypeClass, Factory) == 0xEB8);
static_assert(offsetof(BuildingTypeClass, Cloning) == 0x16AC);
static_assert(offsetof(TechnoTypeClass, Naval) == 0xCCE);
static_assert(offsetof(TechnoClass, Transporter) == 0x11C);
static_assert(offsetof(TechnoClass, Owner) == 0x21C);
// SessionClass 在 0xA8B238，DesiredFrameRate 即 0xA8B558；GameOptionsClass 在 0xA8EB60，GameSpeed 是首个成员。
static_assert(offsetof(SessionClass, DesiredFrameRate) == 0x320);
static_assert(offsetof(GameOptionsClass, GameSpeed) == 0);
// 操作合并键用到的事件载荷字段偏移（相对 DataBuffer）：逐项对 YRpp 的联合体成员声明做核对；
// 各偏移同时经 IDA 对事件构造函数与 handle 分支的读写核对；各事件取哪些字段见 stats/OperationEvents.cpp 的归类表。
static_assert(offsetof(EventClass, DataBuffer) == stats::EVENT_PAYLOAD_OFFSET);
static_assert(stats::NATIVE_EVENT_BYTES == sizeof(EventClass));
static_assert(sizeof(TargetClass) == stats::event_layout::TARGET_BYTES);
static_assert(offsetof(TargetClass, m_ID) == 0 && offsetof(TargetClass, m_RTTI) == 4);
#define RAREPLAY_CHECK_LAYOUT(Struct, Field, Offset) static_assert(offsetof(decltype(EventClass::Struct), Field) == stats::event_layout::Offset)
RAREPLAY_CHECK_LAYOUT(MegaMission, Whom, MEGA_MISSION_WHOM);
RAREPLAY_CHECK_LAYOUT(MegaMission, Mission, MEGA_MISSION_MISSION);
RAREPLAY_CHECK_LAYOUT(MegaMission, Target, MEGA_MISSION_TARGET);
RAREPLAY_CHECK_LAYOUT(MegaMission, Destination, MEGA_MISSION_DESTINATION);
RAREPLAY_CHECK_LAYOUT(MegaMission, Follow, MEGA_MISSION_FOLLOW);
RAREPLAY_CHECK_LAYOUT(MegaMission, IsPlanningEvent, MEGA_MISSION_PLANNING);
static_assert(sizeof(decltype(EventClass::MegaMission)) == stats::event_layout::MEGA_MISSION_BYTES);
RAREPLAY_CHECK_LAYOUT(MegaMissionF, Whom, MEGA_MISSION_F_WHOM);
RAREPLAY_CHECK_LAYOUT(MegaMissionF, Mission, MEGA_MISSION_F_MISSION);
RAREPLAY_CHECK_LAYOUT(MegaMissionF, Target, MEGA_MISSION_F_TARGET);
RAREPLAY_CHECK_LAYOUT(MegaMissionF, Destination, MEGA_MISSION_F_DESTINATION);
RAREPLAY_CHECK_LAYOUT(MegaMissionF, Speed, MEGA_MISSION_F_SPEED);
RAREPLAY_CHECK_LAYOUT(MegaMissionF, MaxSpeed, MEGA_MISSION_F_MAX_SPEED);
static_assert(sizeof(decltype(EventClass::MegaMissionF)) == stats::event_layout::MEGA_MISSION_F_BYTES);
RAREPLAY_CHECK_LAYOUT(Produce, RTTIType, PRODUCTION_RTTI);
RAREPLAY_CHECK_LAYOUT(Produce, HeapID, PRODUCTION_HEAP_ID);
RAREPLAY_CHECK_LAYOUT(Produce, IsNaval, PRODUCTION_IS_NAVAL);
static_assert(sizeof(decltype(EventClass::Produce)) == stats::event_layout::PRODUCTION_BYTES);
static_assert(sizeof(decltype(EventClass::Suspend)) == stats::event_layout::PRODUCTION_BYTES);
static_assert(sizeof(decltype(EventClass::Abandon)) == stats::event_layout::PRODUCTION_BYTES);
static_assert(sizeof(decltype(EventClass::AbandonAll)) == stats::event_layout::PRODUCTION_BYTES);
RAREPLAY_CHECK_LAYOUT(Place, RTTIType, PLACE_RTTI);
RAREPLAY_CHECK_LAYOUT(Place, HeapID, PLACE_HEAP_ID);
RAREPLAY_CHECK_LAYOUT(Place, IsNaval, PLACE_IS_NAVAL);
RAREPLAY_CHECK_LAYOUT(Place, Location, PLACE_LOCATION);
static_assert(sizeof(decltype(EventClass::Place)) == stats::event_layout::PLACE_BYTES);
RAREPLAY_CHECK_LAYOUT(SpecialPlace, ID, SPECIAL_PLACE_ID);
RAREPLAY_CHECK_LAYOUT(SpecialPlace, Location, SPECIAL_PLACE_LOCATION);
static_assert(sizeof(decltype(EventClass::SpecialPlace)) == stats::event_layout::SPECIAL_PLACE_BYTES);
RAREPLAY_CHECK_LAYOUT(SellCell, Location, SELL_CELL_LOCATION);
static_assert(sizeof(decltype(EventClass::SellCell)) == stats::event_layout::SELL_CELL_BYTES);
RAREPLAY_CHECK_LAYOUT(Ally, HouseID, ALLY_HOUSE_ID);
static_assert(sizeof(decltype(EventClass::Ally)) == stats::event_layout::ALLY_BYTES);
RAREPLAY_CHECK_LAYOUT(Archive, Whom1, ARCHIVE_FIRST);
RAREPLAY_CHECK_LAYOUT(Archive, Whom2, ARCHIVE_SECOND);
static_assert(sizeof(decltype(EventClass::Archive)) == stats::event_layout::ARCHIVE_BYTES);
RAREPLAY_CHECK_LAYOUT(PlanConnect, Target1, PLAN_CONNECT_FIRST);
RAREPLAY_CHECK_LAYOUT(PlanConnect, Target2, PLAN_CONNECT_SECOND);
static_assert(sizeof(decltype(EventClass::PlanConnect)) == stats::event_layout::PLAN_CONNECT_BYTES);
// 时间表读取 Timing 的 RequestedFPS（载荷 +0，事件 +7）；处理分支 0x4C807D 原样写入 DesiredFrameRate。
RAREPLAY_CHECK_LAYOUT(Timing, RequestedFPS, TIMING_REQUESTED_FPS);
RAREPLAY_CHECK_LAYOUT(Timing, MaxAhead, TIMING_MAX_AHEAD);
RAREPLAY_CHECK_LAYOUT(Timing, FrameSendRate, TIMING_FRAME_SEND_RATE);
static_assert(sizeof(decltype(EventClass::Timing)) == stats::event_layout::TIMING_BYTES);
static_assert(sizeof(EventClass::Timing.RequestedFPS) == 2);
// 只带一个目标的事件：载荷就是一个 TargetClass。
static_assert(sizeof(decltype(EventClass::Poweron)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Poweroff)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Idle)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Scatter)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Deploy)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Detonate)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Primary)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Repair)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::Sell)) == stats::event_layout::TARGET_BYTES);
static_assert(sizeof(decltype(EventClass::PlanNodeDelete)) == stats::event_layout::TARGET_BYTES);
#undef RAREPLAY_CHECK_LAYOUT
// 操作计数读取的事件字节位置，以及逐项与 YRpp 的事件类型数值一致。
static_assert(offsetof(EventClass, Type) == stats::EVENT_TYPE_OFFSET);
static_assert(offsetof(EventClass, HouseIndex) == stats::EVENT_HOUSE_INDEX_OFFSET);
static_assert(static_cast<std::size_t>(EventType::LAST_EVENT) == stats::NATIVE_EVENT_TYPE_COUNT);
#define RAREPLAY_CHECK_EVENT(name) \
    static_assert(static_cast<std::uint8_t>(EventType::name) == static_cast<std::uint8_t>(stats::NativeEventType::name))
RAREPLAY_CHECK_EVENT(Empty); RAREPLAY_CHECK_EVENT(PowerOn); RAREPLAY_CHECK_EVENT(PowerOff);
RAREPLAY_CHECK_EVENT(Ally); RAREPLAY_CHECK_EVENT(MegaMission); RAREPLAY_CHECK_EVENT(MegaMissionF);
RAREPLAY_CHECK_EVENT(Idle); RAREPLAY_CHECK_EVENT(Scatter); RAREPLAY_CHECK_EVENT(Destruct);
RAREPLAY_CHECK_EVENT(Deploy); RAREPLAY_CHECK_EVENT(Detonate); RAREPLAY_CHECK_EVENT(Place);
RAREPLAY_CHECK_EVENT(Options); RAREPLAY_CHECK_EVENT(GameSpeed); RAREPLAY_CHECK_EVENT(Produce);
RAREPLAY_CHECK_EVENT(Suspend); RAREPLAY_CHECK_EVENT(Abandon); RAREPLAY_CHECK_EVENT(Primary);
RAREPLAY_CHECK_EVENT(SpecialPlace); RAREPLAY_CHECK_EVENT(Exit); RAREPLAY_CHECK_EVENT(Animation);
RAREPLAY_CHECK_EVENT(Repair); RAREPLAY_CHECK_EVENT(Sell); RAREPLAY_CHECK_EVENT(SellCell);
RAREPLAY_CHECK_EVENT(Special); RAREPLAY_CHECK_EVENT(FrameSync); RAREPLAY_CHECK_EVENT(Message);
RAREPLAY_CHECK_EVENT(ResponseTime); RAREPLAY_CHECK_EVENT(FrameInfo); RAREPLAY_CHECK_EVENT(SaveGame);
RAREPLAY_CHECK_EVENT(Archive); RAREPLAY_CHECK_EVENT(AddPlayer); RAREPLAY_CHECK_EVENT(Timing);
RAREPLAY_CHECK_EVENT(ProcessTime); RAREPLAY_CHECK_EVENT(PageUser); RAREPLAY_CHECK_EVENT(RemovePlayer);
RAREPLAY_CHECK_EVENT(LatencyFudge); RAREPLAY_CHECK_EVENT(MegaFrameInfo); RAREPLAY_CHECK_EVENT(PacketTiming);
RAREPLAY_CHECK_EVENT(AboutToExit); RAREPLAY_CHECK_EVENT(FallbackHost); RAREPLAY_CHECK_EVENT(AddressChange);
RAREPLAY_CHECK_EVENT(PlanConnect); RAREPLAY_CHECK_EVENT(PlanCommit); RAREPLAY_CHECK_EVENT(PlanNodeDelete);
RAREPLAY_CHECK_EVENT(AllCheer); RAREPLAY_CHECK_EVENT(AbandonAll);
#undef RAREPLAY_CHECK_EVENT

// 数组计数超过它时视为损坏，与现有 ReplayWorldReader 的对象数组上限一致。
constexpr int MAX_OBJECT_ARRAY_COUNT = 100000;

// SDK 把原生的 0/1 字节声明为 bool；按字节读取，避免非 0/1 值的 bool 语义。
bool ReadFlag(const bool& flag) noexcept
{
    return (*reinterpret_cast<const std::uint8_t*>(&flag)) != 0;
}

template <typename TVector>
bool IsReadableVector(const TVector& vector) noexcept
{
    return vector.Count >= 0 && vector.Count <= MAX_OBJECT_ARRAY_COUNT && (vector.Count == 0 || vector.Items != nullptr);
}

stats::HouseObservation ReadHouse(const HouseClass& house)
{
    stats::HouseObservation observation;
    observation.mHouseIndex = house.ArrayIndex;
    observation.mIsHuman = ReadFlag(house.IsHumanPlayer);
    // 现金加可用矿石价值；经 IDA 核对 Available_Money 只读无副作用，直接读 Balance 会少算矿石。
    observation.mCredits = house.Available_Money();
    observation.mPowerOutput = house.PowerOutput;
    observation.mPowerDrain = house.PowerDrain;
    observation.mAllyMask = house.Allies.data;
    observation.mBarracksInfiltrated = ReadFlag(house.BarracksInfiltrated);
    observation.mWarFactoryInfiltrated = ReadFlag(house.WarFactoryInfiltrated);
    stats::HouseCounters& counters = observation.mCounters;
    for (std::size_t index = 0; index < stats::KILL_MATRIX_SIZE; ++index)
    {
        counters.mKilledUnitsOfHouses[index] = house.KilledUnitsOfHouses[index];
        counters.mKilledBuildingsOfHouses[index] = house.KilledBuildingsOfHouses[index];
    }
    // 工厂产出计数的 Total（CounterClass +0x10）；YRpp 中 LastBuilt* 的偏移写反了，这里不使用它们。
    counters.mBuiltBuildings = house.FactoryProducedBuildingTypes.GetTotal();
    counters.mBuiltVehicles = house.FactoryProducedUnitTypes.GetTotal();
    counters.mBuiltInfantry = house.FactoryProducedInfantryTypes.GetTotal();
    counters.mBuiltAircraft = house.FactoryProducedAircraftTypes.GetTotal();
    // 字段名在 YRpp 中为 PointTotal；IDB 旧名 SiloMoney 有误。回放不模拟结算画面的随机加成。
    counters.mPointTotal = house.PointTotal;
    return observation;
}

// 与原生 0x4FFA50 的工厂建筑分类一致：Factory 取 Infantry(15) 或 InfantryType(16) 为兵营；
// 取 Unit(1) 或 UnitType(40) 且非 Naval 为重工，Naval 属于船厂（NumShipyards），不混入。
bool IsBarracks(const BuildingTypeClass& type) noexcept
{
    return type.Factory == AbstractType::Infantry || type.Factory == AbstractType::InfantryType;
}

bool IsWarFactory(const BuildingTypeClass& type) noexcept
{
    return (type.Factory == AbstractType::Unit || type.Factory == AbstractType::UnitType) && !type.Naval;
}

void CountFactoryBuildings(const HouseClass& house, stats::HouseProductionObservation& out)
{
    if (!IsReadableVector(house.Buildings))
        return;
    for (int index = 0; index < house.Buildings.Count; ++index)
    {
        const BuildingClass* building = house.Buildings.Items[index];
        if (building == nullptr || !building->IsAlive || building->InLimbo || building->Health <= 0 ||
            building->Type == nullptr)
            continue;
        // 建造动画中的工厂还不能生产（NumBarracks 却已计入），所以排除。
        if (building->CurrentMission == Mission::Construction)
            continue;
        const BuildingTypeClass& type = *building->Type;
        if (ReadFlag(type.Cloning))
            continue;
        if (IsBarracks(type))
            ++out.mBarracks.mFactoryBuildings;
        else if (IsWarFactory(type))
            ++out.mWarFactory.mFactoryBuildings;
    }
}

stats::FactoryObservation ReadFactory(const FactoryClass& factory)
{
    stats::FactoryObservation observation;
    observation.mProgress = factory.Production.Value;
    observation.mIsSuspended = ReadFlag(factory.IsSuspended);
    observation.mIsManual = ReadFlag(factory.IsManual);
    observation.mIsOnHold = ReadFlag(factory.OnHold);
    return observation;
}
}

void ReadFrameObservation(stats::FrameObservation& outFrame)
{
    outFrame.mHouses.clear();
    const GameMode mode = SessionClass::Instance.GameMode;
    outFrame.mTiming.mIsNetwork = mode == GameMode::LAN || mode == GameMode::Internet;
    // 只读 GameSpeed 与 DesiredFrameRate；观看者调速只改帧限速寄存器，绝不能读 g_FrameBudget。
    outFrame.mTiming.mGameSpeed = GameOptionsClass::Instance.GameSpeed;
    outFrame.mTiming.mDesiredFrameRate = SessionClass::Instance.DesiredFrameRate;
    for (const HouseClass* house : GetValidatedHouseArray())
    {
        if (IsParticipatingHouse(house))
            outFrame.mHouses.push_back(ReadHouse(*house));
    }
}

void ReadProductionObservation(stats::ProductionObservation& outProduction)
{
    outProduction.mHouses.clear();
    std::vector<std::pair<const HouseClass*, std::size_t>> slots;
    for (const HouseClass* house : GetValidatedHouseArray())
    {
        if (!IsParticipatingHouse(house))
            continue;
        stats::HouseProductionObservation observation;
        observation.mHouseIndex = house->ArrayIndex;
        CountFactoryBuildings(*house, observation);
        slots.emplace_back(house, outProduction.mHouses.size());
        outProduction.mHouses.push_back(std::move(observation));
    }

    const auto& factories = FactoryClass::Array;
    if (!IsReadableVector(factories))
        return;
    for (int index = 0; index < factories.Count; ++index)
    {
        const FactoryClass* factory = factories.Items[index];
        if (factory == nullptr || factory->Owner == nullptr)
            continue;
        std::size_t slot = outProduction.mHouses.size();
        for (const auto& [house, houseSlot] : slots)
        {
            if (house == factory->Owner)
                slot = houseSlot;
        }
        if (slot == outProduction.mHouses.size())
            continue;
        // 有产品按产品归类；产品为空但队列非空时按队首归类。两者都空说明没有项目，不算这座工厂。
        const TechnoTypeClass* type = nullptr;
        if (factory->Object != nullptr)
            type = factory->Object->GetTechnoType();
        else if (factory->QueuedObjects.Count > 0 && factory->QueuedObjects.Items != nullptr)
            type = factory->QueuedObjects.Items[0];
        if (type == nullptr)
            continue;
        stats::HouseProductionObservation& house = outProduction.mHouses[slot];
        const AbstractType kind = type->WhatAmI();
        if (kind == AbstractType::InfantryType)
            house.mBarracks.mFactories.push_back(ReadFactory(*factory));
        else if (kind == AbstractType::UnitType && !ReadFlag(type->Naval))
            house.mWarFactory.mFactories.push_back(ReadFactory(*factory));
    }
}
}
