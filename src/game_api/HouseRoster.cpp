#include "HouseRoster.h"
#include "../core/encoding/Utf8.h"

using byte = unsigned char;

#include <HouseClass.h>
#include <HouseTypeClass.h>

#include <stdexcept>

namespace rareplay::game_api
{
namespace
{
// 原生房屋数组的上限远小于它；超过说明读到了损坏的数据。
constexpr int MAX_HOUSE_ARRAY_COUNT = 64;
}

bool IsParticipatingHouse(const HouseClass* house) noexcept
{
    return house != nullptr && house->Type != nullptr && !house->Type->MultiplayPassive;
}

std::string ReadHouseDisplayName(const HouseClass* house)
{
    auto name = core::encoding::Utf8(house->UIName, 21);
    if (name.empty())
        name = "玩家 " + std::to_string(house->ArrayIndex + 1);
    return name;
}

std::uint32_t ReadHouseColorRgb(const HouseClass* house) noexcept
{
    return (static_cast<std::uint32_t>(house->Color.R) << 16) | (static_cast<std::uint32_t>(house->Color.G) << 8) |
           house->Color.B;
}

std::span<HouseClass* const> GetValidatedHouseArray()
{
    const auto& houses = HouseClass::Array;
    if (houses.Count < 0 || houses.Count > MAX_HOUSE_ARRAY_COUNT || (houses.Count > 0 && !houses.Items))
        throw std::runtime_error("Invalid House array during replay HUD sampling");
    return {houses.Items, static_cast<std::size_t>(houses.Count)};
}
}
