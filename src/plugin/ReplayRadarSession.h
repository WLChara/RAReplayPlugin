#pragma once

#include "../game_api/radar/NativeTacticalView.h"
#include "../game_api/radar/RadarEventResolver.h"
#include "../stats/InfiltrationObservation.h"
#include "../stats/RadarEventLog.h"
#include "../stats/SuperWeaponLaunch.h"
#include "../stats/SuperWeaponMarkTracker.h"
#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace rareplay::plugin
{
/**
 * 一次回放播放会话的雷达事件装配：把 Hook 观察点（超武释放前后、间谍渗透）交给纯逻辑判定，
 * 在帧完成点解析成事件并记入历史、生成超武落点图标，每个主循环迭代发布落点的屏幕位置。
 * 只在游戏线程使用；录制时不创建（回调立即返回）。它不创建任何游戏对象，只读游戏字段，
 * 唯一改变游戏状态的操作是“点击事件跳转镜头”，且只写本地镜头字段。
 * 任何异常只停用本功能（日志一次），不影响回放与界面其余部分。
 */
class ReplayRadarSession final
{
public:
    /** 回放开始时调用，之后观察点才工作。 */
    void Begin();
    void End() noexcept;

    /**
     * Hook 回调入口，参数是原生对象地址（可为空）。Begin/End 成对出现在 Fire_SW 调用 ClickFire 的前后；
     * 观察只复制整数，不解析名称与图标。
     */
    void OnSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
        const void* targetCell) noexcept;
    void OnSuperWeaponFireEnd(const void* super) noexcept;
    void OnBuildingInfiltrated(const void* spy, const void* building) noexcept;

    /**
     * 回放帧完成点：把本帧的待处理观察解析为事件并打上 gameTimeMs（该帧完成后的游戏时间）。
     * gameTimeMs 为空（统计已停用）时本帧的观察被丢弃。
     */
    void OnFrameCompleted(std::optional<std::int64_t> gameTimeMs) noexcept;

    /**
     * 每个主循环迭代调用：丢弃过期落点，把剩余落点投影到画布并发布到独立的落点通道。
     * 位置没有变化时不重复发布。gameTimeMs 为空时撤回落点。
     */
    void PublishMarks(std::uint64_t sessionId, std::optional<std::int64_t> gameTimeMs) noexcept;

    /**
     * 撤回已发布的落点（没有发布时什么也不做）。快进中不投影落点，用它清掉快进前发布的位置，
     * 以免战场停在原处（或兜底重绘）而旧位置仍留在 HUD 上；快进结束后的下一次 PublishMarks 重新发布。
     */
    void WithdrawMarks() noexcept;

    /** 把事件历史写入快照；未启用时不改动快照。 */
    void ApplyTo(viewer::ReplayViewSnapshot& snapshot) noexcept;

    /** 把本地镜头移到事件位置；事件不在历史中、没有位置或镜头不可用时返回 false。 */
    bool CenterOnEvent(std::uint64_t eventId) noexcept;

private:
    struct PendingEvent
    {
        bool mIsLaunch = true;
        stats::SuperWeaponLaunch mLaunch;
        stats::BuildingInfiltrationObservation mInfiltration;
    };

    void Disable(const char* reason) noexcept;

    bool mIsActive = false;
    stats::SuperWeaponFireTracker mTracker;
    std::vector<PendingEvent> mPending;
    game_api::RadarEventResolver mResolver;
    stats::RadarEventLog mLog;
    stats::SuperWeaponMarkTracker mMarks;
    std::shared_ptr<const viewer::SuperWeaponMarkFrame> mPublishedMarks;
    std::uint32_t mLaunchCount = 0;
    std::uint32_t mInfiltrationCount = 0;
    std::uint32_t mFilteredCount = 0;
    std::uint32_t mDroppedPendingCount = 0;
};
}
