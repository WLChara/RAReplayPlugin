#pragma once

#include <cstdint>
#include <span>
#include <string>

class HouseClass;

namespace rareplay::game_api
{
/**
 * 回放界面与统计共同的“参战玩家”判定：非空、有 Type 且不是多人 passive 房屋（中立、特殊等）。
 * 观察者房屋不单独排除，与现有玩家列表保持一致。只能在游戏线程调用。
 */
bool IsParticipatingHouse(const HouseClass* house) noexcept;

/**
 * 房屋的玩家名称：UIName（最多 21 个代码单元）转 UTF-8，为空时“玩家 N”（N 为 ArrayIndex + 1）。
 * 玩家列表与雷达事件共用，保证同一玩家在两处显示相同；house 不得为空，只能在游戏线程调用。
 */
std::string ReadHouseDisplayName(const HouseClass* house);

/** 房屋颜色，0xRRGGBB；house 不得为空。 */
std::uint32_t ReadHouseColorRgb(const HouseClass* house) noexcept;

/**
 * 校验后的 HouseClass::Array 视图，元素可为空指针。数组计数或指针异常时抛出 std::runtime_error，
 * 调用方据此停止本次采样而不是越界读取。
 */
std::span<HouseClass* const> GetValidatedHouseArray();
}
