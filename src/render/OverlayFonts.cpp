#include "OverlayFonts.h"
#include "../core/encoding/Utf8.h"
#include "../ui/ReplayHudScale.h"

#include "imgui.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace rareplay::render
{
namespace
{
// 默认字体的设计尺寸；中文字体缺失时按同一比例缩放。
constexpr float DEFAULT_FONT_PIXELS = 13.0f;

float ClampScale(float scale) noexcept
{
    return std::isfinite(scale) ? std::clamp(scale, 0.5f, 4.0f) : 1.0f;
}
}

bool LoadOverlayFont(ImGuiIO& io, float scale) noexcept
{
    const float fontScale = ClampScale(scale);
    try
    {
        io.Fonts->Clear();
        std::array<wchar_t, MAX_PATH> windowsDirectory{};
        const UINT length = GetWindowsDirectoryW(windowsDirectory.data(),
                                                  static_cast<UINT>(windowsDirectory.size()));
        if (length > 0 && length < windowsDirectory.size())
        {
            const std::wstring root(windowsDirectory.data(), length);
            // 中文字形不需要水平过采样；关闭后同字号图集面积减半，200% 时与原 100% 设置同为 4096×4096。
            ImFontConfig config;
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.PixelSnapH = true;
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
                    io.Fonts->AddFontFromFileTTF(utf8Path.c_str(), ui::HUD_BASE_FONT_PIXELS * fontScale, &config,
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
    ImFontConfig fallback;
    fallback.SizePixels = DEFAULT_FONT_PIXELS * fontScale;
    io.Fonts->AddFontDefault(&fallback);
    return false;
}
}
