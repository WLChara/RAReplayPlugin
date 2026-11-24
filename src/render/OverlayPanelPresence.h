#pragma once

#include "../ui/ReplayHud.h"

namespace rareplay::viewer
{
class ReplayViewExchange;
}

namespace rareplay::render
{
/**
 * 渲染线程在 HUD 绘制数据已提交到本帧后调用：报告 HUD 已呈现，并注明面板是否完整覆盖原生侧栏区。
 * 游戏线程只凭此心跳停止原生侧栏绘制与关闭原生提示，因此绘制失败或提前返回的路径不得调用它。
 */
void ReportPanelPresence(viewer::ReplayViewExchange& exchange, const viewer::ReplayViewSnapshot& snapshot,
                         const ui::ReplayHud& hud) noexcept;
}
