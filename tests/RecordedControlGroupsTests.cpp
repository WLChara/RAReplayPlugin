#include "../src/replay/FrameCodec.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace rareplay::replay;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::istringstream Input(const std::vector<std::uint8_t>& bytes)
{
    return std::istringstream(std::string(bytes.begin(), bytes.end()), std::ios::binary);
}

void StoreU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (8 * index));
}

std::vector<ReplayFrame> FixtureFrames(std::uint32_t version)
{
    std::vector<ReplayFrame> frames(version == 3 ? 5 : 2);
    for (std::size_t index = 0; index < frames.size(); ++index)
        frames[index].mFrameNumber = static_cast<std::uint32_t>(index);
    frames[1].mHasChecksum = true;
    frames[1].mChecksum = 0x01020304;
    RecordedEvent event{};
    event[0] = 0xa1;
    event[110] = 0xb2;
    frames[1].mEvents.push_back(event);
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
    frames[1].mInputEffects.push_back(effect);
    if (version == 3)
    {
        frames[0].mRecordedGroups.emplace();
        frames[1].mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52},
            {0, 102, 52}, {9, 202, 52}}};
        frames[2].mRecordedGroups = RecordedControlGroupSnapshot{{{1, 101, 52}, {9, 202, 52}}};
        frames[4].mRecordedGroups.emplace();
    }
    return frames;
}

