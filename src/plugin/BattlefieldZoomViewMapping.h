#pragma once

#include "../game_api/zoom/BattlefieldZoomAdapter.h"
#include "../viewer/ReplayViewModel.h"

namespace rareplay::plugin
{
/** 缩放不可用时给观看者的原因（用户可见文本）。 */
inline constexpr char ZOOM_REASON_OUT_OF_MEMORY[] = "内存不足，无法缩放";
inline constexpr char ZOOM_REASON_FAILED[] = "缩放出错，已停用";
inline constexpr char ZOOM_REASON_LAYOUT_UNSUPPORTED[] = "当前布局不支持缩放";

/**
 * 档位（rareplay::zoom::ZOOM_LEVELS 的下标）的百分比：物理 ÷ 逻辑 × 100，四舍五入到整数
 * （2/3 为 67、1/3 为 33）。下标越界时返回 100。
 */
int GetBattlefieldZoomPercent(int levelIndex) noexcept;

/**
 * 把缩放适配器的状态换算为快照里的缩放信息；纯函数，不读任何全局状态。
 * - 未启用（录制、回放尚未开始或已结束）：默认值，界面不显示控件；
 * - 资源不足停用：不可用，原因“内存不足，无法缩放”；
 * - 缩放核、引擎状态异常或内部异常停用：不可用，原因“缩放出错，已停用”；
 * - 已启用但最近一次读取的门控关闭：不可用，原因“当前布局不支持缩放”；
 * - 其余可用：百分比取已应用档位；能否继续放大或缩小按请求档位与允许范围判断（换档从请求档位出发）；
 *   请求档位不等于已应用档位时为“待应用”。
 */
viewer::BattlefieldZoomView BuildBattlefieldZoomView(const game_api::zoom::BattlefieldZoomStatus& status);
}
