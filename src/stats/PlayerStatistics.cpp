#include "PlayerStatistics.h"

#include <algorithm>
#include <limits>

namespace rareplay::stats
{
namespace
{
bool IsMatrixIndex(int houseIndex) noexcept
{
    return houseIndex >= 0 && houseIndex < KILL_MATRIX_SIZE;
}

std::uint32_t NonNegative(std::int64_t value) noexcept
{
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, std::numeric_limits<std::uint32_t>::max()));
}

bool IsAlly(const HouseObservation& self, const HouseObservation& other) noexcept
{
    // 调用方已保证 other.mHouseIndex 在 [0, KILL_MATRIX_SIZE)，移位不会越过 32 位。
    return ((self.mAllyMask >> other.mHouseIndex) & 1u) != 0;
}

viewer::CombatStatistics ComputeCombat(const HouseObservation& self, const std::vector<HouseObservation>& houses)
{
    std::int64_t destroyedUnits = 0;
    std::int64_t destroyedBuildings = 0;
    std::int64_t lostUnits = 0;
    std::int64_t lostBuildings = 0;
    for (const HouseObservation& other : houses)
    {
        if (other.mHouseIndex == self.mHouseIndex || !IsMatrixIndex(other.mHouseIndex) || IsAlly(self, other))
            continue;
        const auto victim = static_cast<std::size_t>(other.mHouseIndex);
        const auto attacker = static_cast<std::size_t>(self.mHouseIndex);
        destroyedUnits += std::max(0, self.mCounters.mKilledUnitsOfHouses[victim]);
        destroyedBuildings += std::max(0, self.mCounters.mKilledBuildingsOfHouses[victim]);
        lostUnits += std::max(0, other.mCounters.mKilledUnitsOfHouses[attacker]);
        lostBuildings += std::max(0, other.mCounters.mKilledBuildingsOfHouses[attacker]);
    }
    viewer::CombatStatistics combat;
    combat.mDestroyedUnits = NonNegative(destroyedUnits);
    combat.mDestroyedBuildings = NonNegative(destroyedBuildings);
    combat.mLostUnits = NonNegative(lostUnits);
    combat.mLostBuildings = NonNegative(lostBuildings);
    const std::int64_t lostTotal = lostUnits + lostBuildings;
    if (lostTotal > 0)
        combat.mKillLossRatio = static_cast<float>(static_cast<double>(destroyedUnits + destroyedBuildings) /
                                                   static_cast<double>(lostTotal));
    return combat;
}
}

std::vector<viewer::PlayerStatistics> ComputePlayerStatistics(const std::vector<HouseObservation>& houses,
    std::uint32_t& outSkippedHouses)
{
    outSkippedHouses = 0;
    std::vector<viewer::PlayerStatistics> results;
    results.reserve(houses.size());
    for (const HouseObservation& house : houses)
    {
        viewer::PlayerStatistics result;
        if (IsMatrixIndex(house.mHouseIndex))
            result.mCombat = ComputeCombat(house, houses);
        else
            ++outSkippedHouses;
        const HouseCounters& counters = house.mCounters;
        result.mBuiltBuildings = NonNegative(counters.mBuiltBuildings);
        result.mBuiltVehicles = NonNegative(counters.mBuiltVehicles);
        result.mBuiltInfantry = NonNegative(counters.mBuiltInfantry);
        result.mBuiltAircraft = NonNegative(counters.mBuiltAircraft);
        result.mScore = NonNegative(counters.mPointTotal);
        results.push_back(result);
    }
    return results;
}
}
