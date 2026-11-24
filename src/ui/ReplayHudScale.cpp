#include "ReplayHudScale.h"

#include <algorithm>
#include <cmath>

namespace rareplay::ui
{
int GetAutoHudScalePercent(float clientHeight) noexcept
{
    if (!std::isfinite(clientHeight) || clientHeight <= 0.0f)
        return HUD_SCALE_MIN_PERCENT;
    // 以档位序号计算：0 对应 100%，每档 25%。加 0.5 后取下整，使正中的高度（如 1080）落到较大档。
    const double steps = (static_cast<double>(clientHeight) / HUD_SCALE_REFERENCE_HEIGHT - 1.0) *
                         (100.0 / HUD_SCALE_STEP_PERCENT);
    constexpr int MAX_STEP = (HUD_SCALE_MAX_PERCENT - HUD_SCALE_MIN_PERCENT) / HUD_SCALE_STEP_PERCENT;
    const int step = std::clamp(static_cast<int>(std::floor(steps + 0.5)), 0, MAX_STEP);
    return HUD_SCALE_MIN_PERCENT + step * HUD_SCALE_STEP_PERCENT;
}

int ResolveHudScalePercent(viewer::HudScaleSetting setting, float clientHeight) noexcept
{
    if (setting == viewer::HudScaleSetting::Auto)
        return GetAutoHudScalePercent(clientHeight);
    const int percent = static_cast<int>(setting);
    if (percent < HUD_SCALE_MIN_PERCENT || percent > HUD_SCALE_MAX_PERCENT)
        return GetAutoHudScalePercent(clientHeight);
    return percent;
}
}
