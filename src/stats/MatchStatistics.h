#pragma once

#include "EconomyHistory.h"
#include "GameClock.h"
#include "OperationEvents.h"
#include "ProductionMonitor.h"
#include "StatsObservation.h"
#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace rareplay::stats
{
/** 产线状态每累计这么多游戏时间采样一次，使停产起止的时间精度优于 0.5 秒（还要加上一帧的时长）。 */
constexpr std::int64_t PRODUCTION_SAMPLE_INTERVAL_MS = 250;

/**
 * 一次回放播放会话的统计状态唯一所有者，只含值与纯逻辑，不访问引擎；只在游戏线程使用。
 *
 * 数据流：游戏适配层在每个完成帧调用 OnFrameCompleted；它在 IsProductionSampleDue 为真时再读取较重的产线观测
 * 并调用 OnProductionSampled；回放执行器每注入一条事件调用 OnOperationEvent；发布快照时调用 ApplyTo。
 * 录制时不创建也不使用本对象。
 */
class MatchStatistics final
{
public:
    void Reset();

    /**
     * 登记已完成的一帧：累加游戏时间、更新升级时间、按 1 秒游戏时间采样资金，并记下最新的击杀与产出计数。
     * 同一帧号重复登记（暂停时反复进入帧完成点）不会再次累加或采样。
     */
    void OnFrameCompleted(std::uint32_t frameNumber, const FrameObservation& frame);

    /** 距上次产线采样已累计 PRODUCTION_SAMPLE_INTERVAL_MS 游戏时间，或尚未采样过。 */
    bool IsProductionSampleDue() const noexcept;
    void OnProductionSampled(const ProductionObservation& observation);

    /**
     * 回放执行器在 frameNumber 帧注入的一条完整原生事件（NATIVE_EVENT_BYTES 字节）；按类型与 HouseIndex 计入操作。
     * 同一帧、同一玩家、同类型且除执行者外参数相同的事件合并为一次操作（见 OperationCounter）；
     * 同一帧的事件必须连续登记。
     */
    void OnOperationEvent(std::uint32_t frameNumber, std::span<const std::uint8_t> event);

    /**
     * 把统计写入快照：mGameTime，以及 mPlayers 中每个已有观测的玩家的统计、升级、产线状态和资金曲线。
     * 玩家按 mHouseIndex 匹配；没有数据的玩家保持字段缺失。重复调用不重复累加，曲线在无新样本时共享同一对象。
     */
    void ApplyTo(viewer::ReplayViewSnapshot& snapshot);

    const GameClock& GetClock() const noexcept { return mClock; }
    /** 最近一次 OnFrameCompleted 登记的房屋观测（资金、击杀矩阵、产出计数与分数）；只读，供诊断摘要使用。 */
    const std::vector<HouseObservation>& GetLatestHouses() const noexcept { return mLatestHouses; }
    /** 最近一次计算中因房屋下标超出击杀矩阵而被跳过的房屋数；供诊断日志使用。 */
    std::uint32_t GetSkippedHouseCount() const noexcept { return mSkippedHouses; }

private:
    /** 兵营或重工升级：标志的当前值，以及首次由 0 变 1 的游戏时间。 */
    struct UpgradeTracker
    {
        bool mIsActive = false;
        bool mHasActivated = false;
        std::int64_t mFirstActivatedAtMs = 0;

        void Update(bool isFlagSet, std::int64_t nowMs) noexcept;
        viewer::UpgradeStatus ToStatus() const noexcept { return {mIsActive, mFirstActivatedAtMs}; }
    };

    struct PlayerTrack
    {
        EconomyHistory mEconomy;
        ProductionLineTracker mBarracks;
        ProductionLineTracker mWarFactory;
        UpgradeTracker mBarracksUpgrade;
        UpgradeTracker mWarFactoryUpgrade;
        bool mHasPowerOutage = false;
    };

    PlayerTrack& GetTrack(int houseIndex);

    GameClock mClock;
    OperationCounter mOperations;
    std::map<int, PlayerTrack> mTracks;
    std::vector<HouseObservation> mLatestHouses;
    std::optional<std::int64_t> mNextEconomySampleMs;
    std::optional<std::int64_t> mLastProductionSampleMs;
    std::uint32_t mSkippedHouses = 0;
};
}
