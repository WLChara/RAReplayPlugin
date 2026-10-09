#include "../src/core/encoding/Utf8.h"

#include <array>
#include <limits>
#include <stdexcept>

namespace
{
using rareplay::core::encoding::Utf8;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void TestEmptyInputs()
{
    Require(Utf8(nullptr, 12).empty(), "null input must be empty");
    Require(Utf8(L"ABC", 0).empty(), "zero bound must be empty");
    Require(Utf8(L"", 1).empty(), "empty prefix must be empty");
    Require(Utf8(L"", (std::numeric_limits<std::size_t>::max)()).empty(),
        "first NUL must end scanning even with a large bound");
}

void TestBoundedPrefix()
{
    const std::array<wchar_t, 3> unterminated{L'A', L'B', L'C'};
    Require(Utf8(unterminated.data(), unterminated.size()) == "ABC",
        "bounded input does not require a terminator");
    Require(Utf8(unterminated.data(), 2) == "AB", "bound must truncate the prefix");
    const std::array<wchar_t, 5> embedded{L'A', L'B', L'\0', L'C', L'D'};
    Require(Utf8(embedded.data(), embedded.size()) == "AB", "first NUL must end the prefix");
    Require(Utf8(L"ABC", 4) == "ABC", "terminator must not be present in output");
}

void TestUnicode()
{
    Require(Utf8(L"中文", 2) == "\xE4\xB8\xAD\xE6\x96\x87",
        "Chinese text must use UTF-8 bytes");
    Require(Utf8(L"中文", 1) == "\xE4\xB8\xAD", "bound is measured in UTF-16 code units");
    const std::array<wchar_t, 2> nonBmp{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)};
    Require(Utf8(nonBmp.data(), nonBmp.size()) == "\xF0\x9F\x98\x80",
        "surrogate pair must encode one non-BMP character");
    Require(Utf8(nonBmp.data(), 1) == "\xEF\xBF\xBD",
        "truncated high surrogate must retain replacement behavior");
    const std::array<wchar_t, 2> isolatedLow{static_cast<wchar_t>(0xDE00), L'A'};
    Require(Utf8(isolatedLow.data(), isolatedLow.size()) == "\xEF\xBF\xBD" "A",
        "isolated low surrogate must be replaced without losing subsequent text");
}
}

int main()
{
    TestEmptyInputs();
    TestBoundedPrefix();
    TestUnicode();
}
