#pragma once

#include <cstdint>

namespace rareplay::game_api::native
{
/**
 * 仅供 47F136 的游戏线程同步绘制回调；上层不接收此帧基址与返回指针。
 * 原始像素只读，副本指针只在本次绘制中有效，异常由 Hook 回退到原像素。
 */
const std::uint8_t* GetViewerShroudPixels(std::uintptr_t nativeFrame,
    const std::uint8_t* originalPixels);
/** 绘制调用之外的游戏线程使用；清空显示副本并请求全视区遮罩重绘。 */
void InvalidateViewerShroud();
/**
 * 只登记整视口脏矩形（要求重新生成遮罩显示缓冲），不清显示副本；绘制调用之外、世界存在时的游戏线程使用。
 * 快进中不绘制时脏矩形被逐帧丢弃，半黑幕开启时在完整重绘前用它补上遮罩重绘。
 */
void RegisterViewerShroudFullRedraw() noexcept;
/** 会话销毁只释放副本，不触碰可能已开始销毁的原生世界。 */
void ClearViewerShroud() noexcept;
}
