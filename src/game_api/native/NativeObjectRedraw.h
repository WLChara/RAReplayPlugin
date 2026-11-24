#pragma once

namespace rareplay::game_api::native
{
/**
 * 对已放置在地图上的对象补做原生 ObjectClass::Mark(Down) 的重画请求：对象 IsOnMap 且不在 Limbo 时调用其
 * MarkForRedraw（只置 NeedsRedraw 并请求战术区重画，属渲染状态），否则什么也不做。
 *
 * @param object ObjectClass*（可为空，空时什么也不做）；不转移所有权，只在本次调用中使用。
 *
 * 只供 SetHeight 内飞机影子绘制的中和 Hook 在游戏线程同步调用：该 Hook 跳过原生 Mark 对，
 * 以此保持与原生相同的重画效果。不补做 Tether 的 RequestRedraw，也不修改任何模拟状态。
 */
void MarkPlacedObjectForRedraw(const void* object) noexcept;
}
