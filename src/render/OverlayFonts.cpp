#include "OverlayFonts.h"
#include "../core/encoding/Utf8.h"

#include "imgui.h"

#include <Windows.h>

#include <array>
#include <string>

namespace rareplay::render
{
bool LoadOverlayFont(ImGuiIO& io) noexcept
{
    try
    {
        std::array<wchar_t, MAX_PATH> windowsDirectory{};
        const UINT length = GetWindowsDirectoryW(windowsDirectory.data(),
                                                  static_cast<UINT>(windowsDirectory.size()));
        if (length > 0 && length < windowsDirectory.size())
        {
            const std::wstring root(windowsDirectory.data(), length);
            constexpr const wchar_t* FONT_NAMES[] = {L"msyh.ttc", L"msyh.ttf", L"simhei.ttf"};
            for (const wchar_t* fontName : FONT_NAMES)
            {
                const std::wstring path = root + L"\\Fonts\\" + fontName;
                const DWORD attributes = GetFileAttributesW(path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue;
                }
                const std::string utf8Path = core::encoding::Utf8(path.c_str(), path.size());
                if (!utf8Path.empty() &&
                    io.Fonts->AddFontFromFileTTF(utf8Path.c_str(), 18.0f, nullptr,
                                                 io.Fonts->GetGlyphRangesChineseFull()) != nullptr)
                {
                    return true;
                }
            }
        }
    }
    catch (...)
    {
        OutputDebugStringW(L"RAReplayPlugin: 读取系统中文字体时发生异常，使用默认字体。\n");
    }
    OutputDebugStringW(L"RAReplayPlugin: 未找到可用的系统中文字体，HUD 使用默认字体。\n");
    io.Fonts->AddFontDefault();
    return false;
}
}
