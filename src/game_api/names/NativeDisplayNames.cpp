#include "NativeDisplayNames.h"
#include "../../core/encoding/Utf8.h"

using byte = unsigned char;

#include <AbstractTypeClass.h>

#include <cstring>

namespace rareplay::game_api
{
namespace
{
constexpr std::size_t MAX_UI_NAME_CODE_UNITS = 256;
}

std::string ReadTypeUiName(const AbstractTypeClass* type)
{
    return type ? core::encoding::Utf8(type->UIName, MAX_UI_NAME_CODE_UNITS) : std::string();
}

std::string ReadTypeDisplayName(const AbstractTypeClass* type)
{
    if (!type)
        return "未知类型";
    auto name = ReadTypeUiName(type);
    return name.empty() ? std::string(type->ID, strnlen_s(type->ID, sizeof(type->ID))) : name;
}
}
