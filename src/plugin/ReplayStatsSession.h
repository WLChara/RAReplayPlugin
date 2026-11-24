#pragma once

#include "../replay/ReplayPorts.h"
#include "../stats/MatchStatistics.h"
#include "../stats/StatsObservation.h"
#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <optional>

namespace rareplay::plugin
{
/**
 * 一次回放播放会话的统计采样与发布装配，只在游戏线程使用；录制时不创建。
 * 把 game_api 的只读观测交给 stats::MatchStatistics，并作为回放执行器的帧观察者统计操作事件。
 * 任何异常只停用统计（日志一次），不影响回放与界面。
 */
class ReplayStatsSession final : public replay::IReplayFrameObserver
{
public:
    void Begin();
    void End() noexcept;

    /**
     * 回放帧完成点（CompleteFrame，状态为播放中）调用一次：frameNumber 为刚完成的帧号。
     * 暂停时帧号不推进，重复调用同一帧号不会重复累加。游戏时间每跨过一个 60 秒边界，经 Diagnostics 写一行统计摘要
     * （StatsDigest），快进中照常写。
     */
    void OnFrameCompleted(std::uint32_t frameNumber) noexcept;

    void OnPlaybackFrameExecuted(const replay::ReplayFrame& frame) noexcept override;

    /**
     * 回放当前的游戏时间（毫秒，已累加到最近一次完成的帧）；统计已停用或尚未登记过任何帧时为空。
     * 雷达事件与落点图标用它作时间轴，所以它们与统计同生同灭。
     */
    std::optional<std::int64_t> GetGameTimeMs() const noexcept;

    /** 把统计写入快照；统计已停用或尚无数据时不改动快照。 */
    void ApplyTo(viewer::ReplayViewSnapshot& snapshot) noexcept;

private:
    void Disable(const char* reason) noexcept;

    stats::MatchStatistics mStats;
    stats::FrameObservation mFrame;
    stats::ProductionObservation mProduction;
    bool mIsActive = false;
    bool mHasLoggedSkippedHouses = false;
};
}
