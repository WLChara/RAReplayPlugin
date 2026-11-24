#include "ReplayHudFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace rareplay::ui
{
std::string FormatGameClock(std::int64_t milliseconds)
{
    const std::int64_t totalSeconds = std::max<std::int64_t>(milliseconds, 0) / 1000;
    const long long hours = static_cast<long long>(totalSeconds / 3600);
    const int minutes = static_cast<int>(totalSeconds / 60 % 60);
    const int seconds = static_cast<int>(totalSeconds % 60);
    char buffer[40];
    if (hours > 0)
        std::snprintf(buffer, sizeof(buffer), "%lld:%02d:%02d", hours, minutes, seconds);
    else
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, seconds);
    return buffer;
}

namespace
{
// 按 divisor 缩放并附单位：整除时不带小数，否则最多两位小数并去掉末尾的 0（1.5k、2.25M）。
std::string FormatScaledCredits(bool isNegative, std::int64_t magnitude, std::int64_t divisor, const char* suffix)
{
    char buffer[48];
    if (magnitude % divisor == 0)
    {
        std::snprintf(buffer, sizeof(buffer), "%s%lld%s", isNegative ? "-" : "",
                      static_cast<long long>(magnitude / divisor), suffix);
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%s%.2f", isNegative ? "-" : "",
                  static_cast<double>(magnitude) / static_cast<double>(divisor));
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text + suffix;
}
}

std::string FormatCreditsTick(std::int64_t credits)
{
    const bool isNegative = credits < 0;
    // 取反前先放宽到无符号，避免最小值取反溢出。
    const std::uint64_t magnitude = isNegative ? 0ull - static_cast<std::uint64_t>(credits)
                                               : static_cast<std::uint64_t>(credits);
    if (magnitude < 1000)
    {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%s%llu", isNegative ? "-" : "", static_cast<unsigned long long>(magnitude));
        return buffer;
    }
    const std::int64_t clamped = static_cast<std::int64_t>(std::min<std::uint64_t>(magnitude, INT64_MAX));
    if (clamped >= 1'000'000)
        return FormatScaledCredits(isNegative, clamped, 1'000'000, "M");
    return FormatScaledCredits(isNegative, clamped, 1000, "k");
}

std::string FormatPercent(const float* percent)
{
    if (percent == nullptr || !std::isfinite(*percent))
        return "—";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f%%", static_cast<double>(*percent));
    return buffer;
}

std::string FormatRatio(const float* ratio)
{
    if (ratio == nullptr || !std::isfinite(*ratio))
        return "—";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(*ratio));
    return buffer;
}

std::int64_t GetNiceCreditsStep(double minStep) noexcept
{
    if (!(minStep > 1.0))
        return 1;
    double magnitude = 1.0;
    while (magnitude * 10.0 <= minStep)
        magnitude *= 10.0;
    for (const double factor : {1.0, 2.0, 5.0, 10.0})
    {
        if (magnitude * factor >= minStep)
            return static_cast<std::int64_t>(magnitude * factor);
    }
    return static_cast<std::int64_t>(magnitude * 10.0);
}

std::int64_t GetNiceTimeStepMs(std::int64_t minStepMs) noexcept
{
    constexpr std::array<std::int64_t, 12> STEPS_SECONDS = {10, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 14400, 28800};
    for (const std::int64_t seconds : STEPS_SECONDS)
    {
        if (seconds * 1000 >= minStepMs)
            return seconds * 1000;
    }
    return STEPS_SECONDS.back() * 1000;
}

ChipLayout LayoutChips(const std::vector<float>& widths, float availableWidth, float gap, float lineHeight,
                       float lineGap) noexcept
{
    ChipLayout layout;
    const float limit = std::max(1.0f, availableWidth);
    float x = 0.0f;
    float y = 0.0f;
    layout.mPlacements.reserve(widths.size());
    for (const float rawWidth : widths)
    {
        const float width = std::min(std::max(rawWidth, 1.0f), limit);
        if (x > 0.0f && x + gap + width > limit)
        {
            x = 0.0f;
            y += lineHeight + lineGap;
        }
        else if (x > 0.0f)
        {
            x += gap;
        }
        layout.mPlacements.push_back({x, y, width});
        x += width;
    }
    if (!layout.mPlacements.empty())
        layout.mHeight = y + lineHeight;
    return layout;
}
}
