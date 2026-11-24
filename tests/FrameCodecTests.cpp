#include "../src/replay/FrameCodec.h"

#include <array>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace rareplay::replay;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::istringstream Input(const std::vector<std::uint8_t>& bytes)
{
    return std::istringstream(std::string(bytes.begin(), bytes.end()), std::ios::binary);
}

void TestHeaderLayoutAndValidation()
{
    ReplaySessionInfo info{7, 2, 0, 6, 0x12345678};
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    std::string error;
    Require(FrameCodec::EncodeHeader(info, header, error), "header should encode");
    const std::array<std::uint8_t, 32> expected = {
        0x52, 0x50, 0x46, 0x32, 2, 0, 0, 0, 111, 0, 0, 0,
        7, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0,
        0x78, 0x56, 0x34, 0x12};
    Require(header == expected, "stream header must match the V2 byte contract");
    auto stream = Input(std::vector<std::uint8_t>(header.begin(), header.end()));
    ReplaySessionInfo decoded;
    Require(FrameCodec::DecodeHeader(stream, decoded, error), "header should decode");
    Require(decoded.mGameMode == 7 && decoded.mLocalHouseIndex == 2 &&
        decoded.mFirstFrame == 0 && decoded.mFrameSendRate == 6 &&
        decoded.mRandomSeed == 0x12345678 && decoded.mStreamVersion == 2,
        "session fields should round trip");

    auto legacy = expected;
    legacy[4] = 1;
    stream = Input(std::vector<std::uint8_t>(legacy.begin(), legacy.end()));
    Require(FrameCodec::DecodeHeader(stream, decoded, error) && decoded.mStreamVersion == 1,
        "legacy header remains readable");

    auto invalid = expected;
    invalid[8] = 110;
    stream = Input(std::vector<std::uint8_t>(invalid.begin(), invalid.end()));
    Require(!FrameCodec::DecodeHeader(stream, decoded, error), "wrong event size must fail");
    stream = Input(std::vector<std::uint8_t>(expected.begin(), expected.begin() + 31));
    Require(!FrameCodec::DecodeHeader(stream, decoded, error), "short header must fail");
}

void TestFramesAndEndBoundary()
{
    ReplayFrame empty;
    empty.mFrameNumber = 0;
    std::vector<std::uint8_t> bytes;
    std::string error;
    Require(FrameCodec::EncodeFrame(empty, bytes, error), "empty frame should encode");
    Require(bytes.size() == 20 && bytes[12] == 0 && bytes[16] == 0,
        "empty frame uses a 20 byte record");

    ReplayFrame ordered;
    ordered.mFrameNumber = 1;
    ordered.mChecksum = 0x01020304;
    ordered.mHasChecksum = true;
    RecordedEvent first{};
    RecordedEvent second{};
    first[0] = 0xa1;
    second[0] = 0xb2;
    ordered.mEvents = {first, second};
    RecordedInputEffect effect;
    effect.mAnimationTypeIndex = 17;
    effect.mX = -23;
    effect.mY = 45;
    effect.mZ = -67;
    effect.mLoopDelay = 8;
    effect.mLoopCount = 2;
    effect.mFlags = 0x12345678;
    effect.mForceZAdjust = -9;
    effect.mReverse = true;
    ordered.mInputEffects.push_back(effect);
    std::vector<std::uint8_t> secondBytes;
    Require(FrameCodec::EncodeFrame(ordered, secondBytes, error), "event frame should encode");
    Require(secondBytes.size() == 20 + 2 * 111 + 40 && secondBytes[4] == 4 &&
        secondBytes[7] == 1 && secondBytes[8] == 2 && secondBytes[12] == 1 &&
        secondBytes[16] == 1 && secondBytes[20] == 0xa1 && secondBytes[131] == 0xb2 &&
        secondBytes[242] == 1 && secondBytes[246] == 17 && secondBytes[278] == 1,
        "frame bytes must preserve order, checksum and flag");
    bytes.insert(bytes.end(), secondBytes.begin(), secondBytes.end());
    auto stream = Input(bytes);
    ReplayFrame decoded;
    Require(FrameCodec::ReadFrame(stream, decoded, error) == ReplayReadResult::Frame &&
        decoded.mFrameNumber == 0 && decoded.mEvents.empty() && !decoded.mHasChecksum,
        "empty frame should round trip");
    Require(FrameCodec::ReadFrame(stream, decoded, error) == ReplayReadResult::Frame &&
        decoded.mFrameNumber == 1 && decoded.mHasChecksum &&
        decoded.mChecksum == 0x01020304 && decoded.mEvents.size() == 2 &&
        decoded.mEvents[0][0] == 0xa1 && decoded.mEvents[1][0] == 0xb2 &&
        decoded.mInputEffects.size() == 1 &&
        decoded.mInputEffects[0].mAnimationTypeIndex == 17 &&
        decoded.mInputEffects[0].mX == -23 && decoded.mInputEffects[0].mY == 45 &&
        decoded.mInputEffects[0].mZ == -67 &&
        decoded.mInputEffects[0].mLoopDelay == 8 &&
        decoded.mInputEffects[0].mLoopCount == 2 &&
        decoded.mInputEffects[0].mFlags == 0x12345678 &&
        decoded.mInputEffects[0].mForceZAdjust == -9 &&
        decoded.mInputEffects[0].mReverse,
        "ordered events should round trip");
    Require(FrameCodec::ReadFrame(stream, decoded, error) == ReplayReadResult::End,
        "complete frame boundary is clean EOF");
}

