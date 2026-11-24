#include "../src/stats/PlayerStatistics.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::stats;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

HouseObservation MakeHouse(int index, std::uint32_t allyMask)
{
    HouseObservation house;
    house.mHouseIndex = index;
    house.mAllyMask = allyMask;
    return house;
}

// 0 与 2 是盟友，1 是敌人；下标 3 的房屋是中立方，不在参战列表里。
void CheckEnemyFilterAndSums()
{
    HouseObservation a = MakeHouse(0, 0b0101);
    HouseObservation b = MakeHouse(1, 0b0010);
    HouseObservation c = MakeHouse(2, 0b0101);
    a.mCounters.mKilledUnitsOfHouses[1] = 10;
    a.mCounters.mKilledBuildingsOfHouses[1] = 3;
    a.mCounters.mKilledUnitsOfHouses[2] = 50;      // 击杀盟友不算摧毁
    a.mCounters.mKilledUnitsOfHouses[0] = 7;       // 击杀自己不算摧毁
    a.mCounters.mKilledUnitsOfHouses[3] = 99;      // 中立方不在列表中，不是敌方
    b.mCounters.mKilledUnitsOfHouses[0] = 4;
    b.mCounters.mKilledBuildingsOfHouses[0] = 1;
    c.mCounters.mKilledUnitsOfHouses[0] = 9;       // 盟友对本方的击杀不算损失
    c.mCounters.mKilledUnitsOfHouses[1] = 6;
    std::uint32_t skipped = 99;
    const auto results = ComputePlayerStatistics({a, b, c}, skipped);
    Require(results.size() == 3 && skipped == 0, "results align with input and nothing is skipped");

    Require(results[0].mCombat.has_value(), "house 0 has combat statistics");
    const auto& combatA = *results[0].mCombat;
    Require(combatA.mDestroyedUnits == 10 && combatA.mDestroyedBuildings == 3, "only enemy kills are destroyed");
    Require(combatA.mLostUnits == 4 && combatA.mLostBuildings == 1, "loss sums enemy kills on this house");
    Require(combatA.mKillLossRatio && std::abs(*combatA.mKillLossRatio - 13.0f / 5.0f) < 1e-5f,
            "ratio is destroyed total over lost total");

    // B 的敌人是 A 和 C：摧毁取 B 对 0、2 的击杀；损失取 A、C 对 B 的击杀。
    b.mCounters.mKilledUnitsOfHouses[2] = 2;
    a.mCounters.mKilledBuildingsOfHouses[1] = 3;
    const auto second = ComputePlayerStatistics({a, b, c}, skipped);
    const auto& combatB = *second[1].mCombat;
    Require(combatB.mDestroyedUnits == 4 + 2 && combatB.mDestroyedBuildings == 1, "enemy of B are houses 0 and 2");
    Require(combatB.mLostUnits == 10 + 6 && combatB.mLostBuildings == 3, "B loses to A and C");
}

void CheckRatioDenominatorZero()
{
    HouseObservation a = MakeHouse(0, 0b0001);
    HouseObservation b = MakeHouse(1, 0b0010);
    a.mCounters.mKilledUnitsOfHouses[1] = 5;
    std::uint32_t skipped = 0;
    const auto results = ComputePlayerStatistics({a, b}, skipped);
    Require(results[0].mCombat && !results[0].mCombat->mKillLossRatio && results[0].mCombat->mDestroyedUnits == 5,
            "no losses means no ratio but the destroyed count stays");
    Require(results[1].mCombat && results[1].mCombat->mKillLossRatio &&
            *results[1].mCombat->mKillLossRatio == 0.0f && results[1].mCombat->mLostUnits == 5,
            "no kills with losses is a ratio of zero, not missing");
}

void CheckScoreBuiltAndNegativeCounters()
{
    HouseObservation a = MakeHouse(0, 0b0001);
    a.mCounters.mPointTotal = -250;
    a.mCounters.mBuiltBuildings = 3;
    a.mCounters.mBuiltVehicles = 4;
    a.mCounters.mBuiltInfantry = 5;
    a.mCounters.mBuiltAircraft = -9;
    a.mCounters.mKilledUnitsOfHouses[1] = -3;
    HouseObservation b = MakeHouse(1, 0b0010);
    b.mCounters.mPointTotal = 1234;
    std::uint32_t skipped = 0;
    auto results = ComputePlayerStatistics({a, b}, skipped);
    Require(results[0].mScore == 0 && results[1].mScore == 1234, "score is max(PointTotal, 0)");
    Require(results[0].mBuiltBuildings == 3 && results[0].mBuiltVehicles == 4 && results[0].mBuiltInfantry == 5 &&
            results[0].mBuiltAircraft == 0 && results[0].GetBuiltTotal() == 12, "built counts sum four categories");
    Require(results[0].mCombat->mDestroyedUnits == 0, "negative counters are treated as zero");
}

// 击杀矩阵只有 20 项：下标越界（含 -1）的房屋没有战斗统计，也不会被当成别人的敌方，并被计数。
void CheckOutOfRangeHouses()
{
    HouseObservation a = MakeHouse(0, 0b0001);
    HouseObservation farHouse = MakeHouse(25, 0);
    HouseObservation negative = MakeHouse(-1, 0);
    HouseObservation edge = MakeHouse(19, 0);
    HouseObservation b = MakeHouse(1, 0b0010);
    a.mCounters.mKilledUnitsOfHouses[1] = 2;
    a.mCounters.mKilledUnitsOfHouses[19] = 3;
    edge.mCounters.mKilledUnitsOfHouses[0] = 1;
    farHouse.mCounters.mKilledUnitsOfHouses[0] = 100;
    negative.mCounters.mKilledUnitsOfHouses[0] = 100;
    farHouse.mCounters.mPointTotal = 70;
    std::uint32_t skipped = 0;
    const auto results = ComputePlayerStatistics({a, farHouse, negative, edge, b}, skipped);
    Require(skipped == 2, "two houses are outside the kill matrix");
    Require(!results[1].mCombat && !results[2].mCombat, "out-of-range houses have no combat statistics");
    Require(results[1].mScore == 70, "out-of-range houses still report score");
    Require(results[0].mCombat->mDestroyedUnits == 2 + 3 && results[0].mCombat->mLostUnits == 1,
            "index 19 is inside the matrix and out-of-range houses are never enemies");
    Require(results[3].mCombat.has_value(), "the last valid index works");
}

void CheckEmptyInput()
{
    std::uint32_t skipped = 5;
    Require(ComputePlayerStatistics({}, skipped).empty() && skipped == 0, "empty input");
}
}

int main()
{
    try
    {
        CheckEnemyFilterAndSums();
        CheckRatioDenominatorZero();
        CheckScoreBuiltAndNegativeCounters();
        CheckOutOfRangeHouses();
        CheckEmptyInput();
        std::cout << "Player statistics tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
