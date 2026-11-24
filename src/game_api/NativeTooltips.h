#pragma once

#include <optional>

namespace rareplay::game_api
{
/**
 * 原生提示（CCToolTip::Instance 的 ToolTipManager::IsActive）当前是否启用；实例不存在时为空。
 * 只在游戏线程调用。
 */
std::optional<bool> ReadNativeTooltipState() noexcept;

/**
 * 调用 ToolTipManager::SetState 开关原生提示；实例不存在时不做任何事。
 * 关闭时原生会停止提示计时器并收起当前提示，不影响模拟。只在游戏线程调用。
 */
void SetNativeTooltipState(bool isActive) noexcept;
}
