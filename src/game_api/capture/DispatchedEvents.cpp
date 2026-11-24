#include "DispatchedEvents.h"

#include "../../stats/OperationEvents.h"
#include "../../target/TargetAbi.h"

#include <tuple>

namespace rareplay::game_api
{
// 原生事件长度以 TargetAbi 为准；录像格式（RecordedEvent）与操作计数（stats）因分层各自保留定义，在此核对三者相等。
static_assert(std::tuple_size_v<replay::RecordedEvent> == target::EVENT_BYTES);
static_assert(stats::NATIVE_EVENT_BYTES == target::EVENT_BYTES);

std::uint32_t ReadEventFrame(const replay::RecordedEvent& event) noexcept
{
    return static_cast<std::uint32_t>(event[EVENT_FRAME_OFFSET]) |
        (static_cast<std::uint32_t>(event[EVENT_FRAME_OFFSET + 1]) << 8) |
        (static_cast<std::uint32_t>(event[EVENT_FRAME_OFFSET + 2]) << 16) |
        (static_cast<std::uint32_t>(event[EVENT_FRAME_OFFSET + 3]) << 24);
}

bool IsFrameInfoEvent(const replay::RecordedEvent& event) noexcept
{
    return event[EVENT_TYPE_OFFSET] == EVENT_TYPE_FRAME_INFO;
}

EventFrameRelation ClassifyEventFrame(std::uint32_t eventFrame, std::uint32_t currentFrame) noexcept
{
    const auto eventValue = static_cast<std::int32_t>(eventFrame);
    const auto currentValue = static_cast<std::int32_t>(currentFrame);
    if (eventValue < currentValue)
        return EventFrameRelation::Stale;
    return eventValue == currentValue ? EventFrameRelation::Current : EventFrameRelation::Future;
}

bool TryPrepareDispatchedEvent(const replay::RecordedEvent& dispatched,
    std::uint32_t executionFrame, replay::RecordedEvent& outEvent) noexcept
{
    if (IsFrameInfoEvent(dispatched))
        return false;
    outEvent = dispatched;
    outEvent[EVENT_FRAME_OFFSET] = static_cast<std::uint8_t>(executionFrame);
    outEvent[EVENT_FRAME_OFFSET + 1] = static_cast<std::uint8_t>(executionFrame >> 8);
    outEvent[EVENT_FRAME_OFFSET + 2] = static_cast<std::uint8_t>(executionFrame >> 16);
    outEvent[EVENT_FRAME_OFFSET + 3] = static_cast<std::uint8_t>(executionFrame >> 24);
    return true;
}
}
