#pragma once

#include "../viewer/ReplayViewModel.h"

namespace rareplay::ui
{
constexpr int HUD_SCALE_MIN_PERCENT = 100;
constexpr int HUD_SCALE_MAX_PERCENT = 200;
constexpr int HUD_SCALE_STEP_PERCENT = 25;
/** 自动档以客户区高度 960 为 100%。 */
constexpr float HUD_SCALE_REFERENCE_HEIGHT = 960.0f;
/** 100% 档位下 HUD 字体的像素高度；宿主按“基准 × 档位”重建字体图集。 */
constexpr float HUD_BASE_FONT_PIXELS = 18.0f;

/**
 * 自动档：取最接近 clientHeight / 960 的 25% 档位，恰在两档正中时取较大档，并夹在 100%–200%。
 * clientHeight 为客户区逻辑像素；非有限或非正值按 100%。
 */
int GetAutoHudScalePercent(float clientHeight) noexcept;

/** 返回实际生效的百分比（100–200，步长 25）；宿主与界面必须使用同一结果，保证字体与排版一致。 */
int ResolveHudScalePercent(viewer::HudScaleSetting setting, float clientHeight) noexcept;
}
