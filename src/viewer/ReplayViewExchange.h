#pragma once

#include "ReplayViewModel.h"
#include "SuperWeaponMarks.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace rareplay::viewer
{
/** 渲染线程最近一次实际呈现 HUD 的心跳。 */
struct HudPresence
{
    std::uint64_t mSessionId = 0;
    std::chrono::steady_clock::time_point mPresentedAt;
    /** 本次呈现中侧栏面板位置唯一且完整覆盖原生侧栏区；只有为真时游戏才可停止原生侧栏绘制。 */
    bool mIsPanelCovering = false;
};

/** 游戏线程与渲染线程仅交换不可变快照、有界命令和 HUD 呈现心跳；渲染线程不得访问 YRpp。 */
class ReplayViewExchange final
{
public:
    void Publish(std::shared_ptr<const ReplayViewSnapshot> snapshot) noexcept;
    std::shared_ptr<const ReplayViewSnapshot> Read() const noexcept;
    void Submit(ViewerCommand command);
    std::vector<ViewerCommand> Drain();
    /** 清空快照、命令、落点并撤回 HUD 呈现；会话开始与结束时由游戏线程调用。 */
    void Clear();

    /**
     * 游戏线程在每个主循环迭代发布超武落点的屏幕位置；空指针表示当前没有落点。
     * 与约 100 ms 才发布一次的快照分开，是为了让图标跟上镜头滚动（见 SuperWeaponMarkFrame）。
     * 每次发布整体替换，渲染线程读到的永远是同一迭代内一致的一组位置。
     */
    void PublishMarks(std::shared_ptr<const SuperWeaponMarkFrame> marks) noexcept;
    std::shared_ptr<const SuperWeaponMarkFrame> ReadMarks() const noexcept;

    /**
     * 渲染线程在本帧 HUD 绘制数据已提交后调用。游戏线程据此停止原生侧栏绘制（需 isPanelCovering）
     * 并关闭原生提示，因此只能在真正呈现后报告。
     */
    void ReportHudPresented(std::uint64_t sessionId, std::chrono::steady_clock::time_point presentedAt,
                            bool isPanelCovering) noexcept;
    /** 渲染线程在 F10 隐藏时调用，立即撤回呈现状态。 */
    void WithdrawHudPresence() noexcept;
    /** 任一线程可读；未呈现或已撤回时为空。各字段分别原子读取，跨会话切换的瞬间可能配对到上一帧的值。 */
    std::optional<HudPresence> ReadHudPresence() const noexcept;

private:
    static constexpr std::int64_t NO_PRESENCE = INT64_MIN;

    std::atomic<std::shared_ptr<const ReplayViewSnapshot>> mSnapshot;
    std::atomic<std::shared_ptr<const SuperWeaponMarkFrame>> mMarks;
    std::mutex mCommandMutex;
    std::vector<ViewerCommand> mCommands;
    // 先写会话与覆盖标志，再以 release 写时刻；读方以 acquire 读时刻后再读其他字段。
    std::atomic<std::uint64_t> mPresenceSessionId{0};
    std::atomic<bool> mPresenceCovering{false};
    std::atomic<std::int64_t> mPresenceTicks{NO_PRESENCE};
};

ReplayViewExchange& GetReplayViewExchange() noexcept;
}
