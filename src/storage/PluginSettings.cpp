#include "PluginSettings.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace rareplay::storage
{
namespace
{
constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";
constexpr std::string_view HUD_SECTION = "Hud";
constexpr std::string_view SCALE_KEY = "Scale";
constexpr std::uintmax_t MAX_SETTINGS_BYTES = 64 * 1024;

std::string_view Trim(std::string_view text) noexcept
{
    const auto isSpace = [](char value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; };
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto lower = [](char value) { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; };
        if (lower(left[index]) != lower(right[index]))
            return false;
    }
    return true;
}

std::vector<std::string_view> SplitLines(std::string_view text)
{
    if (text.substr(0, UTF8_BOM.size()) == UTF8_BOM)
        text.remove_prefix(UTF8_BOM.size());
    std::vector<std::string_view> lines;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return lines;
}

// 返回节名；不是节标题时返回空。
std::optional<std::string_view> ParseSection(std::string_view trimmed) noexcept
{
    if (trimmed.size() < 2 || trimmed.front() != '[')
        return std::nullopt;
    const std::size_t close = trimmed.find(']');
    if (close == std::string_view::npos)
        return std::nullopt;
    return Trim(trimmed.substr(1, close - 1));
}

bool IsComment(std::string_view trimmed) noexcept
{
    return !trimmed.empty() && (trimmed.front() == ';' || trimmed.front() == '#');
}

bool IsScaleKeyLine(std::string_view trimmed, std::string_view& outValue) noexcept
{
    const std::size_t equals = trimmed.find('=');
    if (equals == std::string_view::npos || !EqualsIgnoreCase(Trim(trimmed.substr(0, equals)), SCALE_KEY))
        return false;
    outValue = trimmed.substr(equals + 1);
    return true;
}

std::optional<int> ParseScaleValue(std::string_view value) noexcept
{
    const std::size_t comment = value.find_first_of(";#");
    value = Trim(value.substr(0, comment));
    if (EqualsIgnoreCase(value, "auto"))
        return 0;
    if (!value.empty() && value.back() == '%')
        value = Trim(value.substr(0, value.size() - 1));
    int percent = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), percent);
    if (value.empty() || result.ec != std::errc() || result.ptr != value.data() + value.size() ||
        percent == 0 || !IsValidHudScalePercent(percent))
        return std::nullopt;
    return percent;
}

std::string FormatScaleLine(const PluginSettings& settings)
{
    const int percent = IsValidHudScalePercent(settings.mHudScalePercent) ? settings.mHudScalePercent
                                                                          : DEFAULT_HUD_SCALE_PERCENT;
    return std::string(SCALE_KEY) + "=" + (percent == 0 ? std::string("auto") : std::to_string(percent));
}

bool ReadSmallFile(const std::filesystem::path& path, std::string& outText, std::string& outError)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
    {
        outError = "cannot query settings file size: " + error.message();
        return false;
    }
    if (size > MAX_SETTINGS_BYTES)
    {
        outError = "settings file is larger than 64 KiB";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        outError = "cannot open settings file for reading";
        return false;
    }
    outText.assign(static_cast<std::size_t>(size), '\0');
    if (size != 0 && !file.read(outText.data(), static_cast<std::streamsize>(size)))
    {
        outError = "cannot read settings file";
        return false;
    }
    return true;
}
}

bool IsValidHudScalePercent(int percent) noexcept
{
    return percent == 0 || (percent >= 100 && percent <= 200 && percent % 25 == 0);
}

PluginSettings ParsePluginSettings(std::string_view text, std::string& outWarning)
{
    outWarning.clear();
    PluginSettings settings;
    bool isInHud = false;
    for (const std::string_view line : SplitLines(text))
    {
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty() || IsComment(trimmed))
            continue;
        if (const auto section = ParseSection(trimmed))
        {
            isInHud = EqualsIgnoreCase(*section, HUD_SECTION);
            continue;
        }
        std::string_view value;
        if (!isInHud || !IsScaleKeyLine(trimmed, value))
            continue;
        // 同一键出现多次时以最后一次为准；非法值回退默认值而不是沿用前值，便于用户察觉。
        if (const auto percent = ParseScaleValue(value))
        {
            settings.mHudScalePercent = *percent;
            outWarning.clear();
        }
        else
        {
            settings.mHudScalePercent = DEFAULT_HUD_SCALE_PERCENT;
            outWarning = "invalid [Hud] Scale value '" + std::string(Trim(value)) + "', using 100%";
        }
    }
    return settings;
}

