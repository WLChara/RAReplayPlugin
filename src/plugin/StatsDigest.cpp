#include "StatsDigest.h"

#include "../stats/PlayerStatistics.h"

#include <algorithm>
#include <numeric>

namespace rareplay::plugin
{
bool HasCrossedStatsDigestBoundary(std::int64_t previousMs, std::int64_t currentMs) noexcept
{
    if (currentMs <= previousMs || currentMs < STATS_DIGEST_INTERVAL_MS)
        return false;
    // 游戏时间从 0 起单调不减；负值不会出现，按 0 处理以免除法向零取整带来误判。
    const std::int64_t previous = std::max<std::int64_t>(previousMs, 0);
    return currentMs / STATS_DIGEST_INTERVAL_MS > previous / STATS_DIGEST_INTERVAL_MS;
}

std::string FormatStatsDigest(std::uint32_t frameNumber, std::int64_t gameTimeMs,
    const std::vector<stats::HouseObservation>& houses)
{
    std::uint32_t skippedHouses = 0;
    // 战斗与建造统计与输入按下标对齐；与 HUD 的统计同一函数，口径一致。
    const std::vector<viewer::PlayerStatistics> statistics = stats::ComputePlayerStatistics(houses, skippedHouses);
    std::vector<std::size_t> order(houses.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&houses](std::size_t left, std::size_t right) {
        return houses[left].mHouseIndex < houses[right].mHouseIndex;
    });
    std::string text = "Replay stats digest: frame=" + std::to_string(frameNumber) +
        " gameMs=" + std::to_string(gameTimeMs) + " houses=[";
    bool isFirst = true;
    for (const std::size_t index : order)
    {
        const stats::HouseObservation& house = houses[index];
        const viewer::PlayerStatistics& playerStats = statistics[index];
        if (!isFirst)
            text += "; ";
        isFirst = false;
        text += std::to_string(house.mHouseIndex) + ":money=" + std::to_string(house.mCredits) +
            ",score=" + std::to_string(playerStats.mScore) + ",units=-,kills=";
        if (playerStats.mCombat)
            text += std::to_string(static_cast<std::uint64_t>(playerStats.mCombat->mDestroyedUnits) +
                playerStats.mCombat->mDestroyedBuildings);
        else
            text += "-";
        text += ",built=" + std::to_string(playerStats.GetBuiltTotal());
    }
    text += "]";
    return text;
}
}
