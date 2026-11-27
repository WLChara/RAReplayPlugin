#pragma once

#include "StateDigest.h"

namespace rareplay::game_api
{
/**
 * 只读计算当前世界的状态摘要；只在游戏线程的帧完成点、世界存在时调用。
 *
 * 严格只读：只读取字段与数组，不调用任何原生函数或虚函数，不消耗随机数，不分配对象 ID，不写游戏状态；
 * 也不调用 Game::ComputeFrameCRC（它会推进场景随机数）。所读字段的偏移在实现文件中以 static_assert 核对。
 * 场景不存在、房屋或对象数组的计数与指针异常时抛出 std::runtime_error，调用方据此停用摘要。
 */
StateDigest ReadStateDigest();
}
