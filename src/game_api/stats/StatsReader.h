#pragma once

#include "../../stats/StatsObservation.h"

namespace rareplay::game_api
{
/**
 * 只读采样回放世界中的统计原始值；只在游戏线程的帧完成点调用，不修改任何游戏状态，
 * 也不调用有副作用的函数（例如 MapClass::GetCellAt）。所读字段的偏移在 StatsReader.cpp 中以 static_assert 核对。
 * 读到损坏的房屋数组时抛出 std::runtime_error，调用方据此停用本次会话的统计。
 */

/** 每帧读取的低成本数据：帧时长输入，以及各参战房屋的资金、电力、盟友、击杀与产出计数、升级标志。 */
void ReadFrameObservation(stats::FrameObservation& outFrame);

/**
 * 较重的产线数据：遍历各参战房屋的建筑与 FactoryClass::Array，按“兵营（步兵）”“重工（陆地载具，不含船厂）”归类。
 * 工厂建筑只计存活、已放置、不在建造动画中的；Cloning 建筑不算工厂（推断）。
 */
void ReadProductionObservation(stats::ProductionObservation& outProduction);
}
