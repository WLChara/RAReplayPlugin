#pragma once

#include "../viewer/ReplayViewModel.h"

#include <optional>

namespace rareplay::game_api
{
/** 原生侧栏能否被回放面板替换的前提；只含值，不暴露 SDK 对象。 */
struct NativeSidebarState
{
    /** GameOptionsClass::SidebarMode 为真表示侧栏在右。 */
    bool mIsOnRight = false;
    bool mIsArmageddonMode = false;
};

/** 只在游戏线程调用。 */
NativeSidebarState ReadNativeSidebarState() noexcept;

/**
 * 读取 SidebarBounds 作为面板区域（减去 WindowBounds 原点）。
 * 侧栏在左、Armageddon 或边界不完整落在画布内时返回空，此时不得显示面板或跳过原生绘制。只在游戏线程调用。
 */
std::optional<viewer::NativeUiLayout> ReadNativeUiLayout() noexcept;

/** 录制方本地玩家（LocalPlayer）的雷达当前是否可用；只读，不强制开启雷达。只在游戏线程调用。 */
bool IsNativeRadarAvailable() noexcept;

/**
 * 置侧栏、背景、当前 Strip 与能量条的重画标志，让原生侧栏完整重画。
 * 只在游戏线程调用；在 SidebarClass::Draw 中途调用时，调用方还须令本次 Draw 按强制重画执行。
 */
void RequestNativeSidebarRedraw() noexcept;
}
