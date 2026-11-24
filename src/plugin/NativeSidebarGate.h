#pragma once

#include <chrono>
#include <optional>

namespace rareplay::plugin
{
/** 面板心跳的最长有效期；超过即视为面板已不在画面上，交回原生侧栏。 */
constexpr std::chrono::milliseconds PANEL_PRESENCE_TIMEOUT{250};

/** SidebarClass::Draw 跳过点的处理结果。 */
enum class NativeSidebarAction
{
    /** 原生照常绘制。 */
    DrawNative,
    /** 从跳过切回原生：调用方须置原生重画标志，并让本次 Draw 按强制重画执行。 */
    RedrawNative,
    /** 跳过背景、按钮、标签动画与图标条；能量条、雷达与贴图照常。 */
    SkipChrome
};

/** 门控的全部输入；不含引擎对象，便于离线测试。 */
struct NativeSidebarGateInput
{
    /** 回放已开始、未请求退出、界面未失败，状态为播放、暂停或结束。 */
    bool mIsPlaybackActive = false;
    /** 渲染线程报告了本会话的面板，且未因 F10 或位置不确定而撤回。 */
    bool mIsHudVisible = false;
    std::optional<std::chrono::steady_clock::time_point> mPanelPresentedAt;
    std::chrono::steady_clock::time_point mNow;
    /** GameOptionsClass::SidebarMode；左侧布局的 SidebarBounds 不可用。 */
    bool mIsSidebarOnRight = false;
    /** Armageddon 模式下原生不画侧栏与雷达，且 ViewBounds 不同。 */
    bool mIsArmageddonMode = false;
};

/** 跳过原生侧栏会露出旧像素而非黑屏，所以只有面板确实在呈现时才允许跳过。 */
bool ShouldSkipNativeSidebarChrome(const NativeSidebarGateInput& input) noexcept;

/**
 * 记住上一帧是否跳过，在跳过结束的那一帧只请求一次重画。
 * 由播放会话持有，只在游戏线程的侧栏绘制中调用。
 */
class NativeSidebarGate final
{
public:
    NativeSidebarAction Update(const NativeSidebarGateInput& input) noexcept;
    bool IsSkipping() const noexcept { return mIsSkipping; }

private:
    bool mIsSkipping = false;
};
}
