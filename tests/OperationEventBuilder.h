#pragma once

#include "../src/stats/OperationEvents.h"

#include <array>
#include <cstdint>

// 测试用：按原生字节布局（Type +0、HouseIndex +2、载荷 +7）构造完整的 111 字节事件。
// garbage 指定载荷里“没被字段占用”的字节的填充值，用来验证合并键不受未定义字节影响。
namespace rareplay::tests
{
using NativeEvent = std::array<std::uint8_t, stats::NATIVE_EVENT_BYTES>;

class EventBuilder final
{
public:
    EventBuilder(stats::NativeEventType type, int houseIndex, std::uint8_t garbage = 0)
    {
        mEvent.fill(0);
        for (std::size_t index = stats::EVENT_PAYLOAD_OFFSET; index < mEvent.size(); ++index)
            mEvent[index] = garbage;
        mEvent[stats::EVENT_TYPE_OFFSET] = static_cast<std::uint8_t>(type);
        mEvent[stats::EVENT_HOUSE_INDEX_OFFSET] = static_cast<std::uint8_t>(static_cast<std::int8_t>(houseIndex));
    }

    EventBuilder& U8(std::size_t payloadOffset, std::uint8_t value)
    {
        mEvent[stats::EVENT_PAYLOAD_OFFSET + payloadOffset] = value;
        return *this;
    }
    EventBuilder& U16(std::size_t payloadOffset, std::uint16_t value)
    {
        U8(payloadOffset, static_cast<std::uint8_t>(value & 0xFF));
        return U8(payloadOffset + 1, static_cast<std::uint8_t>(value >> 8));
    }
    EventBuilder& U32(std::size_t payloadOffset, std::uint32_t value)
    {
        U16(payloadOffset, static_cast<std::uint16_t>(value & 0xFFFF));
        return U16(payloadOffset + 2, static_cast<std::uint16_t>(value >> 16));
    }
    /** TargetClass：4 字节 ID 加 1 字节 RTTI。 */
    EventBuilder& Target(std::size_t payloadOffset, std::uint32_t id, std::uint8_t rtti)
    {
        U32(payloadOffset, id);
        return U8(payloadOffset + 4, rtti);
    }
    NativeEvent Build() const { return mEvent; }

private:
    NativeEvent mEvent{};
};

inline NativeEvent MakeMegaMission(int house, std::uint32_t whom, std::uint8_t mission, std::uint32_t target,
                                   std::uint32_t destination = 0, std::uint32_t follow = 0, bool isPlanning = false,
                                   std::uint8_t garbage = 0, std::uint8_t gap = 0)
{
    using namespace stats::event_layout;
    return EventBuilder(stats::NativeEventType::MegaMission, house, garbage)
        .Target(MEGA_MISSION_WHOM, whom, 0x01)
        .U8(MEGA_MISSION_MISSION, mission)
        .U8(MEGA_MISSION_MISSION + 1, gap)
        .Target(MEGA_MISSION_TARGET, target, 0x0B)
        .Target(MEGA_MISSION_DESTINATION, destination, 0x0B)
        .Target(MEGA_MISSION_FOLLOW, follow, 0x00)
        .U8(MEGA_MISSION_PLANNING, isPlanning ? 1 : 0)
        .Build();
}

inline NativeEvent MakeMegaMissionF(int house, std::uint32_t whom, std::uint8_t mission, std::uint32_t target,
                                    std::uint32_t destination, std::uint32_t speed, std::uint32_t maxSpeed)
{
    using namespace stats::event_layout;
    return EventBuilder(stats::NativeEventType::MegaMissionF, house)
        .Target(MEGA_MISSION_F_WHOM, whom, 0x01)
        .U8(MEGA_MISSION_F_MISSION, mission)
        .Target(MEGA_MISSION_F_TARGET, target, 0x0B)
        .Target(MEGA_MISSION_F_DESTINATION, destination, 0x0B)
        .U32(MEGA_MISSION_F_SPEED, speed)
        .U32(MEGA_MISSION_F_MAX_SPEED, maxSpeed)
        .Build();
}

/** 载荷只有一个 TargetClass 的事件：Idle、Scatter、Deploy、Detonate、Sell、Repair、PowerOn、Primary 等。 */
inline NativeEvent MakeTargetEvent(stats::NativeEventType type, int house, std::uint32_t target,
                                   std::uint8_t garbage = 0)
{
    return EventBuilder(type, house, garbage).Target(0, target, 0x06).Build();
}

inline NativeEvent MakeProduce(stats::NativeEventType type, int house, std::uint32_t rtti, std::uint32_t heapId,
                               std::uint32_t isNaval, std::uint8_t garbage = 0)
{
    using namespace stats::event_layout;
    return EventBuilder(type, house, garbage)
        .U32(PRODUCTION_RTTI, rtti)
        .U32(PRODUCTION_HEAP_ID, heapId)
        .U32(PRODUCTION_IS_NAVAL, isNaval)
        .Build();
}

inline NativeEvent MakePlace(int house, std::uint32_t heapId, std::uint16_t x, std::uint16_t y)
{
    using namespace stats::event_layout;
    return EventBuilder(stats::NativeEventType::Place, house)
        .U32(PLACE_RTTI, 7)
        .U32(PLACE_HEAP_ID, heapId)
        .U32(PLACE_IS_NAVAL, 0)
        .U16(PLACE_LOCATION, x)
        .U16(PLACE_LOCATION + 2, y)
        .Build();
}

/** 没有有意义载荷的事件（也用于系统事件）。 */
inline NativeEvent MakeSimpleEvent(stats::NativeEventType type, int house, std::uint8_t garbage = 0)
{
    return EventBuilder(type, house, garbage).Build();
}
}
