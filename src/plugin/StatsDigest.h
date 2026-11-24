#pragma once

#include "../stats/StatsObservation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rareplay::plugin
{
/** 统计摘要的游戏时间间隔：每跨过一个整 60 秒边界写一行。 */
constexpr std::int64_t STATS_DIGEST_INTERVAL_MS = 60'000;

/**
 * 游戏时间从 previousMs 增加到 currentMs 时是否跨过（或正好到达）一个 STATS_DIGEST_INTERVAL_MS 的整数倍边界；
 * 时间不增加时为假。一帧最多跨过一个边界（每帧时长远小于 60 秒），所以每个边界只写一行。
 */
bool HasCrossedStatsDigestBoundary(std::int64_t previousMs, std::int64_t currentMs) noexcept;

/**
 * 统计摘要的技术日志，供对比“直接看完”与“跳转后”两份日志：跳转目标之后的各行应逐行相同。格式为
 * “Replay stats digest: frame=N gameMs=T houses=[<房屋下标>:money=…,score=…,units=…,kills=…,built=…; …]”，
 * 房屋按下标升序。数值全部取自统计层这一帧的最新观测：money 为可用资金，score 为分数（负值按 0，与 HUD 相同），
 * kills 为摧毁的敌方单位与建筑之和（下标超出击杀矩阵时为“-”），built 为四类工厂产出之和；统计层没有部队数的观测，
 * units 一律为“-”。
 */
std::string FormatStatsDigest(std::uint32_t frameNumber, std::int64_t gameTimeMs,
    const std::vector<stats::HouseObservation>& houses);
}
