#include "../src/storage/PluginSettings.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::storage::LoadPluginSettings;
using rareplay::storage::ParsePluginSettings;
using rareplay::storage::PluginSettings;
using rareplay::storage::PluginSettingsLoadResult;
using rareplay::storage::SavePluginSettings;
using rareplay::storage::UpdatePluginSettingsText;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

int Parse(const std::string& text, bool& outHasWarning)
{
    std::string warning;
    const PluginSettings settings = ParsePluginSettings(text, warning);
    outHasWarning = !warning.empty();
    return settings.mHudScalePercent;
}

void CheckParse()
{
    bool hasWarning = false;
    Require(Parse("[Hud]\nScale=150\n", hasWarning) == 150 && !hasWarning, "Plain value parses");
    Require(Parse("\xEF\xBB\xBF[hud]\r\n  scale = AUTO  \r\n", hasWarning) == 0 && !hasWarning,
            "BOM, CRLF, case and spaces are tolerated");
    Require(Parse("[Hud]\nScale=175% ; 本机偏好\n", hasWarning) == 175 && !hasWarning,
            "Percent sign and inline comment are tolerated");
    Require(Parse("; 注释\n# 注释\n[Hud]\nScale=200\n", hasWarning) == 200 && !hasWarning, "Comment lines are skipped");
    // 缺省与回退值是 100%；auto 只在文件明确写出时生效。
    Require(Parse("", hasWarning) == 100 && !hasWarning, "Empty file means the 100% default");
    Require(Parse("[Other]\nScale=auto\n", hasWarning) == 100 && !hasWarning, "Scale outside [Hud] is ignored");
    Require(Parse("[Hud]\nOther=1\n", hasWarning) == 100 && !hasWarning, "Missing Scale key means 100%");
    Require(Parse("[Hud]\nScale=110\n", hasWarning) == 100 && hasWarning, "Off-step value falls back to 100%");
    Require(Parse("[Hud]\nScale=250\n", hasWarning) == 100 && hasWarning, "Out-of-range value falls back to 100%");
    Require(Parse("[Hud]\nScale=abc\n", hasWarning) == 100 && hasWarning, "Text value falls back to 100%");
    Require(Parse("[Hud]\nScale=0\n", hasWarning) == 100 && hasWarning, "Numeric zero is not a valid spelling");
    Require(Parse("[Hud]\nScale=\n", hasWarning) == 100 && hasWarning, "Empty value falls back to 100%");
    Require(Parse("[Hud]\nScale=auto\nScale=bad\n", hasWarning) == 100 && hasWarning,
            "Last invalid duplicate wins and falls back to 100%");
    Require(Parse("[Hud]\nScale=bad\nScale=125\n", hasWarning) == 125 && !hasWarning,
            "Last valid duplicate wins");
}

void CheckUpdateText()
{
    const std::string added = UpdatePluginSettingsText("", {150});
    Require(added == "[Hud]\r\nScale=150\r\n", "Empty file gets a [Hud] section");

    const std::string kept = UpdatePluginSettingsText("; 用户注释\r\n[Other]\r\nKey=1\r\n\r\n", {0});
    Require(kept == "; 用户注释\r\n[Other]\r\nKey=1\r\n\r\n[Hud]\r\nScale=auto\r\n",
            "Other sections and comments are preserved: " + kept);

    const std::string replaced = UpdatePluginSettingsText(
        "[Hud]\nOther=x\nScale=100\nScale=125\n[Tail]\nScale=7\n", {200});
    Require(replaced == "[Hud]\r\nOther=x\r\nScale=200\r\n[Tail]\r\nScale=7\r\n",
            "Only one [Hud] Scale remains and other sections are untouched: " + replaced);

    const std::string inserted = UpdatePluginSettingsText("[Hud]\nOther=x\n", {125});
    Require(inserted == "[Hud]\r\nScale=125\r\nOther=x\r\n", "Missing key is inserted under [Hud]: " + inserted);

    const std::string invalid = UpdatePluginSettingsText("", {111});
    Require(invalid == "[Hud]\r\nScale=100\r\n", "Invalid in-memory value is written as the 100% default");
    Require(PluginSettings{}.mHudScalePercent == 100, "Default settings are 100%");

    std::string warning;
    Require(ParsePluginSettings(replaced, warning).mHudScalePercent == 200 && warning.empty(),
            "Updated text round-trips");
}