void TestInvalidRecords()
{
    ReplayFrame frame;
    frame.mFrameNumber = 4;
    frame.mEvents.resize(1);
    std::vector<std::uint8_t> bytes;
    std::string error;
    Require(FrameCodec::EncodeFrame(frame, bytes, error), "test frame should encode");
    auto truncated = bytes;
    truncated.pop_back();
    auto stream = Input(truncated);
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "truncated event must fail");

    auto invalidFlags = bytes;
    invalidFlags[12] = 2;
    stream = Input(invalidFlags);
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "reserved flags must fail");

    auto excessive = bytes;
    excessive[8] = 1;
    excessive[9] = 64;
    stream = Input(excessive);
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "event count above 16384 must fail before allocation");

    auto excessiveEffects = bytes;
    excessiveEffects[16] = 1;
    excessiveEffects[17] = 16;
    stream = Input(excessiveEffects);
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "effect count above 4096 must fail before allocation");

    ReplayFrame effectFrame;
    effectFrame.mInputEffects.push_back({});
    Require(FrameCodec::EncodeFrame(effectFrame, bytes, error), "effect frame should encode");
    auto invalidKind = bytes;
    invalidKind[20] = 2;
    stream = Input(invalidKind);
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "unknown effect kind must fail");
    auto invalidBoolean = bytes;
    invalidBoolean[56] = 2;
    stream = Input(invalidBoolean);
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "effect reverse must be zero or one");
    bytes.pop_back();
    stream = Input(bytes);
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "truncated effect must fail");

    std::vector<std::uint8_t> legacy(16, 0);
    stream = Input(legacy);
    Require(FrameCodec::ReadFrame(stream, frame, error, 1) == ReplayReadResult::Frame &&
        frame.mInputEffects.empty(), "legacy frame remains readable with empty effects");

    ReplayFrame tooMany;
    tooMany.mEvents.resize(FrameCodec::MAX_EVENTS_PER_FRAME + 1);
    Require(!FrameCodec::EncodeFrame(tooMany, bytes, error), "encoder must reject oversize frame");

    stream = Input({0x91});
    Require(FrameCodec::ReadFrame(stream, frame, error) == ReplayReadResult::Error,
        "trailing byte is not clean EOF");
}