std::vector<std::uint8_t> EncodeFixture(std::uint32_t version)
{
    ReplaySessionInfo info{7, 2, 0, 6, 0x12345678, version};
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    std::string error;
    Require(FrameCodec::EncodeHeader(info, header, error), "fixture header encodes");
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    for (const auto& frame : FixtureFrames(version))
    {
        std::vector<std::uint8_t> record;
        Require(FrameCodec::EncodeFrame(frame, record, error, version), "fixture frame encodes");
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    return bytes;
}

void VerifyFixture(const std::vector<std::uint8_t>& bytes, std::uint32_t version)
{
    auto stream = Input(bytes);
    ReplaySessionInfo info;
    std::string error;
    Require(FrameCodec::DecodeHeader(stream, info, error) && info.mStreamVersion == version,
        "fixture version round trips");
    for (const auto& expected : FixtureFrames(version))
    {
        ReplayFrame actual;
        Require(FrameCodec::ReadFrame(stream, actual, error, info.mStreamVersion) ==
            ReplayReadResult::Frame, "fixture frame reads");
        Require(actual.mFrameNumber == expected.mFrameNumber &&
            actual.mChecksum == expected.mChecksum && actual.mHasChecksum == expected.mHasChecksum &&
            actual.mEvents == expected.mEvents && actual.mRecordedGroups == expected.mRecordedGroups,
            "fixture frame and group state round trip");
        std::vector<std::uint8_t> expectedBytes;
        std::vector<std::uint8_t> actualBytes;
        Require(FrameCodec::EncodeFrame(expected, expectedBytes, error, version) &&
            FrameCodec::EncodeFrame(actual, actualBytes, error, version) &&
            expectedBytes == actualBytes, "all payload fields round trip exactly");
    }
    ReplayFrame frame;
    Require(FrameCodec::ReadFrame(stream, frame, error, version) == ReplayReadResult::End,
        "fixture ends at frame boundary");
}

void RejectBytes(const std::vector<std::uint8_t>& bytes, std::uint32_t version = 3)
{
    auto stream = Input(bytes);
    ReplayFrame output;
    output.mFrameNumber = 777;
    output.mRecordedGroups = RecordedControlGroupSnapshot{{{2, 77, 52}}};
    const auto originalGroups = output.mRecordedGroups;
    std::string error;
    Require(FrameCodec::ReadFrame(stream, output, error, version) == ReplayReadResult::Error &&
        !error.empty(), "invalid record must fail");
    Require(output.mFrameNumber == 777 && output.mRecordedGroups == originalGroups,
        "failed decode must preserve caller output");
}

void TestValidationAndTruncation()
{
    ReplayFrame frame;
    frame.mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}, {9, 202, 52}}};
    std::vector<std::uint8_t> valid;
    std::string error;
    Require(FrameCodec::EncodeFrame(frame, valid, error, 3) && valid.size() == 48 &&
        valid[12] == 2 && valid[20] == 2 && valid[24] == 0 && valid[28] == 101 &&
        valid[32] == 52 && valid[36] == 9 && valid[40] == 202,
        "v3 group payload must match exact byte contract");
    for (std::size_t length = 1; length < valid.size(); ++length)
        RejectBytes(std::vector<std::uint8_t>(valid.begin(), valid.begin() + length));
    for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 8>{{
        {24, 10}, {28, 0}, {32, 51}, {40, 101}, {36, 10}, {12, 4},
        {12, 0}, {20, MAX_RECORDED_GROUP_MEMBERS + 1}}})
    {
        auto invalid = valid;
        StoreU32(invalid, offset, value);
        RejectBytes(invalid);
    }
    auto unordered = valid;
    StoreU32(unordered, 24, 9);
    StoreU32(unordered, 36, 0);
    RejectBytes(unordered);
    auto duplicateInGroup = valid;
    StoreU32(duplicateInGroup, 36, 0);
    StoreU32(duplicateInGroup, 40, 101);
    RejectBytes(duplicateInGroup);
    auto reverseIds = valid;
    StoreU32(reverseIds, 24, 9);
    StoreU32(reverseIds, 28, 203);
    RejectBytes(reverseIds);
    auto unknownKind = valid;
    StoreU32(unknownKind, 32, 0);
    RejectBytes(unknownKind);
    std::vector<std::uint8_t> mixed;
    Require(FrameCodec::EncodeFrame(FixtureFrames(3)[1], mixed, error, 3),
        "mixed v3 frame encodes");
    for (std::size_t length = 1; length < mixed.size(); ++length)
        RejectBytes(std::vector<std::uint8_t>(mixed.begin(), mixed.begin() + length));
    for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 2>{{
        {8, FrameCodec::MAX_EVENTS_PER_FRAME + 1},
        {16, FrameCodec::MAX_INPUT_EFFECTS_PER_FRAME + 1}}})
    {
        auto excessive = mixed;
        StoreU32(excessive, offset, value);
        RejectBytes(excessive);
    }
    auto absentWithCount = valid;
    StoreU32(absentWithCount, 0, 1);
    StoreU32(absentWithCount, 12, 0);
    RejectBytes(absentWithCount);
    auto missingInitial = std::vector<std::uint8_t>(24, 0);
    RejectBytes(missingInitial);
    RejectBytes(valid, 6);
    auto invalidV2Flag = std::vector<std::uint8_t>(20, 0);
    StoreU32(invalidV2Flag, 12, 2);
    RejectBytes(invalidV2Flag, 2);
    invalidV2Flag.resize(16);
    RejectBytes(invalidV2Flag, 1);

    const std::array<RecordedControlGroupSnapshot, 7> invalidSnapshots{{
        {{{10, 101, 52}}}, {{{0, 0, 52}}}, {{{0, 101, 53}}},
        {{{0, 101, 52}, {9, 101, 52}}}, {{{0, 101, 52}, {0, 101, 52}}},
        {{{9, 101, 52}, {0, 202, 52}}}, {{{0, 202, 52}, {0, 101, 52}}}}};
    for (const auto& snapshot : invalidSnapshots)
    {
        frame.mRecordedGroups = snapshot;
        auto output = valid;
        Require(!FrameCodec::EncodeFrame(frame, output, error, 3) && output == valid,
            "invalid snapshot must fail without partial encoding");
    }
    frame.mRecordedGroups = RecordedControlGroupSnapshot{};
    for (std::uint32_t id = 1; id <= MAX_RECORDED_GROUP_MEMBERS; ++id)
        frame.mRecordedGroups->mMembers.push_back({9, id, 52});
    Require(FrameCodec::EncodeFrame(frame, valid, error, 3), "maximum member count encodes");
    auto stream = Input(valid);
    ReplayFrame decoded;
    Require(FrameCodec::ReadFrame(stream, decoded, error, 3) == ReplayReadResult::Frame &&
        decoded.mRecordedGroups == frame.mRecordedGroups, "maximum count round trips");
    frame.mRecordedGroups->mMembers.push_back({9, MAX_RECORDED_GROUP_MEMBERS + 1, 52});
    Require(!FrameCodec::EncodeFrame(frame, valid, error, 3), "over maximum encoder fails");
    frame.mRecordedGroups.emplace();
    Require(!FrameCodec::EncodeFrame(frame, valid, error, 2), "v2 cannot silently discard groups");
    frame.mRecordedGroups.reset();
    Require(!FrameCodec::EncodeFrame(frame, valid, error, 3), "v3 requires initial snapshot");
    Require(!FrameCodec::EncodeFrame(frame, valid, error, 1), "v1 is read only");
    Require(!FrameCodec::EncodeFrame(frame, valid, error, 6), "unknown write version fails");

    ReplaySessionInfo info;
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    for (const std::uint32_t version : {0U, 1U, 6U})
    {
        info.mStreamVersion = version;
        Require(!FrameCodec::EncodeHeader(info, header, error), "invalid write header version fails");
    }
    auto headerBytes = EncodeFixture(3);
    for (const std::uint32_t version : {0U, 6U})
    {
        StoreU32(headerBytes, 4, version);
        stream = Input(headerBytes);
        Require(!FrameCodec::DecodeHeader(stream, info, error), "unknown read header version fails");
    }
}
}

int main(int argc, char** argv)
{
    const bool generate = argc == 3 && std::string(argv[1]) == "--generate";
    Require(argc == 2 || generate, "supply fixture directory or --generate directory");
    const std::filesystem::path directory(argv[generate ? 2 : 1]);
    for (const std::uint32_t version : {2U, 3U})
    {
        const auto encoded = EncodeFixture(version);
        const auto path = directory / ("rpf2-groups-v" + std::to_string(version) + ".bin");
        if (generate)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
            Require(static_cast<bool>(output), "fixture generation writes all bytes");
        }
        std::ifstream input(path, std::ios::binary);
        Require(static_cast<bool>(input), "tracked fixture opens");
        const std::vector<std::uint8_t> tracked((std::istreambuf_iterator<char>(input)), {});
        Require(tracked == encoded, "C++ encoder must reproduce tracked fixture bytes");
        VerifyFixture(tracked, version);
    }
    TestValidationAndTruncation();
}
