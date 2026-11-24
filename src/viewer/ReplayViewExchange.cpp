#include "ReplayViewExchange.h"

namespace rareplay::viewer
{
void ReplayViewExchange::Publish(std::shared_ptr<const ReplayViewSnapshot> snapshot) noexcept
{
    mSnapshot.store(std::move(snapshot));
}

std::shared_ptr<const ReplayViewSnapshot> ReplayViewExchange::Read() const noexcept
{
    return mSnapshot.load();
}

void ReplayViewExchange::Submit(ViewerCommand command)
{
    std::lock_guard lock(mCommandMutex);
    // 速度滑块只需要最后一个值；限制队列，避免暂停期间无限积压。
    if (mCommands.size() >= 32)
        mCommands.erase(mCommands.begin());
    mCommands.push_back(command);
}

std::vector<ViewerCommand> ReplayViewExchange::Drain()
{
    std::lock_guard lock(mCommandMutex);
    std::vector<ViewerCommand> commands;
    commands.swap(mCommands);
    return commands;
}

void ReplayViewExchange::PublishMarks(std::shared_ptr<const SuperWeaponMarkFrame> marks) noexcept
{
    mMarks.store(std::move(marks));
}

std::shared_ptr<const SuperWeaponMarkFrame> ReplayViewExchange::ReadMarks() const noexcept
{
    return mMarks.load();
}

void ReplayViewExchange::Clear()
{
    Publish(nullptr);
    PublishMarks(nullptr);
    WithdrawHudPresence();
    std::lock_guard lock(mCommandMutex);
    mCommands.clear();
}

void ReplayViewExchange::ReportHudPresented(std::uint64_t sessionId,
    std::chrono::steady_clock::time_point presentedAt, bool isPanelCovering) noexcept
{
    std::int64_t ticks = presentedAt.time_since_epoch().count();
    if (ticks == NO_PRESENCE)
        ++ticks;
    mPresenceSessionId.store(sessionId, std::memory_order_relaxed);
    mPresenceCovering.store(isPanelCovering, std::memory_order_relaxed);
    mPresenceTicks.store(ticks, std::memory_order_release);
}

void ReplayViewExchange::WithdrawHudPresence() noexcept
{
    mPresenceTicks.store(NO_PRESENCE, std::memory_order_release);
}

std::optional<HudPresence> ReplayViewExchange::ReadHudPresence() const noexcept
{
    const std::int64_t ticks = mPresenceTicks.load(std::memory_order_acquire);
    if (ticks == NO_PRESENCE)
        return std::nullopt;
    HudPresence presence;
    presence.mSessionId = mPresenceSessionId.load(std::memory_order_relaxed);
    presence.mIsPanelCovering = mPresenceCovering.load(std::memory_order_relaxed);
    presence.mPresentedAt = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ticks));
    return presence;
}

ReplayViewExchange& GetReplayViewExchange() noexcept
{
    static ReplayViewExchange exchange;
    return exchange;
}
}