void StoreU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (8 * index));
}

std::uint32_t LoadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(bytes.at(offset + index)) << (8 * index);
    return value;
}

ReplayReadResult Decode(const std::vector<std::uint8_t>& bytes, ReplayFrame& outFrame,
    std::uint32_t version)
{
    auto stream = Input(bytes);
    std::string error;
    const auto result = FrameCodec::ReadFrame(stream, outFrame, error, version);
    Require(result == ReplayReadResult::Frame || !error.empty(), "decode failures explain themselves");
    return result;
}

// v4 帧头与 v3 相同（24 字节）；bit2 置位时紧跟 u32 游戏速度，然后才是事件、输入效果与编队成员。
void TestVersion4GameSpeedRoundTrip()
{
    ReplaySessionInfo info{5, 1, 0, 6, 0x12345678, 4};
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    std::string error;
    Require(FrameCodec::EncodeHeader(info, header, error) && header[4] == 4,
        "version 4 stream header encodes");
    auto headerStream = Input(std::vector<std::uint8_t>(header.begin(), header.end()));
    ReplaySessionInfo decodedInfo;
    Require(FrameCodec::DecodeHeader(headerStream, decodedInfo, error) &&
        decodedInfo.mStreamVersion == 4 && decodedInfo.mGameMode == 5,
        "version 4 stream header decodes");
    auto unknownVersion = std::vector<std::uint8_t>(header.begin(), header.end());
    unknownVersion[4] = 6;
    headerStream = Input(unknownVersion);
    Require(!FrameCodec::DecodeHeader(headerStream, decodedInfo, error), "version 6 is unknown");
    info.mStreamVersion = 6;
    Require(!FrameCodec::EncodeHeader(info, header, error), "version 6 cannot be written");

    ReplayFrame first;
    first.mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}}};
    first.mGameSpeed = 2;
    std::vector<std::uint8_t> bytes;
    Require(FrameCodec::EncodeFrame(first, bytes, error, 4), "frame with speed encodes");
    Require(bytes.size() == 24 + 4 + 12 && LoadU32(bytes, 12) == 6 && LoadU32(bytes, 20) == 1 &&
        LoadU32(bytes, 24) == 2 && LoadU32(bytes, 28) == 0 && LoadU32(bytes, 32) == 101 &&
        LoadU32(bytes, 36) == 52, "speed sits between the record header and the payloads");
    ReplayFrame decoded;
    Require(Decode(bytes, decoded, 4) == ReplayReadResult::Frame && decoded.mGameSpeed == 2u &&
        decoded.mRecordedGroups == first.mRecordedGroups && decoded.mFrameNumber == 0,
        "frame with speed round trips");

    ReplayFrame mixed;
    mixed.mFrameNumber = 3;
    mixed.mGameSpeed = 6;
    RecordedEvent event{};
    event[0] = 0xa1;
    event[110] = 0xb2;
    mixed.mEvents.push_back(event);
    RecordedInputEffect effect;
    effect.mAnimationTypeIndex = 17;
    effect.mX = -23;
    mixed.mInputEffects.push_back(effect);
    Require(FrameCodec::EncodeFrame(mixed, bytes, error, 4), "mixed frame with speed encodes");
    Require(bytes.size() == 24 + 4 + 111 + 40 && LoadU32(bytes, 12) == 4 && LoadU32(bytes, 24) == 6 &&
        bytes[28] == 0xa1 && bytes[138] == 0xb2 && LoadU32(bytes, 139) == 1 && LoadU32(bytes, 143) == 17,
        "events follow the speed field");
    Require(Decode(bytes, decoded, 4) == ReplayReadResult::Frame && decoded.mGameSpeed == 6u &&
        decoded.mEvents == mixed.mEvents && decoded.mInputEffects.size() == 1 &&
        decoded.mInputEffects[0].mX == -23 && !decoded.mRecordedGroups,
        "events and effects after the speed round trip");

    ReplayFrame plain;
    plain.mFrameNumber = 4;
    plain.mEvents.push_back(event);
    Require(FrameCodec::EncodeFrame(plain, bytes, error, 4), "frame without speed encodes");
    Require(bytes.size() == 24 + 111 && LoadU32(bytes, 12) == 0 && bytes[24] == 0xa1,
        "frame without speed keeps the v3 layout");
    Require(Decode(bytes, decoded, 4) == ReplayReadResult::Frame && !decoded.mGameSpeed &&
        decoded.mEvents == plain.mEvents, "frame without speed round trips");
    Require(Decode(bytes, decoded, 3) == ReplayReadResult::Frame && !decoded.mGameSpeed,
        "a v4 record without speed is byte-identical to v3");
}

