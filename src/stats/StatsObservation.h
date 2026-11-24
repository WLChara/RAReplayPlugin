#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace rareplay::stats
{
/**
 * 游戏适配层读取后交给统计逻辑的原始观测值。这里只有值类型，不含引擎对象；
 * 对应引擎字段的偏移由 game_api 的 StatsReader.cpp 以 static_assert 核对。
 */

/** HouseClass 击杀矩阵的固定大小；原生数组只有 20 项且不做越界检查。 */
constexpr int KILL_MATRIX_SIZE = 20;

struct HouseCounters
{
    /** HouseClass::KilledUnitsOfHouses：写在击杀方名下，下标为受害方 ArrayIndex。 */
    std::array<std::int32_t, KILL_MATRIX_SIZE> mKilledUnitsOfHouses{};
    /** HouseClass::KilledBuildingsOfHouses，语义同上。 */
    std::array<std::int32_t, KILL_MATRIX_SIZE> mKilledBuildingsOfHouses{};
    /** 工厂产出计数（CounterClass，+0x10 的 Total），舰船计在载具里。 */
    std::int32_t mBuiltBuildings = 0;
    std::int32_t mBuiltVehicles = 0;
    std::int32_t mBuiltInfantry = 0;
    std::int32_t mBuiltAircraft = 0;
    /** HouseClass::PointTotal，可能为负。 */
    std::int32_t mPointTotal = 0;
};

/** 单个参战房屋在某一帧完成点的观测。 */
struct HouseObservation
{
    /** HouseClass::ArrayIndex。 */
    int mHouseIndex = -1;
    bool mIsHuman = false;
    /** Available_Money()：现金加可用矿石价值。 */
    std::int64_t mCredits = 0;
    int mPowerOutput = 0;
    int mPowerDrain = 0;
    /** HouseClass::Allies，每位代表一个房屋下标；只有下标小于 KILL_MATRIX_SIZE 的位被使用。 */
    std::uint32_t mAllyMask = 0;
    bool mBarracksInfiltrated = false;
    bool mWarFactoryInfiltrated = false;
    HouseCounters mCounters;
};

/** 决定每帧时长的输入，含义见 GameClock。 */
struct FrameTimingInput
{
    /** 联机（LAN 或 Internet）：每帧时长由 SessionClass::DesiredFrameRate 决定。 */
    bool mIsNetwork = false;
    /** GameOptionsClass::GameSpeed，单机每帧时长由它决定。 */
    int mGameSpeed = 0;
    int mDesiredFrameRate = 0;
};

struct FrameObservation
{
    FrameTimingInput mTiming;
    std::vector<HouseObservation> mHouses;
};

/** 一座在制工厂（FactoryClass）的状态，来自 FactoryClass 的 Production.Value、IsSuspended、IsManual、OnHold。 */
struct FactoryObservation
{
    int mProgress = 0;
    bool mIsSuspended = false;
    bool mIsManual = false;
    bool mIsOnHold = false;
};

/** 一条产线（兵营或重工）的观测。 */
struct LineObservation
{
    /** 存活、已放置、不在建造动画中的对应工厂建筑数；为零表示无工厂。 */
    std::uint32_t mFactoryBuildings = 0;
    /** 该产线上有产品（或队列）的工厂；为空表示没有任何项目在生产。 */
    std::vector<FactoryObservation> mFactories;
};

struct HouseProductionObservation
{
    int mHouseIndex = -1;
    LineObservation mBarracks;
    LineObservation mWarFactory;
};

struct ProductionObservation
{
    std::vector<HouseProductionObservation> mHouses;
};
}
