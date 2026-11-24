#pragma once

#include "../../replay/ReplayTypes.h"

#include <cstddef>
#include <cstdint>

namespace rareplay::game_api
{
// 单机分派点采集规则用到的 EventClass 字节布局；GameApi.cpp 以 YRpp 声明 static_assert 核对。
constexpr std::size_t EVENT_TYPE_OFFSET = 0;
constexpr std::size_t EVENT_FRAME_OFFSET = 3;
// EventType::FrameInfo：联机逐帧 CRC 同步记录，不是玩家命令，单机分派采集时不写入录像。
constexpr std::uint8_t EVENT_TYPE_FRAME_INFO = 0x1C;

/** 事件帧戳相对执行帧的关系；与 handleAny 0x64C565/0x64C57B 的有符号比较一致。 */
enum class EventFrameRelation
{
    Stale,
    Current,
    Future
};

/** 读取事件字节中的 Frame（小端 uint32）。 */
std::uint32_t ReadEventFrame(const replay::RecordedEvent& event) noexcept;
bool IsFrameInfoEvent(const replay::RecordedEvent& event) noexcept;
EventFrameRelation ClassifyEventFrame(std::uint32_t eventFrame, std::uint32_t currentFrame) noexcept;

/**
 * 单机分派点的录像规则：FrameInfo 不写入录像；其余事件复制一份，并把副本的 Frame 改写为实际执行帧，
 * 其他字节保持原样。回放在执行帧开头注入副本，由原生 handleAny 按 Frame==当前帧 执行。
 *
 * @return 应写入录像时返回 true 并写 outEvent；FrameInfo 返回 false，outEvent 不变。
 */
bool TryPrepareDispatchedEvent(const replay::RecordedEvent& dispatched,
    std::uint32_t executionFrame, replay::RecordedEvent& outEvent) noexcept;
}
