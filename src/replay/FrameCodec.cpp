#include "FrameCodec.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>

namespace rareplay::replay
{
namespace
{
constexpr std::uint32_t STREAM_MAGIC = 0x32465052;
// 帧流中每条事件的字节数取自 RecordedEvent，录像格式不另设一份长度。
constexpr std::uint32_t EVENT_BYTES = static_cast<std::uint32_t>(std::tuple_size_v<RecordedEvent>);
constexpr std::uint32_t CHECKSUM_PRESENT = 1;
constexpr std::uint32_t GROUP_SNAPSHOT_PRESENT = 2;
// v4 起：记录头之后紧跟 u32 游戏速度，再接事件、输入效果与编队成员。
constexpr std::uint32_t GAME_SPEED_PRESENT = 4;
// v5 的每帧记录与 v4 相同；各处布局与速度规则一律按 >= 判断，版本上限统一取 LATEST_STREAM_VERSION。
constexpr std::uint32_t MAX_STREAM_VERSION = FrameCodec::LATEST_STREAM_VERSION;
constexpr std::size_t FRAME_HEADER_BYTES = 16;
constexpr std::size_t EFFECT_COUNT_BYTES = 4;
constexpr std::size_t EFFECT_BYTES = 40;
constexpr std::size_t GROUP_COUNT_BYTES = 4;
constexpr std::size_t GROUP_MEMBER_BYTES = 12;
constexpr std::size_t GAME_SPEED_BYTES = 4;

/** 各版本 flags 允许的位：v1/v2 只有 bit0，v3 加 bit1，v4 起（含 v5）再加 bit2。 */
constexpr std::uint32_t AllowedFrameFlags(std::uint32_t streamVersion)
{
    return CHECKSUM_PRESENT | (streamVersion >= 3 ? GROUP_SNAPSHOT_PRESENT : 0) |
        (streamVersion >= FrameCodec::GAME_SPEED_STREAM_VERSION ? GAME_SPEED_PRESENT : 0);
}

void StoreU32(std::uint8_t* destination, std::uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
    {
        destination[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::uint32_t LoadU32(const std::uint8_t* source)
{
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index)
    {
        value |= static_cast<std::uint32_t>(source[index]) << (index * 8);
    }
    return value;
}
}

bool FrameCodec::EncodeHeader(const ReplaySessionInfo& info,
    std::array<std::uint8_t, HEADER_BYTES>& outBytes, std::string& error)
{
    if (info.mStreamVersion < 2 || info.mStreamVersion > MAX_STREAM_VERSION)
    {
        error = "不支持写入此帧流版本";
        return false;
    }
    if (info.mFirstFrame != 0)
    {
        error = "First replay frame must be zero";
        return false;
    }
    outBytes.fill(0);
    StoreU32(outBytes.data(), STREAM_MAGIC);
    StoreU32(outBytes.data() + 4, info.mStreamVersion);
    StoreU32(outBytes.data() + 8, EVENT_BYTES);
    StoreU32(outBytes.data() + 12, info.mGameMode);
    StoreU32(outBytes.data() + 16, info.mLocalHouseIndex);
    StoreU32(outBytes.data() + 20, info.mFirstFrame);
    StoreU32(outBytes.data() + 24, info.mFrameSendRate);
    StoreU32(outBytes.data() + 28, info.mRandomSeed);
    error.clear();
    return true;
}

bool FrameCodec::DecodeHeader(std::istream& stream, ReplaySessionInfo& outInfo, std::string& error)
{
    std::array<std::uint8_t, HEADER_BYTES> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
    {
        error = "Truncated replay stream header";
        return false;
    }
    const std::uint32_t version = LoadU32(bytes.data() + 4);
    if (LoadU32(bytes.data()) != STREAM_MAGIC || (version < 1 || version > MAX_STREAM_VERSION) ||
        LoadU32(bytes.data() + 8) != EVENT_BYTES || LoadU32(bytes.data() + 20) != 0)
    {
        error = "Invalid or unsupported replay stream header";
        return false;
    }
    outInfo.mGameMode = LoadU32(bytes.data() + 12);
    outInfo.mLocalHouseIndex = LoadU32(bytes.data() + 16);
    outInfo.mFirstFrame = 0;
    outInfo.mFrameSendRate = LoadU32(bytes.data() + 24);
    outInfo.mRandomSeed = LoadU32(bytes.data() + 28);
    outInfo.mStreamVersion = version;
    error.clear();
    return true;
}

bool FrameCodec::EncodeFrame(const ReplayFrame& frame, std::vector<std::uint8_t>& outBytes,
    std::string& error, std::uint32_t streamVersion)
{
    if (streamVersion < 2 || streamVersion > MAX_STREAM_VERSION)
    {
        error = "不支持写入此帧流版本";
        return false;
    }
    if (!ValidateRecordedControlGroups(frame.mRecordedGroups, frame.mFrameNumber,
        streamVersion, error))
    {
        return false;
    }
    if (frame.mGameSpeed && streamVersion < GAME_SPEED_STREAM_VERSION)
    {
        error = "游戏速度只能写入帧流版本 4 及以上";
        return false;
    }
    if (frame.mGameSpeed && *frame.mGameSpeed > MAX_GAME_SPEED)
    {
        error = "游戏速度超出 0..6：" + std::to_string(*frame.mGameSpeed);
        return false;
    }
    if (frame.mEvents.size() > MAX_EVENTS_PER_FRAME)
    {
        error = "Replay frame exceeds the event limit";
        return false;
    }
    if (frame.mInputEffects.size() > MAX_INPUT_EFFECTS_PER_FRAME)
    {
        error = "Replay frame exceeds the input effect limit";
        return false;
    }
    for (const RecordedInputEffect& effect : frame.mInputEffects)
    {
        if (effect.mKind != PlayerInputEffectKind::MovementFeedback)
        {
            error = "Replay input effect kind is invalid";
            return false;
        }
    }
    const std::size_t recordHeaderBytes = FRAME_HEADER_BYTES + EFFECT_COUNT_BYTES +
        (streamVersion >= 3 ? GROUP_COUNT_BYTES : 0);
    const std::size_t eventsOffset = recordHeaderBytes + (frame.mGameSpeed ? GAME_SPEED_BYTES : 0);
    const std::size_t effectsOffset = eventsOffset + frame.mEvents.size() * EVENT_BYTES;
    const std::size_t groupsOffset = effectsOffset + frame.mInputEffects.size() * EFFECT_BYTES;
    const std::size_t groupCount = frame.mRecordedGroups ?
        frame.mRecordedGroups->mMembers.size() : 0;
    outBytes.resize(groupsOffset + groupCount * GROUP_MEMBER_BYTES);
    StoreU32(outBytes.data(), frame.mFrameNumber);
    StoreU32(outBytes.data() + 4, frame.mChecksum);
    StoreU32(outBytes.data() + 8, static_cast<std::uint32_t>(frame.mEvents.size()));
    StoreU32(outBytes.data() + 12, (frame.mHasChecksum ? CHECKSUM_PRESENT : 0) |
        (frame.mRecordedGroups ? GROUP_SNAPSHOT_PRESENT : 0) |
        (frame.mGameSpeed ? GAME_SPEED_PRESENT : 0));
    StoreU32(outBytes.data() + FRAME_HEADER_BYTES,
        static_cast<std::uint32_t>(frame.mInputEffects.size()));
    if (streamVersion >= 3)
    {
        StoreU32(outBytes.data() + FRAME_HEADER_BYTES + EFFECT_COUNT_BYTES,
            static_cast<std::uint32_t>(groupCount));
    }
    if (frame.mGameSpeed)
    {
        StoreU32(outBytes.data() + recordHeaderBytes, *frame.mGameSpeed);
    }
    for (std::size_t index = 0; index < frame.mEvents.size(); ++index)
    {
        std::memcpy(outBytes.data() + eventsOffset + index * EVENT_BYTES,
            frame.mEvents[index].data(), EVENT_BYTES);
    }
    for (std::size_t index = 0; index < frame.mInputEffects.size(); ++index)
    {
        const RecordedInputEffect& effect = frame.mInputEffects[index];
        std::uint8_t* destination = outBytes.data() + effectsOffset + index * EFFECT_BYTES;
        StoreU32(destination, static_cast<std::uint32_t>(effect.mKind));
        StoreU32(destination + 4, effect.mAnimationTypeIndex);
        StoreU32(destination + 8, static_cast<std::uint32_t>(effect.mX));
        StoreU32(destination + 12, static_cast<std::uint32_t>(effect.mY));
        StoreU32(destination + 16, static_cast<std::uint32_t>(effect.mZ));
        StoreU32(destination + 20, static_cast<std::uint32_t>(effect.mLoopDelay));
        StoreU32(destination + 24, static_cast<std::uint32_t>(effect.mLoopCount));
        StoreU32(destination + 28, effect.mFlags);
        StoreU32(destination + 32, static_cast<std::uint32_t>(effect.mForceZAdjust));
        StoreU32(destination + 36, effect.mReverse ? 1 : 0);
    }
    for (std::size_t index = 0; index < groupCount; ++index)
    {
        const RecordedGroupMember& member = frame.mRecordedGroups->mMembers[index];
        std::uint8_t* destination = outBytes.data() + groupsOffset + index * GROUP_MEMBER_BYTES;
        StoreU32(destination, member.mGroupIndex);
        StoreU32(destination + 4, member.mObjectId);
        StoreU32(destination + 8, member.mTargetKind);
    }
    error.clear();
    return true;
}

ReplayReadResult FrameCodec::ReadFrame(std::istream& stream, ReplayFrame& outFrame,
    std::string& error, std::uint32_t streamVersion)
{
    if (streamVersion < 1 || streamVersion > MAX_STREAM_VERSION)
    {
        error = "Unsupported replay stream version";
        return ReplayReadResult::Error;
    }
    std::array<std::uint8_t, FRAME_HEADER_BYTES + EFFECT_COUNT_BYTES + GROUP_COUNT_BYTES> bytes{};
    const std::size_t headerBytes = FRAME_HEADER_BYTES +
        (streamVersion >= 2 ? EFFECT_COUNT_BYTES : 0) +
        (streamVersion >= 3 ? GROUP_COUNT_BYTES : 0);
    stream.read(reinterpret_cast<char*>(bytes.data()), headerBytes);
    if (stream.gcount() == 0 && stream.eof())
    {
        error.clear();
        return ReplayReadResult::End;
    }
    if (stream.gcount() != static_cast<std::streamsize>(headerBytes))
    {
        error = "Truncated replay frame header";
        return ReplayReadResult::Error;
    }
    const std::uint32_t eventCount = LoadU32(bytes.data() + 8);
    const std::uint32_t flags = LoadU32(bytes.data() + 12);
    if (eventCount > MAX_EVENTS_PER_FRAME || (flags & ~AllowedFrameFlags(streamVersion)) != 0)
    {
        error = "Replay frame count or flags are invalid";
        return ReplayReadResult::Error;
    }
    const std::uint32_t effectCount = streamVersion >= 2 ?
        LoadU32(bytes.data() + FRAME_HEADER_BYTES) : 0;
    const std::uint32_t groupCount = streamVersion >= 3 ?
        LoadU32(bytes.data() + FRAME_HEADER_BYTES + EFFECT_COUNT_BYTES) : 0;
    if (effectCount > MAX_INPUT_EFFECTS_PER_FRAME)
    {
        error = "Replay frame exceeds the input effect limit";
        return ReplayReadResult::Error;
    }
    if (groupCount > MAX_RECORDED_GROUP_MEMBERS ||
        (groupCount != 0 && (flags & GROUP_SNAPSHOT_PRESENT) == 0))
    {
        error = "编队成员数量或快照标志无效";
        return ReplayReadResult::Error;
    }
    ReplayFrame frame;
    frame.mFrameNumber = LoadU32(bytes.data());
    frame.mChecksum = LoadU32(bytes.data() + 4);
    frame.mHasChecksum = (flags & CHECKSUM_PRESENT) != 0;
    if ((flags & GROUP_SNAPSHOT_PRESENT) != 0)
    {
        frame.mRecordedGroups.emplace();
    }
    // 在按外部数量分配载荷前验证首帧和版本约束。
    if (!ValidateRecordedControlGroups(frame.mRecordedGroups, frame.mFrameNumber,
        streamVersion, error))
    {
        return ReplayReadResult::Error;
    }
    if ((flags & GAME_SPEED_PRESENT) != 0)
    {
        std::array<std::uint8_t, GAME_SPEED_BYTES> speedBytes{};
        stream.read(reinterpret_cast<char*>(speedBytes.data()), speedBytes.size());
        if (stream.gcount() != static_cast<std::streamsize>(speedBytes.size()))
        {
            error = "游戏速度字段被截断";
            return ReplayReadResult::Error;
        }
        const std::uint32_t gameSpeed = LoadU32(speedBytes.data());
        if (gameSpeed > MAX_GAME_SPEED)
        {
            error = "游戏速度超出 0..6：" + std::to_string(gameSpeed);
            return ReplayReadResult::Error;
        }
        frame.mGameSpeed = gameSpeed;
    }
    frame.mEvents.resize(eventCount);
    for (auto& event : frame.mEvents)
    {
        stream.read(reinterpret_cast<char*>(event.data()), event.size());
        if (stream.gcount() != static_cast<std::streamsize>(event.size()))
        {
            error = "Truncated replay event";
            return ReplayReadResult::Error;
        }
    }
    frame.mInputEffects.reserve(effectCount);
    for (std::uint32_t index = 0; index < effectCount; ++index)
    {
        std::array<std::uint8_t, EFFECT_BYTES> effectBytes{};
        stream.read(reinterpret_cast<char*>(effectBytes.data()), effectBytes.size());
        if (stream.gcount() != static_cast<std::streamsize>(effectBytes.size()))
        {
            error = "Truncated replay input effect";
            return ReplayReadResult::Error;
        }
        const std::uint32_t kind = LoadU32(effectBytes.data());
        const std::uint32_t reverse = LoadU32(effectBytes.data() + 36);
        if (kind != static_cast<std::uint32_t>(PlayerInputEffectKind::MovementFeedback) || reverse > 1)
        {
            error = "Replay input effect kind or boolean is invalid";
            return ReplayReadResult::Error;
        }
        RecordedInputEffect effect;
        effect.mKind = static_cast<PlayerInputEffectKind>(kind);
        effect.mAnimationTypeIndex = LoadU32(effectBytes.data() + 4);
        effect.mX = std::bit_cast<std::int32_t>(LoadU32(effectBytes.data() + 8));
        effect.mY = std::bit_cast<std::int32_t>(LoadU32(effectBytes.data() + 12));
        effect.mZ = std::bit_cast<std::int32_t>(LoadU32(effectBytes.data() + 16));
        effect.mLoopDelay = std::bit_cast<std::int32_t>(LoadU32(effectBytes.data() + 20));
        effect.mLoopCount = std::bit_cast<std::int32_t>(LoadU32(effectBytes.data() + 24));
        effect.mFlags = LoadU32(effectBytes.data() + 28);
        effect.mForceZAdjust = std::bit_cast<std::int32_t>(LoadU32(effectBytes.data() + 32));
        effect.mReverse = reverse == 1;
        frame.mInputEffects.push_back(effect);
    }
    if (frame.mRecordedGroups)
    {
        frame.mRecordedGroups->mMembers.reserve(groupCount);
        for (std::uint32_t index = 0; index < groupCount; ++index)
        {
            std::array<std::uint8_t, GROUP_MEMBER_BYTES> memberBytes{};
            stream.read(reinterpret_cast<char*>(memberBytes.data()), memberBytes.size());
            if (stream.gcount() != static_cast<std::streamsize>(memberBytes.size()))
            {
                error = "编队成员载荷被截断";
                return ReplayReadResult::Error;
            }
            frame.mRecordedGroups->mMembers.push_back({LoadU32(memberBytes.data()),
                LoadU32(memberBytes.data() + 4), LoadU32(memberBytes.data() + 8)});
        }
    }
    if (!ValidateRecordedControlGroups(frame.mRecordedGroups, frame.mFrameNumber,
        streamVersion, error))
    {
        return ReplayReadResult::Error;
    }
    outFrame = std::move(frame);
    error.clear();
    return ReplayReadResult::Frame;
}

bool FrameCodec::ValidateGameSpeed(const ReplayFrame& frame, std::uint32_t streamVersion,
    std::uint32_t gameMode, std::string& error)
{
    if (!frame.mGameSpeed)
    {
        // 单机开局速度来自 spawn.ini，回放仍以录制值为准；第 0 帧必须给出写回起点。
        if (CarriesGameSpeed(streamVersion, gameMode) && frame.mFrameNumber == 0)
        {
            error = "单机帧流第 0 帧缺少游戏速度";
            return false;
        }
        error.clear();
        return true;
    }
    if (streamVersion < GAME_SPEED_STREAM_VERSION)
    {
        error = "游戏速度只能出现在帧流版本 4 及以上";
        return false;
    }
    if (*frame.mGameSpeed > MAX_GAME_SPEED)
    {
        error = "游戏速度超出 0..6：" + std::to_string(*frame.mGameSpeed);
        return false;
    }
    // 联机速度由原生 GameSpeed 事件在事件阶段写入，帧流不得另行携带。
    if (!IsOfflineGameMode(gameMode))
    {
        error = "联机帧流不得记录游戏速度";
        return false;
    }
    error.clear();
    return true;
}
}
