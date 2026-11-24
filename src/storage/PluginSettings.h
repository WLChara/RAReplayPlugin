#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rareplay::storage
{
/** 设置文件位于游戏目录，作用范围是这台机器上的这个游戏目录。 */
constexpr wchar_t PLUGIN_SETTINGS_FILE_NAME[] = L"RAReplayPluginSettings.ini";

/** 没有设置文件、缺少 Scale 或值非法时使用的界面缩放：100%（“自动”仍可选）。 */
constexpr int DEFAULT_HUD_SCALE_PERCENT = 100;

/**
 * 本机回放界面设置。文件格式为 UTF-8 INI：[Hud] 节下 Scale=auto|100|125|150|175|200。
 * mHudScalePercent 为 0 表示自动，其余为百分比；与 viewer::HudScaleSetting 的数值一致。
 */
struct PluginSettings
{
    int mHudScalePercent = DEFAULT_HUD_SCALE_PERCENT;
};

/** 0 或 100–200 中步长 25 的值。 */
bool IsValidHudScalePercent(int percent) noexcept;

/**
 * 解析设置文本：容忍 UTF-8 BOM、CRLF、首尾空白、; 与 # 注释，节名与键名不区分大小写，数值可带 %。
 * 缺少 Scale 时为默认 100%；值非法时退回 100%，并在 outWarning 中给出英文原因（否则清空）。
 */
PluginSettings ParsePluginSettings(std::string_view text, std::string& outWarning);

/** 只替换或补入 [Hud] 节的 Scale，保留其他节、键与注释；结果使用 CRLF 换行。 */
std::string UpdatePluginSettingsText(std::string_view existing, const PluginSettings& settings);

enum class PluginSettingsLoadResult { Loaded, Missing, Invalid, ReadFailed };

/**
 * 读取设置文件。Missing、ReadFailed 与 Invalid 时 outSettings 都是可用的默认或回退值（100%），
 * 调用方只需对 Invalid、ReadFailed 记录 outMessage。文件超过 64 KiB 视为读取失败。
 */
PluginSettingsLoadResult LoadPluginSettings(const std::filesystem::path& path, PluginSettings& outSettings,
                                            std::string& outMessage) noexcept;

/**
 * 在同目录写临时文件后原子替换，保留原文件中的其他内容。失败时原文件不变，
 * 返回 false 并在 outError 中给出英文原因；不抛异常。
 */
bool SavePluginSettings(const std::filesystem::path& path, const PluginSettings& settings,
                        std::string& outError) noexcept;
}
