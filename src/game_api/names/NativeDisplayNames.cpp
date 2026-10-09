#include "NativeDisplayNames.h"
#include "../../core/encoding/Utf8.h"

using byte = unsigned char;

#include <AbstractTypeClass.h>

#include <cstring>

namespace rareplay::game_api
{
std::string ReadTypeDisplayName(const AbstractTypeClass* type)
{
    if (!type)
        return "未知类型";
    constexpr std::size_t MAX_UI_NAME_CODE_UNITS = 256;
    auto name = core::encoding::Utf8(type->UIName, MAX_UI_NAME_CODE_UNITS);
    return name.empty() ? std::string(type->ID, strnlen_s(type->ID, sizeof(type->ID))) : name;
}
}
