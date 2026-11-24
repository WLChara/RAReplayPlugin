#pragma once

#include "StatsObservation.h"
#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <vector>

namespace rareplay::stats
{
/**
 * 由各房屋的击杀矩阵、工厂产出计数与分数算出战斗与建造类统计，结果与输入按下标对齐。
 *
 * 口径：
 * - “敌方”：houses 中非本方、非盟友的参战房屋；中立等 passive 房屋不在 houses 里，因此天然不是敌方。
 * - 摧毁 = Σ 敌方 j（本方.KilledXOfHouses[j]），分单位与建筑。
 * - 损失 = Σ 敌方 j（j.KilledXOfHouses[本方]），即各敌方对本方的击杀；不用含无击杀者死亡的 TotalKilled*。
 * - 战损比 = 摧毁总数 ÷ 损失总数，损失为 0 时为空。
 * - 建造 = 四类 FactoryProduced 计数；分数 = max(PointTotal, 0)；负计数按 0 处理。
 * 击杀矩阵只有 KILL_MATRIX_SIZE 项且原生不检查越界：房屋下标不在 [0, KILL_MATRIX_SIZE) 时，
 * 该房屋没有战斗统计，也不会被当作别人的敌方；每个这样的房屋使 outSkippedHouses 加一。
 *
 * 结果中的操作、APM 与升级字段留空，由 MatchStatistics 补充。
 */
std::vector<viewer::PlayerStatistics> ComputePlayerStatistics(const std::vector<HouseObservation>& houses,
    std::uint32_t& outSkippedHouses);
}
