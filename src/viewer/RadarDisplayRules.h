#pragma once

#include <cstddef>
#include <cstdint>

namespace rareplay::viewer
{
/**
 * 超武落点图标与雷达事件提示栏的显示规则。所有时间都是回放的游戏时间（毫秒），不是墙钟时间：
 * 暂停时游戏时间不前进，所以图标与提示都不会淡出。游戏线程据此丢弃过期落点，界面据此计算不透明度，
 * 两边必须使用同一份常量与函数。
 */

/** 超武落点图标从释放帧起显示 15 秒。 */
constexpr std::int64_t SUPERWEAPON_MARK_LIFETIME_MS = 15'000;
/** 最后 3 秒线性淡出到 0。 */
constexpr std::int64_t SUPERWEAPON_MARK_FADE_MS = 3'000;
/** 落点图标未淡出时的不透明度，约 60%。 */
constexpr float SUPERWEAPON_MARK_MAX_OPACITY = 0.60f;
/** 同时存在的落点图标上限；防护用，超过时丢弃最旧的。 */
constexpr std::size_t SUPERWEAPON_MARK_LIMIT = 32;

/** 提示栏的“最近”模式：每条事件显示 30 秒游戏时间。 */
constexpr std::int64_t RADAR_EVENT_LIFETIME_MS = 30'000;
/** 最后 5 秒线性淡出到 0（实施细节）。 */
constexpr std::int64_t RADAR_EVENT_FADE_MS = 5'000;
/** “最近”模式最多同时显示的条数，最新的在上。 */
constexpr std::size_t RADAR_EVENT_RECENT_LIMIT = 5;
/** 历史最多保留的条数，超过时丢弃最旧的。 */
constexpr std::size_t RADAR_EVENT_HISTORY_LIMIT = 200;

/**
 * 线性淡出系数：年龄不超过 lifetime − fade 时为 1，到 lifetime 时降为 0，之后为 0。
 * 年龄为负（游戏时间回退等异常）按 0 处理，即完全不透明。
 */
constexpr float GetFadeFactor(std::int64_t ageMs, std::int64_t lifetimeMs, std::int64_t fadeMs) noexcept
{
    if (ageMs <= 0 || ageMs <= lifetimeMs - fadeMs)
        return 1.0f;
    if (ageMs >= lifetimeMs)
        return 0.0f;
    return static_cast<float>(lifetimeMs - ageMs) / static_cast<float>(fadeMs);
}

/** 落点图标的不透明度（0–0.6）：未淡出时为 SUPERWEAPON_MARK_MAX_OPACITY，末段线性降到 0。 */
constexpr float GetSuperWeaponMarkOpacity(std::int64_t ageMs) noexcept
{
    return SUPERWEAPON_MARK_MAX_OPACITY *
           GetFadeFactor(ageMs, SUPERWEAPON_MARK_LIFETIME_MS, SUPERWEAPON_MARK_FADE_MS);
}

/** 落点图标是否已过期（年龄达到显示时长）。 */
constexpr bool IsSuperWeaponMarkExpired(std::int64_t ageMs) noexcept
{
    return ageMs >= SUPERWEAPON_MARK_LIFETIME_MS;
}

/** “最近”模式下事件的不透明度（0–1）。 */
constexpr float GetRadarEventOpacity(std::int64_t ageMs) noexcept
{
    return GetFadeFactor(ageMs, RADAR_EVENT_LIFETIME_MS, RADAR_EVENT_FADE_MS);
}

/** “最近”模式下事件是否仍显示（年龄未达到 30 秒）。 */
constexpr bool IsRadarEventRecent(std::int64_t ageMs) noexcept
{
    return ageMs < RADAR_EVENT_LIFETIME_MS;
}
}