std::string UpdatePluginSettingsText(std::string_view existing, const PluginSettings& settings)
{
    const std::vector<std::string_view> lines = SplitLines(existing);
    std::vector<std::string> output;
    output.reserve(lines.size() + 3);
    const std::string scaleLine = FormatScaleLine(settings);
    bool isInHud = false;
    bool hasWrittenScale = false;
    std::size_t firstHudHeader = SIZE_MAX;
    for (const std::string_view line : lines)
    {
        const std::string_view trimmed = Trim(line);
        const bool isContent = !trimmed.empty() && !IsComment(trimmed);
        if (const auto section = isContent ? ParseSection(trimmed) : std::optional<std::string_view>())
        {
            isInHud = EqualsIgnoreCase(*section, HUD_SECTION);
            if (isInHud && firstHudHeader == SIZE_MAX)
                firstHudHeader = output.size();
            output.emplace_back(line);
            continue;
        }
        std::string_view value;
        if (isInHud && isContent && IsScaleKeyLine(trimmed, value))
        {
            // 只保留一行 Scale，避免旧的重复键在下次读取时覆盖新值。
            if (!hasWrittenScale)
                output.push_back(scaleLine);
            hasWrittenScale = true;
            continue;
        }
        output.emplace_back(line);
    }
    if (!hasWrittenScale)
    {
        if (firstHudHeader != SIZE_MAX)
        {
            output.insert(output.begin() + static_cast<std::ptrdiff_t>(firstHudHeader) + 1, scaleLine);
        }
        else
        {
            while (!output.empty() && Trim(output.back()).empty())
                output.pop_back();
            if (!output.empty())
                output.emplace_back();
            output.push_back("[" + std::string(HUD_SECTION) + "]");
            output.push_back(scaleLine);
        }
    }
    std::string text;
    for (const std::string& line : output)
    {
        text += line;
        text += "\r\n";
    }
    return text;
}

PluginSettingsLoadResult LoadPluginSettings(const std::filesystem::path& path, PluginSettings& outSettings,
                                            std::string& outMessage) noexcept
{
    outSettings = {};
    outMessage.clear();
    try
    {
        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            if (!error)
                return PluginSettingsLoadResult::Missing;
            outMessage = "cannot check settings file: " + error.message();
            return PluginSettingsLoadResult::ReadFailed;
        }
        std::string text;
        if (!ReadSmallFile(path, text, outMessage))
            return PluginSettingsLoadResult::ReadFailed;
        outSettings = ParsePluginSettings(text, outMessage);
        return outMessage.empty() ? PluginSettingsLoadResult::Loaded : PluginSettingsLoadResult::Invalid;
    }
    catch (...)
    {
        outSettings = {};
        outMessage = "unexpected exception while reading settings";
        return PluginSettingsLoadResult::ReadFailed;
    }
}

bool SavePluginSettings(const std::filesystem::path& path, const PluginSettings& settings,
                        std::string& outError) noexcept
{
    outError.clear();
    try
    {
        std::string existing;
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error)
        {
            outError = "cannot check settings file: " + error.message();
            return false;
        }
        // 已有文件读不出来时不覆盖，避免丢失用户在其他节写入的内容。
        if (exists && !ReadSmallFile(path, existing, outError))
            return false;
        const std::string text = UpdatePluginSettingsText(existing, settings);

        // 同目录临时文件保证原子替换不跨卷；进程号区分同时运行的多个游戏实例。
        auto temporary = path;
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            outError = "cannot create temporary settings file, Win32 error " + std::to_string(GetLastError());
            return false;
        }
        DWORD written = 0;
        const bool isWritten = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
            written == text.size() && FlushFileBuffers(file);
        const DWORD writeError = isWritten ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!isWritten)
        {
            DeleteFileW(temporary.c_str());
            outError = "cannot write temporary settings file, Win32 error " + std::to_string(writeError);
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD moveError = GetLastError();
            DeleteFileW(temporary.c_str());
            outError = "cannot replace settings file, Win32 error " + std::to_string(moveError);
            return false;
        }
        return true;
    }
    catch (...)
    {
        outError = "unexpected exception while saving settings";
        return false;
    }
}
}