void TestVersion4GameSpeedRejections()
{
    std::string error;
    ReplayFrame frame;
    frame.mFrameNumber = 1;
    frame.mGameSpeed = 3;
    std::vector<std::uint8_t> bytes = {0xee};
    for (const std::uint32_t version : {2u, 3u})
        Require(!FrameCodec::EncodeFrame(frame, bytes, error, version) && bytes.size() == 1,
            "versions before 4 cannot carry a game speed");
    frame.mGameSpeed = 7;
    Require(!FrameCodec::EncodeFrame(frame, bytes, error, 4) && bytes.size() == 1,
        "speed 7 cannot be written");

    frame.mGameSpeed = 3;
    std::vector<std::uint8_t> valid;
    Require(FrameCodec::EncodeFrame(frame, valid, error, 4), "valid speed frame encodes");
    ReplayFrame output;
    output.mFrameNumber = 777;
    auto speedSeven = valid;
    StoreU32(speedSeven, 24, 7);
    Require(Decode(speedSeven, output, 4) == ReplayReadResult::Error && output.mFrameNumber == 777,
        "speed 7 is rejected without touching the output");
    StoreU32(speedSeven, 24, 0xffffffffu);
    Require(Decode(speedSeven, output, 4) == ReplayReadResult::Error, "huge speed is rejected");
    for (std::size_t length = 1; length < valid.size(); ++length)
        Require(Decode(std::vector<std::uint8_t>(valid.begin(), valid.begin() + length), output, 4) ==
            ReplayReadResult::Error, "every truncation of a speed record is rejected");
    for (const std::uint32_t unknownBit : {8u, 0x10u, 0x80000000u})
    {
        auto unknown = valid;
        StoreU32(unknown, 12, 4 | unknownBit);
        Require(Decode(unknown, output, 4) == ReplayReadResult::Error, "unknown v4 flag bits are rejected");
    }

    // 旧版本记录置 bit2 一律拒绝，即使后面恰好跟着 4 个字节。
    std::vector<std::uint8_t> v3Record(24 + 4, 0);
    StoreU32(v3Record, 0, 1);
    StoreU32(v3Record, 12, 4);
    Require(Decode(v3Record, output, 3) == ReplayReadResult::Error, "v3 rejects bit2");
    std::vector<std::uint8_t> v2Record(20 + 4, 0);
    StoreU32(v2Record, 12, 4);
    Require(Decode(v2Record, output, 2) == ReplayReadResult::Error, "v2 rejects bit2");
    std::vector<std::uint8_t> v1Record(16 + 4, 0);
    StoreU32(v1Record, 12, 4);
    Require(Decode(v1Record, output, 1) == ReplayReadResult::Error, "v1 rejects bit2");
    Require(Decode(valid, output, 6) == ReplayReadResult::Error, "version 6 records are unknown");
}

