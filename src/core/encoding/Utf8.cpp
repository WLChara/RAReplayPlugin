#include "Utf8.h"

#include <Windows.h>

#include <limits>

namespace rareplay::core::encoding
{
std::string Utf8(const wchar_t* text, std::size_t maxLength)
{
    if (!text || maxLength == 0)
        return {};

    std::size_t length = 0;
    while (length < maxLength && text[length] != L'\0')
        ++length;
    if (length == 0 || length > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return {};

    const auto inputLength = static_cast<int>(length);
    // flags=0 保留显示文本对孤立代理代码单元的替换行为。
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, inputLength, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};

    std::string result(static_cast<std::size_t>(bytes), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, inputLength,
        result.data(), bytes, nullptr, nullptr);
    if (written != bytes)
        return {};
    return result;
}
}
