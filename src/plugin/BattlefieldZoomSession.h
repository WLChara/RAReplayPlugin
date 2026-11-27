#pragma once

#include "../game_api/zoom/BattlefieldZoomAdapter.h"

namespace rareplay::plugin
{
/**
 * 一次回放播放会话的战局缩放生命周期：回放开始时启用游戏线程上的缩放适配器，结束、失败或会话结束时停用
 * （写回物理视口并释放逻辑资源），并把适配器积累的技术日志写入插件日志。录制时不 Begin，适配器保持未启用，
 * 全部缩放 Hook 原样返回。档位策略由适配器持有（物理战术区只在游戏中可知），本类只转交观看者的意图。
 * 由 PlaybackViewingSession 持有，只在游戏线程、DrawOnTop 之外使用；全部接口不抛异常，缩放失败只停用缩放。
 */
class BattlefieldZoomSession final
{
public:
    /** 回放开始时调用一次；请求档位为 1×。 */
    void Begin() noexcept;
    /**
     * 进程内重开后再次开始：与 Begin 相同地启用，然后请求恢复上次 Stop 前观看者请求的档位
     * （以战术区中心为锚点，在下一次绘制时应用，范围按新的物理战术区夹取）。从未 Stop 过时同 Begin。
     */
    void BeginWithPreviousZoom() noexcept;
    /**
     * 回放结束、失败、意外异常或进程内重开前调用；录制或未 Begin 时也可调用，此时没有副作用。
     * 停用前记下观看者请求的档位，供 BeginWithPreviousZoom 恢复。
     */
    void Stop() noexcept;
    /** 每个主循环迭代调用：把适配器积累的日志写入插件日志。 */
    void Update() noexcept;

    /**
     * 换档：steps 为档数，正数放大；结果夹在当前允许范围内，在下一次绘制时按 anchor 取锚点应用
     * （Ctrl+滚轮为鼠标指着的地面点，HUD 按钮为战术区中心；鼠标手势或镜头平移动画进行中时推迟）。
     * 未 Begin 或缩放已停用时忽略。
     */
    void StepZoom(int steps, game_api::zoom::ZoomStepAnchor anchor) noexcept;
    /** 复原到 1×，在下一次绘制时以战术区中心为锚点应用。未 Begin 或缩放已停用时忽略。 */
    void ResetZoom() noexcept;
    /** 当前档位、允许范围、是否激活、停用原因与屏幕上那一帧的映射。 */
    game_api::zoom::BattlefieldZoomStatus GetStatus() const noexcept;

private:
    void FlushLog() noexcept;

    bool mIsActive = false;
    // 最近一次 Stop 前观看者请求的档位（rareplay::zoom::ZOOM_LEVELS 的下标）。
    int mRequestedIndexAtStop = rareplay::zoom::IDENTITY_ZOOM_INDEX;
};
}