// 规则 3、4：需要帧流头 gameMode 的速度规则；完成文件、未完成扫描、会话与写入端共用 ValidateGameSpeed。
void TestStreamGameSpeedRules()
{
    std::string error;
    ReplayFrame first;
    ReplayFrame later;
    later.mFrameNumber = 5;
    ReplayFrame firstWithSpeed;
    firstWithSpeed.mGameSpeed = 0;
    ReplayFrame laterWithSpeed;
    laterWithSpeed.mFrameNumber = 5;
    laterWithSpeed.mGameSpeed = 6;
    for (const std::uint32_t mode : {0u, 5u})
    {
        Require(!FrameCodec::ValidateGameSpeed(first, 4, mode, error) && !error.empty(),
            "offline v4 frame 0 must carry a speed");
        Require(FrameCodec::ValidateGameSpeed(firstWithSpeed, 4, mode, error) &&
            FrameCodec::ValidateGameSpeed(later, 4, mode, error) &&
            FrameCodec::ValidateGameSpeed(laterWithSpeed, 4, mode, error),
            "offline v4 accepts speed on any frame and omission after frame 0");
        Require(FrameCodec::CarriesGameSpeed(4, mode) && !FrameCodec::CarriesGameSpeed(3, mode),
            "only offline v4 streams carry speed");
        for (const std::uint32_t version : {1u, 2u, 3u})
        {
            Require(FrameCodec::ValidateGameSpeed(first, version, mode, error),
                "older offline streams never require a speed");
            Require(!FrameCodec::ValidateGameSpeed(laterWithSpeed, version, mode, error),
                "older streams cannot smuggle a speed");
        }
    }
    for (const std::uint32_t mode : {1u, 2u, 3u, 4u, 6u, 7u})
    {
        Require(!FrameCodec::CarriesGameSpeed(4, mode), "non-offline modes do not carry speed");
        Require(FrameCodec::ValidateGameSpeed(first, 4, mode, error) &&
            FrameCodec::ValidateGameSpeed(later, 4, mode, error),
            "online v4 frames need no speed");
        Require(!FrameCodec::ValidateGameSpeed(firstWithSpeed, 4, mode, error) &&
            !FrameCodec::ValidateGameSpeed(laterWithSpeed, 4, mode, error),
            "online v4 frames must never carry a speed");
    }
    ReplayFrame tooFast;
    tooFast.mFrameNumber = 2;
    tooFast.mGameSpeed = 7;
    Require(!FrameCodec::ValidateGameSpeed(tooFast, 4, 5, error), "in-memory speed 7 is rejected");
}

// v5 只标记录制契约（单机录制时已中和飞机影子绘制），帧流头与每帧记录的布局都与 v4 相同。
void TestVersion5StreamHeader()
{
    static_assert(FrameCodec::GAME_SPEED_STREAM_VERSION == 4);
    static_assert(FrameCodec::AIRCRAFT_DRAW_NEUTRALIZED_STREAM_VERSION == 5);
    static_assert(FrameCodec::LATEST_STREAM_VERSION == 5);

    ReplaySessionInfo info{5, 1, 0, 6, 0x12345678, 4};
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> v4Header{};
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> v5Header{};
    std::string error;
    Require(FrameCodec::EncodeHeader(info, v4Header, error), "version 4 header encodes");
    info.mStreamVersion = 5;
    Require(FrameCodec::EncodeHeader(info, v5Header, error) && v5Header[4] == 5, "version 5 header encodes");
    for (std::size_t index = 0; index < v5Header.size(); ++index)
        Require(index == 4 || v5Header[index] == v4Header[index], "v5 header differs from v4 only in the version");
    auto stream = Input(std::vector<std::uint8_t>(v5Header.begin(), v5Header.end()));
    ReplaySessionInfo decoded;
    Require(FrameCodec::DecodeHeader(stream, decoded, error) && decoded.mStreamVersion == 5 &&
        decoded.mGameMode == 5 && decoded.mLocalHouseIndex == 1 && decoded.mFirstFrame == 0 &&
        decoded.mFrameSendRate == 6 && decoded.mRandomSeed == 0x12345678, "version 5 header round trips");

    auto unknown = std::vector<std::uint8_t>(v5Header.begin(), v5Header.end());
    for (const std::uint32_t version : {6u, 7u, 0xffffffffu})
    {
        StoreU32(unknown, 4, version);
        stream = Input(unknown);
        Require(!FrameCodec::DecodeHeader(stream, decoded, error), "versions above 5 are rejected on read");
        info.mStreamVersion = version;
        Require(!FrameCodec::EncodeHeader(info, v5Header, error), "versions above 5 cannot be written");
    }
    ReplayFrame frame;
    frame.mRecordedGroups.emplace();
    frame.mGameSpeed = 2;
    std::vector<std::uint8_t> bytes = {0xee};
    Require(!FrameCodec::EncodeFrame(frame, bytes, error, 6) && bytes.size() == 1, "version 6 records cannot be written");
    Require(FrameCodec::EncodeFrame(frame, bytes, error, 5), "version 5 record encodes");
    ReplayFrame output;
    Require(Decode(bytes, output, 6) == ReplayReadResult::Error, "version 6 records cannot be read");
}

