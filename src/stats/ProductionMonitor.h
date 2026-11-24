#pragma once

#include "StatsObservation.h"
#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace rareplay::stats
{
/** 短于它的停产不记入历史，也不会让产线状态变成“停产”（AI 每件产品完成后有几帧空档）。 */
constexpr std::int64_t STALL_MIN_DURATION_MS = 2000;
/** FactoryClass::Production.Value 的满格，已完成待出厂（0x4C9B20）。 */
constexpr int FACTORY_PROGRESS_DONE = 54;

/** 玩家是否处于电力不足：PowerDrain > 0 且 PowerOutput < PowerDrain，与 GetPowerPercentage（0x4FCE30）一致。 */
bool HasPowerOutage(int powerOutput, int powerDrain) noexcept;

/**
 * 单座在制工厂的状态：
 * Value ≥ 54 为已完成待出厂，算生产中；IsSuspended 且手动为暂停；IsSuspended 且非手动为无可用通电工厂而自动挂起；
 * 未挂起且 OnHold 为资金不足；其余为生产中。
 */
viewer::ProductionLineState ClassifyFactory(const FactoryObservation& factory) noexcept;

/**
 * 一条产线的状态：没有工厂建筑为 NoFactory；有工厂但没有在制工厂为 Stalled；
 * 多个在制工厂时取最能代表“在工作”的一个（生产中 > 缺钱 > 暂停 > 断电）。
 */
viewer::ProductionLineState ClassifyLine(const LineObservation& line) noexcept;

/**
 * 总停产时间（毫秒）：已写入历史的停产时长之和，加上正在进行的停产。进行中的停产只有持续不短于
 * STALL_MIN_DURATION_MS 才计入（计入整段时长），与“结束时不足阈值不写历史”的口径一致。
 * isStallActive 为假时忽略 stallStartMs；nowMs 早于 stallStartMs 时进行中的部分按 0。
 */
std::int64_t ComputeTotalStalledMs(std::int64_t historyTotalMs, bool isStallActive, std::int64_t stallStartMs,
                                   std::int64_t nowMs) noexcept;

/**
 * 停产率（百分数，夹在 0–100）：总停产时间 ÷ 游戏时间 × 100。游戏时间不是正数时为空。
 * 分母是整段游戏时间，不扣除尚无工厂的时间。
 */
std::optional<float> ComputeStallRatePercent(std::int64_t totalStalledMs, std::int64_t gameTimeMs) noexcept;

/**
 * 单条产线（某玩家的兵营或重工）的状态机与停产历史。只在游戏线程使用。
 *
 * - 时间是游戏时间毫秒，精度为采样间隔；停产的起点取首个观测到停产的采样时刻。
 * - 停产结束时若持续不短于 STALL_MIN_DURATION_MS 才写入历史，并带上期间是否出现过电力不足。
 * - 对外状态是“确认状态”：停产持续不足阈值时仍报告上一个确认状态，避免 AI 换件空档造成闪烁；
 *   这是第一次观测时直接采用当前状态的例外（开局就没有生产也是真实的停产）。
 */
class ProductionLineTracker final
{
public:
    void Update(std::int64_t nowMs, const LineObservation& line, bool hasPowerOutage);
    /**
     * 当前状态与停产汇总。gameTimeMs 是发布时刻的游戏时间（不是最近一次采样时刻），
     * 用于计算进行中的停产时长与停产率；产线状态每 250 ms 才采样一次，所以它可能略晚于采样时刻。
     */
    viewer::ProductionLineStatus GetStatus(std::int64_t gameTimeMs) const;
    void Reset() noexcept;

private:
    bool mHasObserved = false;
    viewer::ProductionLineState mConfirmedState = viewer::ProductionLineState::NoFactory;
    bool mIsStallActive = false;
    std::int64_t mStallStartMs = 0;
    bool mStallHadPowerOutage = false;
    // 每次追加都复制出新对象，已发布的快照继续共享旧对象，因此发布后永不修改。
    std::shared_ptr<const viewer::StallHistory> mHistory;
    // 历史中全部区间时长之和，随追加累加，避免每次发布都遍历历史。
    std::int64_t mHistoryTotalMs = 0;
};
}
