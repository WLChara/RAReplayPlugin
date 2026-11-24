#include "../src/plugin/StatsDigest.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using rareplay::plugin::FormatStatsDigest;
using rareplay::plugin::HasCrossedStatsDigestBoundary;
using rareplay::plugin::STATS_DIGEST_INTERVAL_MS;
using rareplay::stats::HouseObservation;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckBoundaries()
{
    Require(STATS_DIGEST_INTERVAL_MS == 60'000, "the digest is written every 60 s of game time");
    Require(!HasCrossedStatsDigestBoundary(0, 16) && !HasCrossedStatsDigestBoundary(59'000, 59'999),
            "no digest before the first minute");
    Require(HasCrossedStatsDigestBoundary(59'984, 60'000), "reaching exactly 60 s writes a digest");
    Require(HasCrossedStatsDigestBoundary(59'990, 60'006), "crossing 60 s writes a digest");
    Require(!HasCrossedStatsDigestBoundary(60'000, 60'016), "the same minute is not written twice");
    Require(HasCrossedStatsDigestBoundary(119'950, 120'050), "the second minute is written");
    Require(!HasCrossedStatsDigestBoundary(120'050, 120'050), "a repeated frame (no time added) writes nothing");
    Require(!HasCrossedStatsDigestBoundary(130'000, 70'000), "time never goes backward");

    // 按 16 ms 一帧累计 10 分钟：恰好 10 行，每行对应一个整分钟边界。
    int lines = 0;
    std::int64_t previous = 0;
    for (int frame = 0; frame < 600'000 / 16 + 10; ++frame)
    {
        const std::int64_t current = previous + 16;
        if (HasCrossedStatsDigestBoundary(previous, current))
            ++lines;
        previous = current;
    }
    Require(lines == 10, "one digest per minute over ten minutes: " + std::to_string(lines));
}

HouseObservation MakeHouse(int houseIndex, std::int64_t credits, std::int32_t points)
{
    HouseObservation house;
    house.mHouseIndex = houseIndex;
    house.mIsHuman = true;
    house.mCredits = credits;
    house.mCounters.mPointTotal = points;
    return house;
}

void CheckFormat()
{
    // 两个敌对房屋（不结盟），观测顺序与下标顺序相反，摘要按下标升序。
    HouseObservation second = MakeHouse(3, 900, -5);
    second.mCounters.mKilledUnitsOfHouses[1] = 2;
    second.mCounters.mBuiltInfantry = 4;
    HouseObservation first = MakeHouse(1, 12'345, 1'200);
    first.mCounters.mKilledUnitsOfHouses[3] = 7;
    first.mCounters.mKilledBuildingsOfHouses[3] = 1;
    first.mCounters.mBuiltBuildings = 5;
    first.mCounters.mBuiltVehicles = 6;
    first.mCounters.mBuiltInfantry = 7;
    first.mCounters.mBuiltAircraft = 1;
    // 下标超出 20 项击杀矩阵：没有战斗统计，kills 为“-”。
    HouseObservation outside = MakeHouse(25, 0, 10);
    const std::string text = FormatStatsDigest(3'750, 60'000, {second, first, outside});
    Require(text == "Replay stats digest: frame=3750 gameMs=60000 houses=["
                    "1:money=12345,score=1200,units=-,kills=8,built=19; "
                    "3:money=900,score=0,units=-,kills=2,built=4; "
                    "25:money=0,score=10,units=-,kills=-,built=0]",
            "digest text: " + text);
    Require(FormatStatsDigest(0, 60'000, {}) == "Replay stats digest: frame=0 gameMs=60000 houses=[]",
            "no houses gives an empty list");
    // 同样的输入得到同样的文字：跳转与直接观看的日志可以逐行比较。
    Require(FormatStatsDigest(3'750, 60'000, {second, first, outside}) == text, "the digest is deterministic");
}
}

int main()
{
    try
    {
        CheckBoundaries();
        CheckFormat();
        std::cout << "Stats digest tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
