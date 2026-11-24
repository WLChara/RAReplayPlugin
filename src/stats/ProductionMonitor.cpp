#include "ProductionMonitor.h"

#include <algorithm>

namespace rareplay::stats
{
namespace
{
using viewer::ProductionLineState;

// 数值越小越能代表产线在工作。
int Priority(ProductionLineState state) noexcept
{
    switch (state)
    {
    case ProductionLineState::Producing: return 0;
    case ProductionLineState::NoFunds: return 1;
    case ProductionLineState::Paused: return 2;
    case ProductionLineState::PowerLoss: return 3;
    default: return 4;
    }
}
}

bool HasPowerOutage(int powerOutput, int powerDrain) noexcept
{
    return powerDrain > 0 && powerOutput < powerDrain;
}

std::int64_t ComputeTotalStalledMs(std::int64_t historyTotalMs, bool isStallActive, std::int64_t stallStartMs,
                                   std::int64_t nowMs) noexcept
{
    std::int64_t total = historyTotalMs;
    if (isStallActive)
    {
        const std::int64_t ongoing = std::max<std::int64_t>(nowMs - stallStartMs, 0);
        if (ongoing >= STALL_MIN_DURATION_MS)
            total += ongoing;
    }
    return total;
}

std::optional<float> ComputeStallRatePercent(std::int64_t totalStalledMs, std::int64_t gameTimeMs) noexcept
{
    if (gameTimeMs <= 0)
        return std::nullopt;
    const double percent = static_cast<double>(std::max<std::int64_t>(totalStalledMs, 0)) * 100.0 /
                           static_cast<double>(gameTimeMs);
    return static_cast<float>(std::clamp(percent, 0.0, 100.0));
}

ProductionLineState ClassifyFactory(const FactoryObservation& factory) noexcept
{
    if (factory.mProgress >= FACTORY_PROGRESS_DONE)
        return ProductionLineState::Producing;
    if (factory.mIsSuspended)
        return factory.mIsManual ? ProductionLineState::Paused : ProductionLineState::PowerLoss;
    if (factory.mIsOnHold)
        return ProductionLineState::NoFunds;
    return ProductionLineState::Producing;
}

ProductionLineState ClassifyLine(const LineObservation& line) noexcept
{
    if (line.mFactoryBuildings == 0)
        return ProductionLineState::NoFactory;
    if (line.mFactories.empty())
        return ProductionLineState::Stalled;
    ProductionLineState best = ClassifyFactory(line.mFactories.front());
    for (const FactoryObservation& factory : line.mFactories)
    {
        const ProductionLineState state = ClassifyFactory(factory);
        if (Priority(state) < Priority(best))
            best = state;
    }
    return best;
}

void ProductionLineTracker::Update(std::int64_t nowMs, const LineObservation& line, bool hasPowerOutage)
{
    const ProductionLineState state = ClassifyLine(line);
    if (state == ProductionLineState::Stalled)
    {
        if (!mIsStallActive)
        {
            mIsStallActive = true;
            mStallStartMs = nowMs;
            mStallHadPowerOutage = false;
        }
        mStallHadPowerOutage = mStallHadPowerOutage || hasPowerOutage;
        if (!mHasObserved || nowMs - mStallStartMs >= STALL_MIN_DURATION_MS)
            mConfirmedState = ProductionLineState::Stalled;
    }
    else
    {
        if (mIsStallActive)
        {
            if (nowMs - mStallStartMs >= STALL_MIN_DURATION_MS)
            {
                auto next = mHistory ? std::make_shared<viewer::StallHistory>(*mHistory)
                                     : std::make_shared<viewer::StallHistory>();
                next->push_back({mStallStartMs, nowMs, mStallHadPowerOutage});
                mHistory = std::move(next);
                mHistoryTotalMs += nowMs - mStallStartMs;
            }
            mIsStallActive = false;
        }
        mConfirmedState = state;
    }
    mHasObserved = true;
}

viewer::ProductionLineStatus ProductionLineTracker::GetStatus(std::int64_t gameTimeMs) const
{
    viewer::ProductionLineStatus status;
    status.mState = mConfirmedState;
    if (mConfirmedState == ProductionLineState::Stalled && mIsStallActive)
    {
        status.mStalledSinceMs = mStallStartMs;
        status.mStallHadPowerOutage = mStallHadPowerOutage;
    }
    status.mHistory = mHistory;
    // 进行中的停产以 mIsStallActive 为准：开局即停产时首次观测就是“已确认”状态，但总时间仍要等满 2 秒才计入。
    status.mTotalStalledMs = ComputeTotalStalledMs(mHistoryTotalMs, mIsStallActive, mStallStartMs, gameTimeMs);
    status.mStallRatePercent = ComputeStallRatePercent(status.mTotalStalledMs, gameTimeMs);
    return status;
}

void ProductionLineTracker::Reset() noexcept
{
    *this = ProductionLineTracker();
}
}