// 同一内容的 v5 记录与 v4 记录逐字节相同，并按两个版本解码为同一帧。
void TestVersion5RecordsMatchVersion4()
{
    ReplayFrame first;
    first.mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}, {9, 202, 52}}};
    first.mGameSpeed = 2;
    ReplayFrame mixed;
    mixed.mFrameNumber = 3;
    mixed.mGameSpeed = 6;
    RecordedEvent event{};
    event[0] = 0xa1;
    event[110] = 0xb2;
    mixed.mEvents.push_back(event);
    RecordedInputEffect effect;
    effect.mAnimationTypeIndex = 17;
    effect.mX = -23;
    effect.mReverse = true;
    mixed.mInputEffects.push_back(effect);
    mixed.mRecordedGroups = RecordedControlGroupSnapshot{{{1, 101, 52}}};
    ReplayFrame plain;
    plain.mFrameNumber = 4;
    plain.mEvents.push_back(event);
    ReplayFrame online;
    online.mFrameNumber = 5;
    online.mChecksum = 0x01020304;
    online.mHasChecksum = true;
    ReplayFrame empty;
    empty.mFrameNumber = 6;

    std::string error;
    for (const ReplayFrame& frame : {first, mixed, plain, online, empty})
    {
        std::vector<std::uint8_t> v4Bytes;
        std::vector<std::uint8_t> v5Bytes;
        Require(FrameCodec::EncodeFrame(frame, v4Bytes, error, 4) && FrameCodec::EncodeFrame(frame, v5Bytes, error, 5),
            "the frame encodes as v4 and v5");
        Require(v5Bytes == v4Bytes, "a v5 record is byte-identical to the v4 record of the same frame");
        ReplayFrame decoded4;
        ReplayFrame decoded5;
        Require(Decode(v4Bytes, decoded4, 4) == ReplayReadResult::Frame &&
            Decode(v5Bytes, decoded5, 5) == ReplayReadResult::Frame, "the record decodes as v4 and v5");
        std::vector<std::uint8_t> reencoded;
        Require(decoded5.mFrameNumber == frame.mFrameNumber && decoded5.mGameSpeed == frame.mGameSpeed &&
            decoded5.mHasChecksum == frame.mHasChecksum && decoded5.mChecksum == frame.mChecksum &&
            decoded5.mEvents == frame.mEvents && decoded5.mRecordedGroups == frame.mRecordedGroups &&
            FrameCodec::EncodeFrame(decoded5, reencoded, error, 5) && reencoded == v5Bytes &&
            FrameCodec::EncodeFrame(decoded4, reencoded, error, 4) && reencoded == v5Bytes,
            "v4 and v5 decode the record to the same frame");
    }
}

