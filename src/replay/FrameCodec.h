#pragma once

#include "ReplayPorts.h"

#include <array>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace rareplay::replay
{
class FrameCodec
{
public:
    static constexpr std::size_t HEADER_BYTES = 32;
    static constexpr std::uint32_t MAX_EVENTS_PER_FRAME = 16'384;
    static constexpr std::uint32_t MAX_INPUT_EFFECTS_PER_FRAME = 4'096;
    /** 首个可在每帧记录中携带游戏速度（flags bit2）的帧流版本。 */
    static constexpr std::uint32_t GAME_SPEED_STREAM_VERSION = 4;
    /**
     * 首个“单机帧流在录制时已中和飞机影子绘制的模拟副作用（R1）”的帧流版本。每帧记录的布局与 v4 完全相同，
     * 版本号只标记录制契约；更早版本的单机录像在有飞机的对局中可能无法精确重演。
     */
    static constexpr std::uint32_t AIRCRAFT_DRAW_NEUTRALIZED_STREAM_VERSION = 5;
    /** 写入端一律写、读取端接受的最高帧流版本；读取端接受 1..LATEST_STREAM_VERSION。 */
    static constexpr std::uint32_t LATEST_STREAM_VERSION = 5;
    /** 记录的游戏速度上限；与 GameOptionsClass::GameSpeed 的合法档位 0..6 一致。 */
    static constexpr std::uint32_t MAX_GAME_SPEED = 6;

    /** 帧流头 gameMode 为 0（Campaign）或 5（Skirmish）时按单机规则处理，其余值一律按联机规则。 */
    static constexpr bool IsOfflineGameMode(std::uint32_t gameMode) noexcept
    {
        return gameMode == 0 || gameMode == 5;
    }
    /** 帧流是否记录游戏速度：v4 起（含 v5）的单机帧流。录制端据此决定是否采样，读取端据此要求第 0 帧带速度。 */
    static constexpr bool CarriesGameSpeed(std::uint32_t streamVersion, std::uint32_t gameMode) noexcept
    {
        return streamVersion >= GAME_SPEED_STREAM_VERSION && IsOfflineGameMode(gameMode);
    }
    /**
     * 帧流录制时是否已中和飞机影子绘制的模拟副作用：v5 起的单机帧流。联机录制从不中和，与版本无关；
     * 回放端一律中和，因此为 false 的录像在含飞机的对局里可能无法精确重演。
     */
    static constexpr bool IsRecordedWithAircraftDrawNeutralized(std::uint32_t streamVersion,
        std::uint32_t gameMode) noexcept
    {
        return streamVersion >= AIRCRAFT_DRAW_NEUTRALIZED_STREAM_VERSION && IsOfflineGameMode(gameMode);
    }

    static bool EncodeHeader(const ReplaySessionInfo& info,
        std::array<std::uint8_t, HEADER_BYTES>& outBytes, std::string& error);
    static bool DecodeHeader(std::istream& stream, ReplaySessionInfo& outInfo, std::string& error);
    /** 编码前校验与版本有关的规则（含游戏速度只能出现在 v4 起且不超过 6）；失败时不改动 outBytes。 */
    static bool EncodeFrame(const ReplayFrame& frame, std::vector<std::uint8_t>& outBytes,
        std::string& error, std::uint32_t streamVersion = 2);
    /** 按版本解码一条记录并校验 flags 允许位与速度范围；失败时不改动 outFrame。 */
    static ReplayReadResult ReadFrame(std::istream& stream, ReplayFrame& outFrame,
        std::string& error, std::uint32_t streamVersion = 2);
    /**
     * 校验一帧的游戏速度字段与帧流头是否相容，规则：
     * v1–3 不得带速度；速度不超过 6；帧流头 gameMode 不是 0/5 时任何帧不得带速度；
     * v4 起（含 v5）单机帧流的第 0 帧必须带速度。完成文件逐帧解码、未完成录像扫描、播放会话与写入端共用此函数。
     */
    static bool ValidateGameSpeed(const ReplayFrame& frame, std::uint32_t streamVersion,
        std::uint32_t gameMode, std::string& error);
};
}
