#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "../viewer/ReplayViewModel.h"
#include "../game_api/ReplayWorldReader.h"
#include "BattlefieldZoomSession.h"
#include "ReplayRadarSession.h"
#include "ReplayStatsSession.h"

namespace rareplay::plugin
{
/** 游戏线程拥有采样节奏与会话标识；图形线程只接收复制后的值。 */
class ReplayViewPublisher final
{
public:
    void Begin(std::string replayName, int recordedLocalHouseIndex);
    /**
     * 统计、雷达事件与战局缩放由调用方（观看会话 PlaybackViewingSession）持有，统计与雷达在帧完成点采样；
     * 这里只在发布时把它们写入快照，缩放状态按发布时刻读取并换算（BuildBattlefieldZoomView）。
     * totalFrames 与 timeline 来自时间表扫描（未就绪时为空），原样写入快照的 mTotalFrames 与 mTimeline；
     * 快进状态与能否跳转随 controls 写入。
     */
    void Publish(std::optional<int> playbackSpeedSetting, bool canChangeSpeed,
        const viewer::PlaybackControls& controls,
        const std::optional<replay::RecordedControlGroupSnapshot>& recordedGroups,
        std::optional<std::uint32_t> totalFrames, std::shared_ptr<const viewer::GameTimeline> timeline,
        ReplayStatsSession& stats, ReplayRadarSession& radar, const BattlefieldZoomSession& battlefieldZoom);
    void End();
    std::uint64_t GetSessionId() const noexcept { return mSessionId; }
    /** 设置写入之后发布的快照；变化时下一次 Publish 立即采样，不等 100 ms 节流。 */
    void SetHudScale(viewer::HudScaleSetting setting) noexcept;
    /** 当前 HUD 档位的唯一持有者；Begin/End 不重置，跨会话保留最近一次设置。 */
    viewer::HudScaleSetting GetHudScale() const noexcept { return mHudScale; }

private:
    std::uint64_t mSessionId = 0;
    viewer::HudScaleSetting mHudScale = viewer::HudScaleSetting::Percent100;
    std::string mReplayName;
    int mRecordedLocalHouseIndex = -1;
    std::chrono::steady_clock::time_point mNextSample;
    bool mHasLoggedLayout = false;
    game_api::ReplayWorldReader mWorldReader;
    std::uint32_t mIconLogCount = 0;
};
}