// flags 允许位、bit2 速度字段与帧流级速度规则对 v5 与 v4 完全相同。
void TestVersion5FlagsAndSpeedRulesMatchVersion4()
{
    ReplayFrame frame;
    frame.mFrameNumber = 1;
    frame.mGameSpeed = 3;
    std::vector<std::uint8_t> valid;
    std::string error;
    Require(FrameCodec::EncodeFrame(frame, valid, error, 5) && LoadU32(valid, 12) == 4 && LoadU32(valid, 24) == 3,
        "a v5 speed record keeps bit2 and the speed after the record header");
    for (const std::uint32_t flags : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 0xcu, 0x10u, 0x80000004u})
    {
        auto record = valid;
        StoreU32(record, 12, flags);
        ReplayFrame decoded4;
        ReplayFrame decoded5;
        const auto result4 = Decode(record, decoded4, 4);
        const auto result5 = Decode(record, decoded5, 5);
        Require(result4 == result5, "v5 accepts exactly the flags that v4 accepts");
        Require(result5 == ReplayReadResult::Error || (decoded4.mGameSpeed == decoded5.mGameSpeed &&
            decoded4.mHasChecksum == decoded5.mHasChecksum && decoded4.mRecordedGroups == decoded5.mRecordedGroups),
            "v5 interprets the allowed flags as v4 does");
    }
    for (const std::uint32_t speed : {7u, 0xffffffffu})
    {
        auto record = valid;
        StoreU32(record, 24, speed);
        ReplayFrame output;
        Require(Decode(record, output, 5) == ReplayReadResult::Error, "v5 rejects speeds above 6");
    }

    ReplayFrame first;
    ReplayFrame later;
    later.mFrameNumber = 5;
    ReplayFrame firstWithSpeed;
    firstWithSpeed.mGameSpeed = 0;
    ReplayFrame laterWithSpeed;
    laterWithSpeed.mFrameNumber = 5;
    laterWithSpeed.mGameSpeed = 6;
    ReplayFrame tooFast;
    tooFast.mFrameNumber = 2;
    tooFast.mGameSpeed = 7;
    for (std::uint32_t mode = 0; mode <= 7; ++mode)
    {
        Require(FrameCodec::CarriesGameSpeed(5, mode) == FrameCodec::CarriesGameSpeed(4, mode) &&
            FrameCodec::CarriesGameSpeed(5, mode) == FrameCodec::IsOfflineGameMode(mode),
            "v5 carries speed exactly when v4 does");
        for (const ReplayFrame& candidate : {first, later, firstWithSpeed, laterWithSpeed, tooFast})
        {
            std::string error4;
            std::string error5;
            Require(FrameCodec::ValidateGameSpeed(candidate, 5, mode, error5) ==
                FrameCodec::ValidateGameSpeed(candidate, 4, mode, error4) && error4.empty() == error5.empty(),
                "v5 applies the v4 stream speed rules");
        }
    }
}

void TestAircraftDrawNeutralizedVersions()
{
    for (std::uint32_t version = 1; version <= 6; ++version)
    {
        for (std::uint32_t mode = 0; mode <= 7; ++mode)
        {
            Require(FrameCodec::IsRecordedWithAircraftDrawNeutralized(version, mode) ==
                (version >= 5 && (mode == 0 || mode == 5)),
                "only v5 and later offline streams were recorded with aircraft draw neutralized");
        }
    }
}
}

int main()
{
    TestHeaderLayoutAndValidation();
    TestFramesAndEndBoundary();
    TestInvalidRecords();
    TestVersion4GameSpeedRoundTrip();
    TestVersion4GameSpeedRejections();
    TestStreamGameSpeedRules();
    TestVersion5StreamHeader();
    TestVersion5RecordsMatchVersion4();
    TestVersion5FlagsAndSpeedRulesMatchVersion4();
    TestAircraftDrawNeutralizedVersions();
}
