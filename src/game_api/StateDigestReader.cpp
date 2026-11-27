#include "StateDigestReader.h"

#include "../target/TargetAbi.h"

// 此版 YRpp 的若干声明仍使用 Win32 的小写 byte 基类型。
using byte = unsigned char;

#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <EventClass.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <Randomizer.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace rareplay::game_api
{
namespace
{
// 以下偏移逐项经 IDA 对照反汇编核对；YRpp 声明漂移时必须在编译期失败。
// 场景：Timing 处理分支 0x4C8011 以 [Scenario+0] & 0x1000 判断战争迷雾（SpecialFlags）；Create_ID（0x68BCB0）对 +0x214
// 先加一再返回，构造函数把结果写入 AbstractClass+0x10；InitRandom 把场景随机数构造在 +0x218。
static_assert(offsetof(ScenarioClass, SpecialFlags) == 0);
static_assert(sizeof(ScenarioFlags) == sizeof(std::uint32_t));
static_assert(offsetof(ScenarioClass, UniqueID) == 0x214);
static_assert(offsetof(ScenarioClass, Random) == 0x218);
// 全局、局部变量的变化标志：SetGlobalVariable（0x689670）等四个设置函数置 1，LogicClass::Update 在 0x55B01D 读取、
// 0x55B1D8 清零。
static_assert(offsetof(ScenarioClass, VariablesChanged) == 0x34AA);
// 变量的值：ClearScenario 0x685354 起按 41 字节步长读写 +0x1CB0 起的 50 个全局变量值；局部变量的 100 个值紧随其后从 +0x24B2 起。
static_assert(sizeof(Variable) == 41);
static_assert(offsetof(ScenarioClass, GlobalVariables) + offsetof(Variable, Value) == 0x1CB0);
static_assert(offsetof(ScenarioClass, LocalVariables) + offsetof(Variable, Value) == 0x24B2);
constexpr std::size_t GLOBAL_VARIABLE_COUNT = 50;
constexpr std::size_t LOCAL_VARIABLE_COUNT = 100;
static_assert(sizeof(ScenarioClass::GlobalVariables) == GLOBAL_VARIABLE_COUNT * sizeof(Variable));
static_assert(sizeof(ScenarioClass::LocalVariables) == LOCAL_VARIABLE_COUNT * sizeof(Variable));
// Randomizer::Random（0x65C780）：[+0] 非 0 时返回 0 且不推进；否则以 +4、+8 为下标访问 +0xC 起的表，下标到 250（0xFA）回绕。
static_assert(offsetof(Randomizer, unknown_00) == 0);
static_assert(offsetof(Randomizer, Next1) == 4);
static_assert(offsetof(Randomizer, Next2) == 8);
static_assert(offsetof(Randomizer, Table) == 0xC);
constexpr std::size_t RANDOM_TABLE_SIZE = 250;
static_assert(sizeof(Randomizer::Table) == RANDOM_TABLE_SIZE * sizeof(std::uint32_t));
// 房屋：GetArrayIndex（0x50E370）读 +0x30；GiveMoney（0x4F9950）加到 +0x30C；BeaconClass::Draw（0x430270）以 +0x16054 为
// 下标取配色表。电力与盟友位掩码的偏移与统计读取的核对结果相同。
static_assert(offsetof(HouseClass, ArrayIndex) == 0x30);
static_assert(offsetof(HouseClass, Type) == 0x34);
static_assert(offsetof(HouseClass, Balance) == 0x30C);
static_assert(offsetof(HouseClass, PowerOutput) == 0x53A4);
static_assert(offsetof(HouseClass, PowerDrain) == 0x53A8);
static_assert(offsetof(HouseClass, Allies) == 0x5788);
static_assert(offsetof(HouseClass, ColorSchemeIndex) == 0x16054);
static_assert(sizeof(HouseClass::Allies) == sizeof(std::uint32_t));
// 各类型的 GetArrayIndex（HouseType 0x512730；UnitType 0x748180、InfantryType 0x524D60、BuildingType 0x465DB0、
// AircraftType 0x41CFD0）分别读 +0xB4 与 +0xDF8。
static_assert(offsetof(HouseTypeClass, ArrayIndex) == 0xB4);
static_assert(offsetof(UnitTypeClass, ArrayIndex) == 0xDF8);
static_assert(offsetof(InfantryTypeClass, ArrayIndex) == 0xDF8);
static_assert(offsetof(BuildingTypeClass, ArrayIndex) == 0xDF8);
static_assert(offsetof(AircraftTypeClass, ArrayIndex) == 0xDF8);
// 对象：AbstractClass 构造函数（0x410170）把 +0x10 置 −1，随后由 Create_ID 写入；ObjectClass::GetCoords（0x5F65A0）
// 复制 +0x9C 起的 12 字节。Health、CurrentMission、Owner 与统计读取的口径相同。各类的 GetType（Unit 0x741490、
// Infantry 0x51FAF0、Building 0x459EE0、Aircraft 0x41C200）分别读 +0x6C4、+0x6C0、+0x520、+0x6C4。
static_assert(offsetof(UnitClass, UniqueID) == 0x10);
static_assert(offsetof(UnitClass, Health) == 0x6C);
static_assert(offsetof(UnitClass, Location) == 0x9C);
static_assert(sizeof(CoordStruct) == 3 * sizeof(std::int32_t));
static_assert(offsetof(UnitClass, CurrentMission) == 0xAC);
static_assert(offsetof(UnitClass, Owner) == 0x21C);
static_assert(offsetof(UnitClass, Type) == 0x6C4);
static_assert(offsetof(InfantryClass, UniqueID) == 0x10 && offsetof(InfantryClass, Location) == 0x9C);
static_assert(offsetof(InfantryClass, Type) == 0x6C0);
static_assert(offsetof(BuildingClass, UniqueID) == 0x10 && offsetof(BuildingClass, Location) == 0x9C);
static_assert(offsetof(BuildingClass, Type) == 0x520);
static_assert(offsetof(AircraftClass, UniqueID) == 0x10 && offsetof(AircraftClass, Location) == 0x9C);
static_assert(offsetof(AircraftClass, Type) == 0x6C4);
// 会话（0xA8B238）：Timing 处理分支读写 MaxAhead 0xA8B550、FrameSendRate 0xA8B554、DesiredFrameRate 0xA8B558。
static_assert(offsetof(SessionClass, GameMode) == 0);
static_assert(offsetof(SessionClass, MaxAhead) == 0x318);
static_assert(offsetof(SessionClass, FrameSendRate) == 0x31C);
static_assert(offsetof(SessionClass, DesiredFrameRate) == 0x320);

// 原生房屋数组远小于前者；对象数组上限与界面、统计读取一致。超过说明读到了损坏的数据。
constexpr int MAX_HOUSE_ARRAY_COUNT = 64;
constexpr int MAX_OBJECT_ARRAY_COUNT = 100000;

template <typename TItem>
void RequireReadableVector(const DynamicVectorClass<TItem*>& vector, int maxCount, const char* error)
{
    if (vector.Count < 0 || vector.Count > maxCount || (vector.Count > 0 && vector.Items == nullptr))
        throw std::runtime_error(error);
}

// SDK 把原生的 0/1 字节声明为 bool；按字节读取，避免非 0/1 值的 bool 语义。
std::uint8_t ReadByte(const bool& flag) noexcept
{
    return *reinterpret_cast<const std::uint8_t*>(&flag);
}

std::int32_t ReadGlobalInt(std::uintptr_t address) noexcept
{
    return *reinterpret_cast<const std::int32_t*>(address);
}

void ReadRandom(const Randomizer& random, StateDigest& digest) noexcept
{
    digest.mRandomIsConstant = ReadByte(random.unknown_00);
    digest.mRandomNext1 = random.Next1;
    digest.mRandomNext2 = random.Next2;
    std::array<std::uint32_t, RANDOM_TABLE_SIZE> table{};
    for (std::size_t index = 0; index < RANDOM_TABLE_SIZE; ++index)
        table[index] = static_cast<std::uint32_t>(random.Table[index]);
    digest.mRandomTableHash = HashRandomTable(table);
}

void ReadHouses(StateDigest& digest)
{
    const auto& houses = HouseClass::Array;
    RequireReadableVector(houses, MAX_HOUSE_ARRAY_COUNT, "Invalid House array during state digest");
    Fnv1a32 hash;
    // 数组顺序即 ArrayIndex 顺序；空指针项按默认字段加入，保留数组布局。
    for (int index = 0; index < houses.Count; ++index)
    {
        HouseDigestFields fields;
        if (const HouseClass* house = houses.Items[index])
        {
            fields.mArrayIndex = house->ArrayIndex;
            fields.mTypeIndex = house->Type ? house->Type->ArrayIndex : -1;
            fields.mColorSchemeIndex = house->ColorSchemeIndex;
            fields.mBalance = house->Balance;
            fields.mPowerOutput = house->PowerOutput;
            fields.mPowerDrain = house->PowerDrain;
            fields.mAllyMask = static_cast<std::uint32_t>(house->Allies.data);
        }
        AddHouseDigest(hash, fields);
    }
    digest.mHouseCount = static_cast<std::uint32_t>(houses.Count);
    digest.mHouseHash = hash.GetValue();
}

// 只读字段：Location 而不是 GetCoords，CurrentMission 而不是 GetCurrentMission，不经过任何虚函数。
template <typename TObject>
ObjectArrayDigest ReadObjects(const DynamicVectorClass<TObject*>& objects, const char* error)
{
    RequireReadableVector(objects, MAX_OBJECT_ARRAY_COUNT, error);
    Fnv1a32 hash;
    for (int index = 0; index < objects.Count; ++index)
    {
        ObjectDigestFields fields;
        if (const TObject* object = objects.Items[index])
        {
            fields.mUniqueId = static_cast<std::uint32_t>(object->UniqueID);
            fields.mTypeIndex = object->Type ? object->Type->ArrayIndex : -1;
            fields.mX = object->Location.X;
            fields.mY = object->Location.Y;
            fields.mZ = object->Location.Z;
            fields.mHealth = object->Health;
            fields.mMission = static_cast<std::int32_t>(object->CurrentMission);
            fields.mOwnerIndex = object->Owner ? object->Owner->ArrayIndex : -1;
        }
        AddObjectDigest(hash, fields);
    }
    return {static_cast<std::uint32_t>(objects.Count), hash.GetValue()};
}

// 只取每项的值字节，按数组顺序；变量名不参与。
template <std::size_t COUNT>
std::uint32_t ReadVariableValues(const Variable (&variables)[COUNT]) noexcept
{
    std::array<std::uint8_t, COUNT> values{};
    for (std::size_t index = 0; index < COUNT; ++index)
        values[index] = static_cast<std::uint8_t>(variables[index].Value);
    return HashVariableValues(values);
}

ObjectArrayDigest& At(StateDigest& digest, DigestObjectKind kind) noexcept
{
    return digest.mObjects[static_cast<std::size_t>(kind)];
}
}

StateDigest ReadStateDigest()
{
    const ScenarioClass* scenario = ScenarioClass::Instance;
    if (!scenario)
        throw std::runtime_error("No scenario during state digest");
    StateDigest digest;
    digest.mFrame = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    ReadRandom(scenario->Random, digest);
    digest.mUniqueIdCounter = static_cast<std::uint32_t>(scenario->UniqueID);
    ReadHouses(digest);
    At(digest, DigestObjectKind::Unit) = ReadObjects(UnitClass::Array, "Invalid Unit array during state digest");
    At(digest, DigestObjectKind::Infantry) =
        ReadObjects(InfantryClass::Array, "Invalid Infantry array during state digest");
    At(digest, DigestObjectKind::Building) =
        ReadObjects(BuildingClass::Array, "Invalid Building array during state digest");
    At(digest, DigestObjectKind::Aircraft) =
        ReadObjects(AircraftClass::Array, "Invalid Aircraft array during state digest");

    const SessionClass& session = SessionClass::Instance;
    digest.mGameMode = static_cast<std::uint32_t>(session.GameMode);
    digest.mMaxAhead = session.MaxAhead;
    digest.mFrameSendRate = session.FrameSendRate;
    digest.mDesiredFrameRate = session.DesiredFrameRate;
    digest.mTimingTimeStamp = ReadGlobalInt(target::TIMING_TIME_STAMP);
    digest.mFrameSyncTargetFrame = ReadGlobalInt(target::FRAME_SYNC_TARGET_FRAME);
    // 只读队列计数字段；DoList 在 0x8B41F8，事件数组从 +0xC 起（与 DO_LIST_EVENTS 一致）。
    digest.mDoListCount = EventClass::DoList.Count;

    std::memcpy(&digest.mSpecialFlags, &scenario->SpecialFlags, sizeof(digest.mSpecialFlags));
    digest.mVariablesChanged = ReadByte(scenario->VariablesChanged);
    digest.mGlobalVariablesHash = ReadVariableValues(scenario->GlobalVariables);
    digest.mLocalVariablesHash = ReadVariableValues(scenario->LocalVariables);
    return digest;
}
}
