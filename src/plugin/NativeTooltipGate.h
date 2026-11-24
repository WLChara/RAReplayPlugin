#pragma once

#include <optional>

namespace rareplay::plugin
{
/**
 * 回放播放中且 HUD 可见时关闭原生提示，隐藏 HUD 或回放结束时恢复原值。
 *
 * 原生提示由消息泵里的 GetCursorPos 驱动，插件窗口过程拦不到，会在底栏上方的地图区弹出；
 * 0x48CE5D、0x52DB04 与选项应用 0x4E1FA9 还会再次调用 SetState，所以每帧都要检查并重新关闭。
 * 关闭期间观察到的“已启用”只可能来自这些外部调用，视为新的原值，恢复时以它为准。
 * 由播放会话持有，只在游戏线程调用；不含引擎对象，便于离线测试。
 */
class NativeTooltipGate final
{
public:
    /**
     * shouldSuppress：回放播放中且 HUD 可见；currentIsActive：原生提示当前状态。
     * 返回需要写入的新状态；为空表示本帧不调用 SetState。
     */
    std::optional<bool> Update(bool shouldSuppress, bool currentIsActive) noexcept;
    bool IsSuppressing() const noexcept { return mIsSuppressing; }

private:
    bool mIsSuppressing = false;
    bool mSavedIsActive = false;
};
}
