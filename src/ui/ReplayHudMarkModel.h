#pragma once

#include "ReplayHudLayout.h"
#include "../viewer/SuperWeaponMarks.h"

#include <vector>

/**
 * 超武落点图标在客户区中的排布（不含 ImGui 的纯逻辑）：位置换算、大小、错开、裁剪与不透明度。
 * 落点图标只由插件在画面上层绘制，不创建任何游戏对象，不影响模拟与对象 ID。
 */
namespace rareplay::ui
{
/** 100% 档下没有图标时通用标记的大小，与原生 cameo 同为 60×48。 */
constexpr float MARK_FALLBACK_WIDTH = 60.0f;
constexpr float MARK_FALLBACK_HEIGHT = 48.0f;
/** 100% 档下图标最长一边的上限；更大的图标按比例缩小，保持原始宽高比。 */
constexpr float MARK_MAX_EXTENT = 96.0f;

struct MarkPlacement
{
    const viewer::SuperWeaponMark* mMark = nullptr;
    /** 落点在客户区的位置（错开之前）。 */
    UiPoint mAnchor;
    /** 图标框，客户区坐标；与其他落点太近时按序错开少许。 */
    UiRect mIconRect;
    /** 当前不透明度（0 到 0.6）。 */
    float mOpacity = 0.0f;
    /** 图标框与战术视图相交且不透明度大于 0；屏幕外或已完全淡出的落点为假，不绘制。 */
    bool mIsVisible = false;
};

struct MarkLayout
{
    /** 战术视图（地图区）在客户区中的范围；落点图标被裁剪在其中。 */
    UiRect mClip;
    /** 与 frame.mMarks 一一对应，顺序相同（最旧的在前）。 */
    std::vector<MarkPlacement> mPlacements;
};

/**
 * 按帧里的画布坐标、画面映射与 HUD 缩放排布全部落点。图标按原始像素比例绘制，大小随 HUD 缩放；
 * 与更早的落点距离小于图标大小的 0.7 倍时，按序向右上错开，最多错开 4 次，避免完全重叠。
 * 错开只依赖落点之间的相对位置，镜头滚动时整体平移，不会抖动。
 */
MarkLayout LayoutSuperWeaponMarks(const viewer::SuperWeaponMarkFrame& frame, const NativePointMapper& mapper,
    float scale);
}