void CheckFiles(const std::filesystem::path& directory)
{
    const auto path = directory / rareplay::storage::PLUGIN_SETTINGS_FILE_NAME;
    PluginSettings settings{125};
    std::string message;
    Require(LoadPluginSettings(path, settings, message) == PluginSettingsLoadResult::Missing &&
            settings.mHudScalePercent == 100, "Missing file loads as the 100% default");

    std::string error;
    Require(SavePluginSettings(path, {175}, error), "Save creates the file: " + error);
    Require(ReadAll(path) == "[Hud]\r\nScale=175\r\n", "Saved text is UTF-8 INI");
    Require(LoadPluginSettings(path, settings, message) == PluginSettingsLoadResult::Loaded &&
            settings.mHudScalePercent == 175, "Saved value loads back");

    WriteAll(path, "; 保留\r\n[Hud]\r\nScale=125\r\n[Other]\r\nA=1\r\n");
    Require(SavePluginSettings(path, {0}, error), "Save over an existing file: " + error);
    Require(ReadAll(path) == "; 保留\r\n[Hud]\r\nScale=auto\r\n[Other]\r\nA=1\r\n",
            "Existing comments and sections survive a save");
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        Require(entry.path().filename() == rareplay::storage::PLUGIN_SETTINGS_FILE_NAME,
                "No temporary file is left behind");

    WriteAll(path, "[Hud]\r\nScale=999\r\n");
    Require(LoadPluginSettings(path, settings, message) == PluginSettingsLoadResult::Invalid &&
            settings.mHudScalePercent == 100 && !message.empty(), "Invalid file value loads as 100% with a message");

    WriteAll(path, "[Hud]\r\nScale=auto\r\n");
    Require(LoadPluginSettings(path, settings, message) == PluginSettingsLoadResult::Loaded &&
            settings.mHudScalePercent == 0, "Explicit auto is still loaded as auto");

    WriteAll(path, std::string(70 * 1024, 'x'));
    Require(LoadPluginSettings(path, settings, message) == PluginSettingsLoadResult::ReadFailed &&
            settings.mHudScalePercent == 100, "Oversized file is a read failure that falls back to 100%");
    Require(!SavePluginSettings(path, {150}, error) && !error.empty(),
            "Unreadable existing file is not overwritten");
    Require(ReadAll(path).size() == 70 * 1024, "Failed save keeps the original file");

    // 写失败：目标目录不存在、目标是目录。两者都返回 false 并给出原因，不抛异常。
    Require(!SavePluginSettings(directory / L"missing" / L"settings.ini", {150}, error) && !error.empty(),
            "Missing directory is a reported write failure");
    const auto folder = directory / L"folder.ini";
    std::filesystem::create_directory(folder);
    Require(!SavePluginSettings(folder, {150}, error) && !error.empty(),
            "Directory target is a reported write failure");
    Require(LoadPluginSettings(folder, settings, message) == PluginSettingsLoadResult::ReadFailed &&
            settings.mHudScalePercent == 100, "Directory target reads as failure and 100%");
}
}

int main()
{
    std::filesystem::path directory;
    try
    {
        CheckParse();
        CheckUpdateText();
        // 只写系统临时目录下的独立子目录，测试结束后删除。
        directory = std::filesystem::temp_directory_path() /
            (L"rareplay-settings-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        CheckFiles(directory);
        std::filesystem::remove_all(directory);
        std::cout << "Plugin settings tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::error_code ignored;
        if (!directory.empty())
            std::filesystem::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
