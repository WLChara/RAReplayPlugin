#include "../src/storage/ReplayArchive.h"
#include "../src/storage/ReplayBlockCodec.h"
#include "../src/storage/FrameReader.h"
#include "../src/storage/FrameWriter.h"
#include "../src/replay/FrameCodec.h"
#include "../src/replay/ReplaySession.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace rareplay::storage;
using namespace rareplay::replay;

constexpr std::size_t CONTAINER_HEADER_BYTES = 96;
constexpr std::size_t STREAM_HEADER_BYTES = 32;
constexpr std::size_t EVENT_BYTES = 111;
constexpr std::size_t EFFECT_BYTES = 40;
constexpr std::size_t GROUP_MEMBER_BYTES = 12;

void Require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        wchar_t base[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, base) != 0, "GetTempPathW failed");
        for (unsigned int attempt = 0; attempt < 100; ++attempt)
        {
            mPath = std::filesystem::path(base) /
                (L"RAReplay未完成测试-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(attempt));
            if (CreateDirectoryW(mPath.c_str(), nullptr)) { return; }
        }
        throw std::runtime_error("unable to create temporary directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(mPath, ignored);
    }
    std::filesystem::path File(const wchar_t* name) const { return mPath / name; }
private:
    std::filesystem::path mPath;
};

std::array<std::uint8_t, 32> TargetHash()
{
    std::array<std::uint8_t, 32> hash{};
    for (std::size_t index = 0; index < hash.size(); ++index)
    { hash[index] = static_cast<std::uint8_t>(index + 1); }
    return hash;
}

const std::vector<std::uint8_t> METADATA = {0x41, 0x00, 0xfe, 0x42, 0x43};

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "unable to open test file");
    LARGE_INTEGER size{};
    Require(GetFileSizeEx(file, &size) && size.QuadPart >= 0, "unable to size test file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    Require(bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
        &read, nullptr) && read == bytes.size()), "unable to read test file");
    CloseHandle(file);
    return bytes;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "unable to create test file");
    DWORD written = 0;
    Require(bytes.empty() || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
        &written, nullptr) && written == bytes.size()), "unable to write test file");
    CloseHandle(file);
}

void PatchU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    Require(offset + 4 <= bytes.size(), "patch offset out of range");
    for (unsigned int index = 0; index < 4; ++index)
    { bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8)); }
}

/** 创建逻辑长度为 totalBytes 的稀疏文件，开头为 head；只用于验证 1 GiB 边界，不应占用实际磁盘空间。 */
void WriteSparseFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& head,
    std::uint64_t totalBytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "unable to create sparse test file");
    // 尽力而为：文件系统不支持稀疏文件时仍可继续，只是会占用磁盘空间。
    DWORD returned = 0;
    DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    DWORD written = 0;
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(totalBytes);
    Require(WriteFile(file, head.data(), static_cast<DWORD>(head.size()), &written, nullptr) &&
        written == head.size() && SetFilePointerEx(file, end, nullptr, FILE_BEGIN) &&
        SetEndOfFile(file), "unable to extend sparse test file");
    CloseHandle(file);
}

void Append(std::vector<std::uint8_t>& destination, const std::vector<std::uint8_t>& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

std::vector<std::uint8_t> Prefix(const std::vector<std::uint8_t>& bytes, std::size_t count)
{
    Require(count <= bytes.size(), "prefix length out of range");
    return std::vector<std::uint8_t>(bytes.data(), bytes.data() + count);
}

void ZeroRange(std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count)
{
    Require(offset + count <= bytes.size(), "zero range out of range");
    for (std::size_t index = 0; index < count; ++index) { bytes[offset + index] = 0; }
}

/**
 * 四帧样本：帧 2 同时含事件、输入效果和（版本 3 起）三个编队成员，便于在各载荷中途截断。
 * 版本 4 单机样本（gameMode 5）另在帧 0 记录速度 2、帧 2 记录速度 4；联机样本不带速度。
 */
struct Sample
{
    ReplaySessionInfo mInfo;
    std::vector<ReplayFrame> mFrames;
    std::size_t mHeaderBytes = 0;
    /** 帧 2 记录中事件载荷的起点：记录头之后，若带速度再加 4 字节。 */
    std::size_t mThirdPayloadOffset = 0;
};

constexpr std::uint32_t ONLINE_MODE = 3;
constexpr std::uint32_t SKIRMISH_MODE = 5;

Sample MakeSample(std::uint32_t version, std::uint32_t gameMode = ONLINE_MODE)
{
    Sample sample;
    sample.mInfo = ReplaySessionInfo{gameMode, 1, 0, 6, 42, version};
    sample.mHeaderBytes = version >= 3 ? 24 : 20;
    const bool carriesSpeed = FrameCodec::CarriesGameSpeed(version, gameMode);
    const bool hasChecksum = !FrameCodec::IsOfflineGameMode(gameMode);
    sample.mThirdPayloadOffset = sample.mHeaderBytes + (carriesSpeed ? 4 : 0);

    ReplayFrame first;
    if (version >= 3) { first.mRecordedGroups.emplace(); }
    if (carriesSpeed) { first.mGameSpeed = 2; }

    ReplayFrame second;
    second.mFrameNumber = 1;
    second.mHasChecksum = hasChecksum;
    second.mChecksum = hasChecksum ? 0x01020304 : 0;
    RecordedEvent event{};
    event[0] = 0xa1;
    event[110] = 0xb2;
    second.mEvents.push_back(event);
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
    second.mInputEffects.push_back(effect);
    if (version >= 3)
    {
        second.mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}, {9, 202, 52}}};
    }

    ReplayFrame third;
    third.mFrameNumber = 2;
    third.mHasChecksum = hasChecksum;
    third.mChecksum = hasChecksum ? 0x0a0b0c0d : 0;
    if (carriesSpeed) { third.mGameSpeed = 4; }
    RecordedEvent firstEvent{};
    RecordedEvent secondEvent{};
    firstEvent[0] = 3;
    secondEvent[0] = 4;
    third.mEvents = {firstEvent, secondEvent};
    RecordedInputEffect firstEffect;
    firstEffect.mAnimationTypeIndex = 5;
    RecordedInputEffect secondEffect;
    secondEffect.mAnimationTypeIndex = 6;
    secondEffect.mX = 77;
    third.mInputEffects = {firstEffect, secondEffect};
    if (version >= 3)
    {
        third.mRecordedGroups = RecordedControlGroupSnapshot{{{1, 101, 52}, {2, 303, 52}, {9, 202, 52}}};
    }

    ReplayFrame fourth;
    fourth.mFrameNumber = 3;
    sample.mFrames = {first, second, third, fourth};
    return sample;
}

std::vector<std::uint8_t> EncodeStreamHeader(const Sample& sample)
{
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    std::string error;
    Require(FrameCodec::EncodeHeader(sample.mInfo, header, error), "stream header should encode");
    return std::vector<std::uint8_t>(header.begin(), header.end());
}

std::vector<std::uint8_t> EncodeRecord(const Sample& sample, std::size_t index)
{
    std::vector<std::uint8_t> bytes;
    std::string error;
    Require(FrameCodec::EncodeFrame(sample.mFrames[index], bytes, error, sample.mInfo.mStreamVersion),
        "sample frame should encode");
    return bytes;
}

/** 帧流头加前 frameCount 条完整记录。 */
std::vector<std::uint8_t> EncodeStream(const Sample& sample, std::size_t frameCount)
{
    auto bytes = EncodeStreamHeader(sample);
    for (std::size_t index = 0; index < frameCount; ++index)
    {
        Append(bytes, EncodeRecord(sample, index));
    }
    return bytes;
}

std::uint32_t LoadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    Require(offset + 4 <= bytes.size(), "load offset out of range");
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index)
    { value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8); }
    return value;
}

/** 未完成录像的容器前缀（96 字节头加 metadata 段）及其版本；帧流段的组装方式由版本决定。 */
struct Container
{
    std::uint32_t mVersion = 2;
    std::vector<std::uint8_t> mPrefix;
};

/**
 * 容器 v2：写入端已一律写 v3，按 v2 契约手工组装头（flags=0、streamBytes=0、内容哈希全零）与原样 metadata，
 * 字节与旧写入端 Abort 后留下的前缀相同。
 */
Container MakeV2Container()
{
    Container container;
    auto& bytes = container.mPrefix;
    bytes.assign(CONTAINER_HEADER_BYTES, 0);
    const std::array<std::uint8_t, 8> magic = {'R', 'A', 'R', 'E', 'P', 'L', '2', 0};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    PatchU32(bytes, 8, 2);
    PatchU32(bytes, 12, static_cast<std::uint32_t>(CONTAINER_HEADER_BYTES));
    PatchU32(bytes, 16, static_cast<std::uint32_t>(METADATA.size()));
    const auto target = TargetHash();
    std::copy(target.begin(), target.end(), bytes.begin() + 32);
    Append(bytes, METADATA);
    return container;
}

/** 容器 v3：用真实写入端生成头与清单块，只打开后中止（没有待写块），留下 flags=0 的 .partial。 */
Container MakeV3Container(const TemporaryDirectory& directory)
{
    ArchiveWriter writer;
    std::string error;
    const auto finalPath = directory.File(L"prefix.yrrep");
    std::filesystem::remove(directory.File(L"prefix.yrrep.partial"));
    Require(writer.Open(finalPath.wstring(), TargetHash(), METADATA, error), "prefix writer should open");
    writer.Abort();
    Container container;
    container.mVersion = 3;
    container.mPrefix = ReadBytes(directory.File(L"prefix.yrrep.partial"));
    Require(container.mPrefix.size() == CONTAINER_HEADER_BYTES + block::HEADER_BYTES + METADATA.size() &&
        LoadU32(container.mPrefix, 8) == 3 && LoadU32(container.mPrefix, 20) == 0,
        "writer prefix is the v3 header plus one stored metadata block");
    return container;
}

Container MakeContainer(const TemporaryDirectory& directory, std::uint32_t containerVersion)
{
    return containerVersion == 3 ? MakeV3Container(directory) : MakeV2Container();
}

std::vector<std::uint8_t> EncodeOneBlock(const std::vector<std::uint8_t>& raw)
{
    std::vector<std::uint8_t> encoded;
    std::string error;
    Require(block::EncodeBlock(raw.data(), raw.size(), encoded, error), "test block encodes");
    return encoded;
}

/** 把逻辑帧流切成至多 chunkBytes 字节的块并依次编码；切点不必落在帧边界上。空流不产生块。 */
std::vector<std::uint8_t> EncodeBlocks(const std::vector<std::uint8_t>& stream, std::size_t chunkBytes)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t offset = 0; offset < stream.size(); offset += chunkBytes)
    {
        const std::size_t size = (std::min)(chunkBytes, stream.size() - offset);
        Append(bytes, EncodeOneBlock(std::vector<std::uint8_t>(stream.begin() + static_cast<std::ptrdiff_t>(offset),
            stream.begin() + static_cast<std::ptrdiff_t>(offset + size))));
    }
    return bytes;
}

// v3 帧流段按 97 字节切块，块边界大多落在帧记录中途，验证帧级规则只看拼接后的逻辑帧流。
constexpr std::size_t BLOCK_SPLIT_BYTES = 97;

std::vector<std::uint8_t> Combine(const std::vector<std::uint8_t>& prefix,
    const std::vector<std::uint8_t>& stream)
{
    auto bytes = prefix;
    Append(bytes, stream);
    return bytes;
}

/** v2 直接拼接帧流；v3 先把帧流切块编码，得到的块全部有效。 */
std::vector<std::uint8_t> Combine(const Container& container, const std::vector<std::uint8_t>& stream)
{
    return Combine(container.mPrefix, container.mVersion == 3 ? EncodeBlocks(stream, BLOCK_SPLIT_BYTES) : stream);
}

struct OpenOutcome
{
    bool mOpened = false;
    bool mIsUnfinalized = false;
    std::uint32_t mRecoveredFrames = 0;
    std::string mError;
};

/** 写入文件后经 FrameReader 打开；打开失败时 reader 保持关闭。 */
OpenOutcome OpenWithReader(const TemporaryDirectory& directory, const wchar_t* name,
    const std::vector<std::uint8_t>& bytes, FrameReader& reader)
{
    const auto path = directory.File(name);
    WriteBytes(path, bytes);
    OpenOutcome outcome;
    std::vector<std::uint8_t> metadata;
    outcome.mOpened = reader.Open(path.wstring(), TargetHash(), metadata, outcome.mError);
    outcome.mIsUnfinalized = reader.IsUnfinalized();
    outcome.mRecoveredFrames = reader.GetRecoveredFrameCount();
    if (outcome.mOpened)
    {
        Require(metadata == METADATA, "recovered archive must return the original metadata");
    }
    return outcome;
}

bool SameFrame(const ReplayFrame& actual, const ReplayFrame& expected, std::uint32_t version)
{
    std::vector<std::uint8_t> actualBytes;
    std::vector<std::uint8_t> expectedBytes;
    std::string error;
    return FrameCodec::EncodeFrame(actual, actualBytes, error, version) &&
        FrameCodec::EncodeFrame(expected, expectedBytes, error, version) &&
        actualBytes == expectedBytes && actual.mFrameNumber == expected.mFrameNumber;
}

/** 读出 expectedCount 帧并与样本逐帧一致，随后必须稳定返回 End。 */
void ExpectFramesThenEnd(FrameReader& reader, const Sample& sample, std::size_t expectedCount)
{
    std::string error;
    for (std::size_t index = 0; index < expectedCount; ++index)
    {
        ReplayFrame decoded;
        Require(reader.ReadFrame(decoded, error) == ReplayReadResult::Frame &&
            SameFrame(decoded, sample.mFrames[index], sample.mInfo.mStreamVersion),
            "recovered frame must match the recorded frame");
    }
    ReplayFrame ignored;
    Require(reader.ReadFrame(ignored, error) == ReplayReadResult::End && error.empty(),
        "recovered part must end with a normal EOF");
    Require(reader.ReadFrame(ignored, error) == ReplayReadResult::End,
        "EOF must be stable and must not decode the ignored tail");
}

void ExpectRecovered(const TemporaryDirectory& directory, const wchar_t* name,
    const std::vector<std::uint8_t>& bytes, const Sample& sample, std::size_t expectedCount,
    const char* message)
{
    FrameReader reader;
    const OpenOutcome outcome = OpenWithReader(directory, name, bytes, reader);
    Require(outcome.mOpened && outcome.mIsUnfinalized &&
        outcome.mRecoveredFrames == expectedCount, message);
    ReplaySessionInfo info;
    std::string error;
    Require(reader.GetSessionInfo(info, error) &&
        info.mStreamVersion == sample.mInfo.mStreamVersion && info.mRandomSeed == 42,
        "recovered reader must expose the stream header");
    ExpectFramesThenEnd(reader, sample, expectedCount);
}

void ExpectRejected(const TemporaryDirectory& directory, const wchar_t* name,
    const std::vector<std::uint8_t>& bytes, const char* message)
{
    FrameReader reader;
    const OpenOutcome outcome = OpenWithReader(directory, name, bytes, reader);
    Require(!outcome.mOpened && !outcome.mError.empty() && !reader.IsUnfinalized() &&
        reader.GetRecoveredFrameCount() == 0, message);
    ReplayFrame ignored;
    std::string error;
    Require(reader.ReadFrame(ignored, error) == ReplayReadResult::Error,
        "a reader that failed to open must stay closed");
}

void TestTruncationPoints(std::uint32_t containerVersion, std::uint32_t version, std::uint32_t gameMode = ONLINE_MODE)
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(version, gameMode);
    const auto container = MakeContainer(directory, containerVersion);
    const auto twoFrames = EncodeStream(sample, 2);
    const auto thirdRecord = EncodeRecord(sample, 2);
    const std::size_t headerBytes = sample.mHeaderBytes;
    const std::size_t payloadStart = sample.mThirdPayloadOffset;
    const std::size_t eventsEnd = payloadStart + 2 * EVENT_BYTES;
    const std::size_t effectsEnd = eventsEnd + 2 * EFFECT_BYTES;
    Require(thirdRecord.size() == effectsEnd + (version >= 3 ? 3 * GROUP_MEMBER_BYTES : 0),
        "sample record layout matches the documented sizes");

    std::vector<std::pair<const char*, std::size_t>> cuts = {
        {"mid frame header", 10},
        {"exactly the frame header", headerBytes},
        {"mid event", payloadStart + EVENT_BYTES + 50},
        {"mid effect", eventsEnd + EFFECT_BYTES + 20},
        {"one byte short", thirdRecord.size() - 1},
    };
    if (version >= 3)
    {
        cuts.push_back({"mid group member", effectsEnd + GROUP_MEMBER_BYTES + 5});
    }
    if (payloadStart != headerBytes)
    {
        cuts.push_back({"mid game speed", headerBytes + 2});
        cuts.push_back({"exactly the game speed", payloadStart});
    }
    for (const auto& [name, cutBytes] : cuts)
    {
        auto stream = twoFrames;
        Append(stream, Prefix(thirdRecord, cutBytes));
        ExpectRecovered(directory, L"cut.yrrep.partial", Combine(container, stream), sample, 2, name);
    }

    ExpectRecovered(directory, L"boundary.yrrep.partial", Combine(container, twoFrames), sample, 2,
        "clean frame boundary recovers every frame");
    ExpectRecovered(directory, L"whole.yrrep.partial", Combine(container, EncodeStream(sample, 3)), sample, 3,
        "a complete third frame is recovered");
    ExpectRecovered(directory, L"all.yrrep.partial", Combine(container, EncodeStream(sample, 4)), sample, 4,
        "an unfinalized file that happens to hold complete frames recovers them all");
}

void TestInvalidTailStopsRecovery(std::uint32_t containerVersion, std::uint32_t version,
    std::uint32_t gameMode = ONLINE_MODE)
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(version, gameMode);
    const auto container = MakeContainer(directory, containerVersion);
    const auto twoFrames = EncodeStream(sample, 2);
    const auto fourthRecord = EncodeRecord(sample, 3);
    const std::size_t payloadStart = sample.mThirdPayloadOffset;

    auto zeroTail = twoFrames;
    zeroTail.insert(zeroTail.end(), 200, 0);
    ExpectRecovered(directory, L"zero-tail.yrrep.partial", Combine(container, zeroTail), sample, 2,
        "all-zero tail is a complete but invalid record");

    // 无效记录之后即使跟着看似有效的帧 3，也必须全部忽略。
    const auto expectStopsAtInvalidRecord = [&](const char* message,
        const std::function<void(std::vector<std::uint8_t>&)>& mutate)
    {
        auto record = EncodeRecord(sample, 2);
        mutate(record);
        auto stream = twoFrames;
        Append(stream, record);
        Append(stream, fourthRecord);
        ExpectRecovered(directory, L"invalid-middle.yrrep.partial", Combine(container, stream), sample, 2,
            message);
    };
    expectStopsAtInvalidRecord("unknown flag bit", [](auto& record) { PatchU32(record, 12, 0x80 | 1); });
    expectStopsAtInvalidRecord("skipped frame number", [](auto& record) { PatchU32(record, 0, 5); });
    expectStopsAtInvalidRecord("repeated frame number", [](auto& record) { PatchU32(record, 0, 1); });
    expectStopsAtInvalidRecord("event count over the limit", [](auto& record) { PatchU32(record, 8, 16385); });
    expectStopsAtInvalidRecord("effect count over the limit", [](auto& record) { PatchU32(record, 16, 4097); });
    expectStopsAtInvalidRecord("invalid effect kind",
        [&](auto& record) { PatchU32(record, payloadStart + 2 * EVENT_BYTES, 9); });
    expectStopsAtInvalidRecord("invalid effect boolean",
        [&](auto& record) { PatchU32(record, payloadStart + 2 * EVENT_BYTES + 36, 2); });
    // 规则 1：v1–3 置 bit2 即无效；v4 联机置 bit2 违反规则 4；v4 单机速度越界违反规则 2。
    if (!FrameCodec::CarriesGameSpeed(version, gameMode))
    {
        expectStopsAtInvalidRecord("game speed flag where speed is not allowed",
            [&](auto& record) { PatchU32(record, 12, (version >= 3 ? 2u : 0u) | 4u); });
    }
    else
    {
        expectStopsAtInvalidRecord("game speed 7",
            [&](auto& record) { PatchU32(record, sample.mHeaderBytes, 7); });
        expectStopsAtInvalidRecord("huge game speed",
            [&](auto& record) { PatchU32(record, sample.mHeaderBytes, 0xffffffffu); });
        expectStopsAtInvalidRecord("unknown flag bit next to the speed flag",
            [](auto& record) { PatchU32(record, 12, 2u | 4u | 8u); });
    }
    if (version >= 3)
    {
        const std::size_t membersStart = payloadStart + 2 * EVENT_BYTES + 2 * EFFECT_BYTES;
        expectStopsAtInvalidRecord("group members out of order",
            [&](auto& record) { PatchU32(record, membersStart, 9); });
        expectStopsAtInvalidRecord("invalid group member target kind",
            [&](auto& record) { PatchU32(record, membersStart + 8, 7); });
        expectStopsAtInvalidRecord("zero group member object id",
            [&](auto& record) { PatchU32(record, membersStart + 4, 0); });
        expectStopsAtInvalidRecord("duplicate group member object id",
            [&](auto& record) { PatchU32(record, membersStart + GROUP_MEMBER_BYTES + 4, 101); });
        expectStopsAtInvalidRecord("group count over the limit",
            [](auto& record) { PatchU32(record, 20, 16385); });
        expectStopsAtInvalidRecord("group members without the snapshot flag",
            [](auto& record) { PatchU32(record, 12, 1); });
    }
}

void TestZeroFramesRejected(std::uint32_t containerVersion, std::uint32_t version, std::uint32_t gameMode = ONLINE_MODE)
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(version, gameMode);
    const auto container = MakeContainer(directory, containerVersion);

    ExpectRejected(directory, L"header-only.yrrep.partial", Combine(container, EncodeStream(sample, 0)),
        "a stream header without any frame is not recoverable");
    const auto firstRecord = EncodeRecord(sample, 0);
    for (const std::size_t cutBytes : {std::size_t{1}, std::size_t{10}, firstRecord.size() - 1})
    {
        auto stream = EncodeStream(sample, 0);
        Append(stream, Prefix(firstRecord, cutBytes));
        ExpectRejected(directory, L"first-cut.yrrep.partial", Combine(container, stream),
            "a truncated first frame is not recoverable");
    }
    // 第 0 帧是完整记录但帧号错误或标志无效时同样没有可恢复帧。
    auto badFirst = EncodeStream(sample, 1);
    PatchU32(badFirst, STREAM_HEADER_BYTES + 12, 0x80);
    ExpectRejected(directory, L"first-invalid.yrrep.partial", Combine(container, badFirst),
        "an invalid first frame is not recoverable");
    auto numberedFirst = EncodeStream(sample, 1);
    PatchU32(numberedFirst, STREAM_HEADER_BYTES, 1);
    ExpectRejected(directory, L"first-numbered.yrrep.partial", Combine(container, numberedFirst),
        "recovery must start at frame zero");
    if (version >= 3)
    {
        auto withoutSnapshot = EncodeStream(sample, 1);
        PatchU32(withoutSnapshot, STREAM_HEADER_BYTES + 12, 0);
        ExpectRejected(directory, L"first-no-snapshot.yrrep.partial", Combine(container, withoutSnapshot),
            "version 3 frame 0 must carry the full snapshot");
        auto zeroTail = EncodeStream(sample, 0);
        zeroTail.insert(zeroTail.end(), 200, 0);
        ExpectRejected(directory, L"v3-zero-tail.yrrep.partial", Combine(container, zeroTail),
            "an all-zero tail is not a valid version 3 first frame");
    }
    if (FrameCodec::CarriesGameSpeed(version, gameMode))
    {
        // 规则 3：单机 v4 第 0 帧缺 bit2（其余部分完整有效）时没有可恢复帧。
        ReplayFrame firstWithoutSpeed = sample.mFrames[0];
        firstWithoutSpeed.mGameSpeed.reset();
        std::vector<std::uint8_t> record;
        std::string error;
        Require(FrameCodec::EncodeFrame(firstWithoutSpeed, record, error, version),
            "a frame 0 without speed still encodes at the codec level");
        auto stream = EncodeStream(sample, 0);
        Append(stream, record);
        Append(stream, EncodeRecord(sample, 1));
        ExpectRejected(directory, L"first-no-speed.yrrep.partial", Combine(container, stream),
            "offline version 4 frame 0 must carry the game speed");
    }
    else if (version >= FrameCodec::GAME_SPEED_STREAM_VERSION)
    {
        // 规则 4：联机 v4 第 0 帧带速度即无效。
        ReplayFrame firstWithSpeed = sample.mFrames[0];
        firstWithSpeed.mGameSpeed = 2;
        std::vector<std::uint8_t> record;
        std::string error;
        Require(FrameCodec::EncodeFrame(firstWithSpeed, record, error, version),
            "a speed frame encodes at the codec level");
        auto stream = EncodeStream(sample, 0);
        Append(stream, record);
        ExpectRejected(directory, L"online-first-speed.yrrep.partial", Combine(container, stream),
            "online version 4 frame 0 must not carry a game speed");
    }
}

void TestHeaderRulesAndStreamHeader()
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(2);
    const auto prefix = MakeV2Container().mPrefix;
    const auto good = Combine(prefix, EncodeStream(sample, 3));
    ExpectRecovered(directory, L"good.yrrep.partial", good, sample, 3, "control file recovers");

    auto streamBytes = good;
    streamBytes[24] = 1;
    ExpectRejected(directory, L"stream-bytes.yrrep.partial", streamBytes,
        "flags 0 with non-zero streamBytes is rejected");
    auto hashByte = good;
    hashByte[64 + 31] = 1;
    ExpectRejected(directory, L"hash.yrrep.partial", hashByte,
        "flags 0 with a non-zero content hash is rejected");
    for (const std::uint32_t flags : {2u, 3u, 0x80000000u, 0xffffffffu})
    {
        auto bad = good;
        PatchU32(bad, 20, flags);
        ExpectRejected(directory, L"flags.yrrep.partial", bad, "unknown flags are rejected");
    }
    auto wrongTarget = good;
    wrongTarget[32] ^= 1;
    ExpectRejected(directory, L"target.yrrep.partial", wrongTarget,
        "unfinalized archives still require the matching gamemd hash");
    auto wrongMagic = good;
    wrongMagic[0] ^= 1;
    ExpectRejected(directory, L"magic.yrrep.partial", wrongMagic, "container magic is required");
    auto wrongVersion = good;
    wrongVersion[8] = 1;
    ExpectRejected(directory, L"version.yrrep.partial", wrongVersion, "container version is required");
    auto zeroMetadata = good;
    zeroMetadata[16] = 0;
    ExpectRejected(directory, L"zero-metadata.yrrep.partial", zeroMetadata, "metadata must be non-empty");
    auto hugeMetadata = good;
    PatchU32(hugeMetadata, 16, 16 * 1024 * 1024 + 1);
    ExpectRejected(directory, L"huge-metadata.yrrep.partial", hugeMetadata, "metadata is bounded");

    // 容器必须完整包含 metadata 和 32 字节帧流头。
    ExpectRejected(directory, L"cut-metadata.yrrep.partial",
        Prefix(good, prefix.size() - 1),
        "metadata must be complete");
    ExpectRejected(directory, L"cut-stream-header.yrrep.partial",
        Prefix(good, prefix.size() + STREAM_HEADER_BYTES - 1),
        "the 32-byte stream header must be complete");
    ExpectRejected(directory, L"no-stream.yrrep.partial", prefix, "an archive without a stream header is rejected");

    auto badStreamMagic = good;
    badStreamMagic[prefix.size()] ^= 1;
    ExpectRejected(directory, L"stream-magic.yrrep.partial", badStreamMagic, "stream magic is required");
    for (const std::uint32_t version : {0u, 6u, 7u, 0xffffffffu})
    {
        auto badStreamVersion = good;
        PatchU32(badStreamVersion, prefix.size() + 4, version);
        ExpectRejected(directory, L"stream-version.yrrep.partial", badStreamVersion,
            "stream version is bounded to 1..5");
    }
    auto badEventBytes = good;
    badEventBytes[prefix.size() + 8] = 110;
    ExpectRejected(directory, L"event-bytes.yrrep.partial", badEventBytes, "event size is fixed");
    auto badFirstFrame = good;
    badFirstFrame[prefix.size() + 20] = 1;
    ExpectRejected(directory, L"first-frame.yrrep.partial", badFirstFrame, "the first frame is fixed at zero");
}

void TestArchiveReaderModes()
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(2);
    const auto prefix = MakeV2Container().mPrefix;
    const auto stream = EncodeStream(sample, 3);
    auto partialBytes = Combine(prefix, stream);
    partialBytes.insert(partialBytes.end(), 10, 0xcc);
    const auto partialPath = directory.File(L"modes.yrrep.partial");
    WriteBytes(partialPath, partialBytes);

    ArchiveReader reader;
    ArchiveInfo info;
    std::string error;
    Require(!reader.Open(partialPath.wstring(), info, error) && !error.empty(),
        "the finalized-only open must keep rejecting an unfinalized file");
    Require(reader.OpenUnfinalized(partialPath.wstring(), info, error) && !info.mIsFinalized &&
        info.mStreamBytes == stream.size() + 10 && info.mMetadata == METADATA &&
        info.mTargetHash == TargetHash() && reader.GetSize() == static_cast<int>(stream.size() + 10),
        "unfinalized open reports the actual stream length without hashing");
    Require(reader.Seek(-10, SEEK_END) && reader.GetPosition() == static_cast<int>(stream.size()),
        "the unfinalized stream is bounded by the actual file length");
    Require(reader.OpenByFlags(partialPath.wstring(), info, error) && !info.mIsFinalized,
        "flag dispatch accepts flags 0");

    // 同一份内容正常完成后，三种打开方式的取舍保持各自的规则。
    const auto finalPath = directory.File(L"modes-final.yrrep");
    {
        FrameWriter writer;
        Require(writer.Open(finalPath.wstring(), TargetHash(), METADATA, sample.mInfo, error),
            "frame writer opens");
        for (std::size_t index = 0; index < 3; ++index)
        {
            Require(writer.AppendFrame(sample.mFrames[index], error), "frame appends");
        }
        Require(writer.Finish(error), "writer finalizes");
    }
    Require(reader.Open(finalPath.wstring(), info, error) && info.mIsFinalized &&
        info.mStreamBytes == stream.size(), "finalized open still works");
    Require(!reader.OpenUnfinalized(finalPath.wstring(), info, error) && !error.empty(),
        "an archive with flags 1 is not an unfinalized recording");
    Require(reader.OpenByFlags(finalPath.wstring(), info, error) && info.mIsFinalized &&
        info.mStreamBytes == stream.size(), "flag dispatch accepts flags 1");
    reader.Close();
}

// 候选帧流 = 文件长度 - 96 - metadata；超过 1 GiB 必须拒绝，而不是截断后继续恢复。
void TestStreamLengthLimitRejectsInsteadOfTruncating()
{
    constexpr std::uint64_t STREAM_LIMIT = 1024ULL * 1024ULL * 1024ULL;
    TemporaryDirectory directory;
    const Sample sample = MakeSample(2);
    const auto prefix = MakeV2Container().mPrefix;
    // 一条有效帧 0，其后全零；全零记录的帧号 0 与期望的 1 不符，扫描在此停止。
    const auto head = Combine(prefix, EncodeStream(sample, 1));

    {
        const auto atLimit = directory.File(L"at-limit.yrrep.partial");
        WriteSparseFile(atLimit, head, prefix.size() + STREAM_LIMIT);
        FrameReader reader;
        std::vector<std::uint8_t> metadata;
        std::string error;
        Require(reader.Open(atLimit.wstring(), TargetHash(), metadata, error) && reader.IsUnfinalized() &&
            reader.GetRecoveredFrameCount() == 1, "a candidate stream of exactly 1 GiB is accepted");
        ExpectFramesThenEnd(reader, sample, 1);
        ArchiveReader archive;
        ArchiveInfo info;
        Require(archive.OpenUnfinalized(atLimit.wstring(), info, error) && info.mStreamBytes == STREAM_LIMIT,
            "the archive layer reports the full 1 GiB candidate stream");
    }
    {
        const auto overLimit = directory.File(L"over-limit.yrrep.partial");
        WriteSparseFile(overLimit, head, prefix.size() + STREAM_LIMIT + 1);
        FrameReader reader;
        std::vector<std::uint8_t> metadata;
        std::string error;
        Require(!reader.Open(overLimit.wstring(), TargetHash(), metadata, error) && !error.empty() &&
            !reader.IsUnfinalized() && reader.GetRecoveredFrameCount() == 0,
            "a candidate stream over 1 GiB is rejected even though its first frame is valid");
        ArchiveReader archive;
        ArchiveInfo info;
        Require(!archive.OpenUnfinalized(overLimit.wstring(), info, error) && !error.empty() &&
            !archive.OpenByFlags(overLimit.wstring(), info, error) && !error.empty(),
            "both unfinalized open paths reject an oversized candidate stream");
    }
}

// v3：候选帧流段超过 1 GiB 同样直接拒绝；恰为 1 GiB 时按块扫描，在第一个无效块头（全零）之前截止。
void TestBlockStreamLengthLimit()
{
    constexpr std::uint64_t STREAM_LIMIT = 1024ULL * 1024ULL * 1024ULL;
    TemporaryDirectory directory;
    const Sample sample = MakeSample(2);
    const auto container = MakeV3Container(directory);
    const auto frameZero = EncodeStream(sample, 1);
    const auto blocks = EncodeBlocks(frameZero, BLOCK_SPLIT_BYTES);
    const auto head = Combine(container.mPrefix, blocks);
    {
        const auto atLimit = directory.File(L"block-at-limit.yrrep.partial");
        WriteSparseFile(atLimit, head, container.mPrefix.size() + STREAM_LIMIT);
        FrameReader reader;
        std::vector<std::uint8_t> metadata;
        std::string error;
        Require(reader.Open(atLimit.wstring(), TargetHash(), metadata, error) && reader.IsUnfinalized() &&
            reader.GetRecoveredFrameCount() == 1 && reader.GetContainerVersion() == 3,
            "a v3 candidate segment of exactly 1 GiB is scanned block by block");
        ExpectFramesThenEnd(reader, sample, 1);
        ArchiveReader archive;
        ArchiveInfo info;
        Require(archive.OpenUnfinalized(atLimit.wstring(), info, error) && info.mStreamBytes == frameZero.size() &&
            info.mStoredStreamBytes == blocks.size(), "only the valid block prefix is counted");
    }
    {
        const auto overLimit = directory.File(L"block-over-limit.yrrep.partial");
        WriteSparseFile(overLimit, head, container.mPrefix.size() + STREAM_LIMIT + 1);
        FrameReader reader;
        std::vector<std::uint8_t> metadata;
        std::string error;
        Require(!reader.Open(overLimit.wstring(), TargetHash(), metadata, error) && !error.empty() &&
            reader.GetRecoveredFrameCount() == 0, "a v3 candidate segment over 1 GiB is rejected");
    }
}

/** 把块头中的一个 u32 字段改成给定值。 */
std::vector<std::uint8_t> WithBlockField(std::vector<std::uint8_t> block, std::size_t fieldOffset, std::uint32_t value)
{
    PatchU32(block, fieldOffset, value);
    return block;
}

// v3 未完成录像的块级截止：块头无效、载荷不完整、解码失败或 CRC 不符即在该块之前截止，其后全部忽略；
// 有效块拼接后的逻辑帧流再按帧级规则截止，两者叠加。
void TestBlockLevelCutoff()
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(3);
    const auto container = MakeV3Container(directory);
    const auto twoFrames = EncodeStream(sample, 2);
    const auto base = Combine(container.mPrefix, EncodeBlocks(twoFrames, BLOCK_SPLIT_BYTES));
    const auto third = EncodeOneBlock(EncodeRecord(sample, 2));
    const auto fourth = EncodeOneBlock(EncodeRecord(sample, 3));
    Require(LoadU32(third, 4) == 1, "the third frame block is a deflate block");
    const auto withTail = [&](const std::vector<std::vector<std::uint8_t>>& tails)
    {
        auto bytes = base;
        for (const auto& tail : tails)
            Append(bytes, tail);
        return bytes;
    };

    ExpectRecovered(directory, L"blocks.yrrep.partial", withTail({third, fourth}), sample, 4,
        "every valid block is recovered");
    ExpectRecovered(directory, L"blocks-boundary.yrrep.partial", base, sample, 2,
        "a file ending exactly at a block boundary recovers every frame");

    // 不完整的块：不足 20 字节的块头、恰好只有块头、载荷差 1 字节。
    for (const std::size_t cut : {std::size_t{1}, std::size_t{10}, std::size_t{19}, block::HEADER_BYTES,
             block::HEADER_BYTES + 1, third.size() - 1})
    {
        ExpectRecovered(directory, L"block-cut.yrrep.partial", withTail({Prefix(third, cut)}), sample, 2,
            "an incomplete block stops recovery before it");
    }
    // 垃圾与全零尾部。
    ExpectRecovered(directory, L"block-garbage.yrrep.partial",
        withTail({std::vector<std::uint8_t>(64, 0xa5), third}), sample, 2, "a garbage block header stops recovery");
    ExpectRecovered(directory, L"block-zero.yrrep.partial", withTail({std::vector<std::uint8_t>(200, 0)}), sample, 2,
        "an all-zero tail stops recovery");
    // 块头字段无效、解码失败或 CRC 不符：其后即使跟着有效块也全部忽略。
    auto damaged = third;
    damaged[block::HEADER_BYTES + (damaged.size() - block::HEADER_BYTES) / 2] ^= 0x10;
    const std::vector<std::pair<const char*, std::vector<std::uint8_t>>> invalidThirds = {
        {"bad magic", WithBlockField(third, 0, block::MAGIC ^ 1)},
        {"method 2", WithBlockField(third, 4, 2)},
        {"raw size over 4 MiB", WithBlockField(third, 8, block::STREAM_RAW_LIMIT + 1)},
        {"deflate payload not shorter than raw", WithBlockField(third, 12, LoadU32(third, 8))},
        {"decodes to fewer bytes than recorded", WithBlockField(third, 8, LoadU32(third, 8) + 1)},
        {"CRC mismatch", WithBlockField(third, 16, LoadU32(third, 16) ^ 1)},
        {"damaged payload", damaged},
    };
    for (const auto& [name, invalid] : invalidThirds)
        ExpectRecovered(directory, L"block-invalid.yrrep.partial", withTail({invalid, fourth}), sample, 2, name);

    // 块级截止落在帧记录中途：最后一个有效块只含帧 2 的前半，帧 2 不完整，只恢复帧 0–1。
    {
        const auto threeFrames = EncodeStream(sample, 3);
        const std::size_t cut = twoFrames.size() + 30;
        const auto bytes = Combine(container.mPrefix, EncodeBlocks(Prefix(threeFrames, cut), BLOCK_SPLIT_BYTES));
        auto rest = EncodeOneBlock(std::vector<std::uint8_t>(threeFrames.begin() + static_cast<std::ptrdiff_t>(cut),
            threeFrames.end()));
        ExpectRecovered(directory, L"block-mid-frame-valid.yrrep.partial", Combine(bytes, rest), sample, 3,
            "a frame spanning two valid blocks is recovered");
        PatchU32(rest, 16, LoadU32(rest, 16) ^ 1);
        ExpectRecovered(directory, L"block-mid-frame.yrrep.partial", Combine(bytes, rest), sample, 2,
            "a frame cut by the block-level stop is not recovered");
    }
    // 帧级截止落在有效块内部：全零记录之后的有效帧也被忽略。
    {
        auto stream = twoFrames;
        stream.insert(stream.end(), 200, 0);
        Append(stream, EncodeRecord(sample, 2));
        ExpectRecovered(directory, L"frame-cut-in-block.yrrep.partial", Combine(container, stream), sample, 2,
            "a frame-level stop inside valid blocks still applies");
    }

    // 第一块无效或没有任何块：没有可恢复的逻辑帧流，拒绝打开。
    ExpectRejected(directory, L"no-block.yrrep.partial", container.mPrefix, "a file without any block is rejected");
    ExpectRejected(directory, L"short-block.yrrep.partial", Combine(container.mPrefix, Prefix(third, 19)),
        "fewer than 20 bytes after the metadata is rejected");
    const auto firstBlock = EncodeOneBlock(twoFrames);
    ExpectRejected(directory, L"first-crc.yrrep.partial",
        Combine(Combine(container.mPrefix, WithBlockField(firstBlock, 16, LoadU32(firstBlock, 16) ^ 1)), third),
        "an invalid first block is rejected even if later blocks are valid");
    ExpectRejected(directory, L"first-garbage.yrrep.partial",
        Combine(container.mPrefix, std::vector<std::uint8_t>(100, 0xa5)), "garbage instead of the first block is rejected");
    // 有效块的逻辑帧流不足 32 字节帧流头。
    ExpectRejected(directory, L"short-logical.yrrep.partial",
        Combine(container.mPrefix, EncodeOneBlock(Prefix(twoFrames, STREAM_HEADER_BYTES - 1))),
        "valid blocks shorter than the stream header are rejected");
}

// v3 未完成录像的头与清单段规则。
void TestBlockHeaderRules()
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(2);
    const auto container = MakeV3Container(directory);
    const auto good = Combine(container, EncodeStream(sample, 3));
    ExpectRecovered(directory, L"v3-good.yrrep.partial", good, sample, 3, "v3 control file recovers");

    auto streamBytes = good;
    streamBytes[24] = 1;
    ExpectRejected(directory, L"v3-stream-bytes.yrrep.partial", streamBytes, "v3 flags 0 needs zero streamBytes");
    auto hashByte = good;
    hashByte[64] = 1;
    ExpectRejected(directory, L"v3-hash.yrrep.partial", hashByte, "v3 flags 0 needs a zero content hash");
    auto flags = good;
    PatchU32(flags, 20, 2);
    ExpectRejected(directory, L"v3-flags.yrrep.partial", flags, "v3 unknown flags are rejected");
    auto wrongTarget = good;
    wrongTarget[32] ^= 1;
    ExpectRejected(directory, L"v3-target.yrrep.partial", wrongTarget, "v3 still requires the gamemd hash");
    auto otherVersion = good;
    PatchU32(otherVersion, 8, 4);
    ExpectRejected(directory, L"v3-version.yrrep.partial", otherVersion, "container version 4 is unsupported");
    auto asVersion2 = good;
    PatchU32(asVersion2, 8, 2);
    ExpectRejected(directory, L"v3-as-v2.yrrep.partial", asVersion2,
        "block content read under the v2 rules has no valid stream header");
    auto headerOnlyMetadata = good;
    PatchU32(headerOnlyMetadata, 16, static_cast<std::uint32_t>(block::HEADER_BYTES));
    ExpectRejected(directory, L"v3-metadata-20.yrrep.partial", headerOnlyMetadata,
        "the v3 metadata segment needs at least 21 bytes");

    // 清单段必须是完整有效的一块，正好占满 metadataBytes。
    auto metadataCrc = good;
    metadataCrc[CONTAINER_HEADER_BYTES + 16] ^= 1;
    ExpectRejected(directory, L"v3-metadata-crc.yrrep.partial", metadataCrc, "a damaged metadata block is rejected");
    auto metadataMagic = good;
    metadataMagic[CONTAINER_HEADER_BYTES] ^= 1;
    ExpectRejected(directory, L"v3-metadata-magic.yrrep.partial", metadataMagic, "a metadata block needs its magic");
    auto metadataSize = good;
    PatchU32(metadataSize, 16, LoadU32(good, 16) + 1);
    ExpectRejected(directory, L"v3-metadata-size.yrrep.partial", metadataSize,
        "a metadata block that does not fill metadataBytes is rejected");
    ExpectRejected(directory, L"v3-metadata-cut.yrrep.partial", Prefix(good, container.mPrefix.size() - 1),
        "an incomplete metadata segment is rejected");
}

void TestFinalizedRulesIgnorePartialSuffix()
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(2);
    const auto finalPath = directory.File(L"complete.yrrep");
    std::string error;
    {
        FrameWriter writer;
        Require(writer.Open(finalPath.wstring(), TargetHash(), METADATA, sample.mInfo, error),
            "frame writer opens");
        for (std::size_t index = 0; index < 3; ++index)
        {
            Require(writer.AppendFrame(sample.mFrames[index], error), "frame appends");
        }
        Require(writer.Finish(error), "writer finalizes");
    }
    const auto complete = ReadBytes(finalPath);

    // 完成文件即使被命名为 .partial 也沿用严格规则，且不被当成未完成录像。
    {
        FrameReader reader;
        const OpenOutcome outcome = OpenWithReader(directory, L"complete.yrrep.partial", complete, reader);
        Require(outcome.mOpened && !outcome.mIsUnfinalized && outcome.mRecoveredFrames == 0 &&
            !reader.IsUnfinalized() && reader.GetRecoveredFrameCount() == 0,
            "finalized files report no recovery regardless of suffix");
        ExpectFramesThenEnd(reader, sample, 3);
    }
    {
        FrameReader reader;
        const OpenOutcome outcome = OpenWithReader(directory, L"plain.yrrep", complete, reader);
        Require(outcome.mOpened && !outcome.mIsUnfinalized && outcome.mRecoveredFrames == 0,
            "regular finalized open is unchanged");
        ExpectFramesThenEnd(reader, sample, 3);
    }

    // flags==1 时没有任何宽松：截断、尾部垃圾、哈希错误都必须拒绝，不能回退成恢复。
    auto truncated = complete;
    truncated.pop_back();
    ExpectRejected(directory, L"complete-truncated.yrrep.partial", truncated, "finalized truncation is rejected");
    auto trailing = complete;
    trailing.push_back(0);
    ExpectRejected(directory, L"complete-trailing.yrrep.partial", trailing, "finalized trailing bytes are rejected");
    auto corrupted = complete;
    corrupted.back() ^= 1;
    ExpectRejected(directory, L"complete-corrupt.yrrep.partial", corrupted, "finalized hash mismatch is rejected");

    // 反过来：清除完成标志、长度和哈希后，同样的内容按未完成录像整体恢复。
    auto reset = complete;
    PatchU32(reset, 20, 0);
    ZeroRange(reset, 24, 8);
    ZeroRange(reset, 64, 32);
    ExpectRecovered(directory, L"complete-reset.yrrep.partial", reset, sample, 3,
        "a header reset to the unfinalized state recovers all frames");
}

void TestWriterAbortLeavesRecoverablePartial()
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(3);
    FrameWriter writer;
    std::string error;
    const auto finalPath = directory.File(L"aborted.yrrep");
    Require(writer.Open(finalPath.wstring(), TargetHash(), METADATA, sample.mInfo, error),
        "frame writer opens");
    for (std::size_t index = 0; index < 3; ++index)
    {
        Require(writer.AppendFrame(sample.mFrames[index], error), "frame appends");
    }
    writer.Abort();
    Require(!std::filesystem::exists(finalPath), "aborted recording never publishes the final file");

    const auto partialPath = directory.File(L"aborted.yrrep.partial");
    // 默认策略下 3 帧都还在待写块中，Abort 把它写成帧流段唯一的一块。
    const auto partialBytes = ReadBytes(partialPath);
    const std::size_t streamStart = CONTAINER_HEADER_BYTES + LoadU32(partialBytes, 16);
    const auto streamBytes = EncodeStream(sample, 3);
    Require(LoadU32(partialBytes, 8) == 3 && partialBytes.size() > streamStart + block::HEADER_BYTES &&
        LoadU32(partialBytes, streamStart) == block::MAGIC && LoadU32(partialBytes, streamStart + 8) == streamBytes.size() &&
        partialBytes.size() == streamStart + block::HEADER_BYTES + LoadU32(partialBytes, streamStart + 12),
        "abort writes the pending block as the only stream block");
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    Require(reader.Open(partialPath.wstring(), TargetHash(), metadata, error) && metadata == METADATA &&
        reader.IsUnfinalized() && reader.GetRecoveredFrameCount() == 3,
        "the writer's own partial output is recoverable");
    ExpectFramesThenEnd(reader, sample, 3);

    // 已写出若干块之后 Abort：待写块（帧 3）接在其后写出，全部 4 帧可恢复。
    {
        FrameWriter blockWriter(ArchiveBlockPolicy{64 * 1024, 2});
        const auto path = directory.File(L"aborted-after-blocks.yrrep");
        Require(blockWriter.Open(path.wstring(), TargetHash(), METADATA, sample.mInfo, error), "frame writer opens");
        for (std::size_t index = 0; index < 4; ++index)
            Require(blockWriter.AppendFrame(sample.mFrames[index], error), "frame appends");
        Require(blockWriter.GetLogicalStreamBytes() > blockWriter.GetStoredStreamBytes() &&
            blockWriter.GetStoredStreamBytes() > 0, "two blocks are written and frame 3 is pending");
        blockWriter.Abort();
        FrameReader blockReader;
        Require(blockReader.Open(path.wstring() + L".partial", TargetHash(), metadata, error) &&
            blockReader.GetRecoveredFrameCount() == 4, "abort writes the pending block after the earlier blocks");
        ExpectFramesThenEnd(blockReader, sample, 4);
    }
    // 写入端已处于失败状态时 Abort 不写待写块：帧流段为空，按没有有效块拒绝。
    {
        ArchiveWriter archive;
        const auto path = directory.File(L"failed-abort.yrrep");
        const auto header = EncodeStreamHeader(sample);
        Require(archive.Open(path.wstring(), TargetHash(), METADATA, error) &&
            archive.Write(header.data(), static_cast<int>(header.size())) == static_cast<int>(header.size()) &&
            archive.GetStoredSize() == 0 && archive.Write(nullptr, 5) == 0 && archive.Failed(),
            "an invalid write puts the writer into the failed state");
        archive.Abort();
        const auto bytes = ReadBytes(directory.File(L"failed-abort.yrrep.partial"));
        Require(bytes.size() == CONTAINER_HEADER_BYTES + LoadU32(bytes, 16),
            "a failed writer's abort leaves only the header and the metadata block");
        FrameReader failedReader;
        Require(!failedReader.Open(path.wstring() + L".partial", TargetHash(), metadata, error) && !error.empty(),
            "a partial without stream blocks is not recoverable");
    }

    // 重新打开时先前的状态必须清空；打开失败后不能残留恢复信息。
    const auto missing = directory.File(L"missing.yrrep.partial");
    Require(!reader.Open(missing.wstring(), TargetHash(), metadata, error) &&
        !reader.IsUnfinalized() && reader.GetRecoveredFrameCount() == 0,
        "a failed open clears previous recovery state");
}

class FakeGame final : public IReplayGame
{
public:
    ReplaySessionInfo mInfo{3, 1, 0, 6, 42};
    std::uint32_t mFrame = 0;
    std::vector<ReplayFrame> mExecuted;
    std::vector<std::uint32_t> mAppliedSpeeds;
    std::int32_t mGameSpeed = 2;
    int mEndCount = 0;

    bool ReadSessionInfo(ReplaySessionInfo& outInfo, std::string&) override
    {
        outInfo = mInfo;
        return true;
    }
    bool ReadFrameContext(std::uint32_t& outFrameNumber, std::uint32_t& outChecksum,
        bool& outHasChecksum, std::string&) override
    {
        outFrameNumber = mFrame;
        outChecksum = 0;
        outHasChecksum = false;
        return true;
    }
    bool CopyDueEvents(std::vector<RecordedEvent>&, std::string&) override { return true; }
    bool CopyPlayerInputEffects(std::vector<RecordedInputEffect>&, std::string&) override { return true; }
    bool CopyRecordedControlGroups(std::optional<RecordedControlGroupSnapshot>&, std::string&) override
    {
        return true;
    }
    bool ReadGameSpeed(std::int32_t& outSpeed, std::string&) override
    {
        outSpeed = mGameSpeed;
        return true;
    }
    bool ApplyGameSpeed(std::uint32_t speed, std::string&) override
    {
        mAppliedSpeeds.push_back(speed);
        mGameSpeed = static_cast<std::int32_t>(speed);
        return true;
    }
    bool ApplyPlayerInputEffects(const std::vector<RecordedInputEffect>&, std::string&) override
    {
        return true;
    }
    bool ExecuteReplayFrame(const ReplayFrame& frame, std::string&) override
    {
        mExecuted.push_back(frame);
        return true;
    }
    void DiscardLocalCommands() override {}
    void EndReplay() override { ++mEndCount; }
};

void TestSessionPlaysUnfinalizedReplayToNormalEnd(std::uint32_t containerVersion, std::uint32_t version,
    std::uint32_t gameMode = ONLINE_MODE)
{
    TemporaryDirectory directory;
    const Sample sample = MakeSample(version, gameMode);
    const auto container = MakeContainer(directory, containerVersion);
    auto stream = EncodeStream(sample, 3);
    const auto fourthRecord = EncodeRecord(sample, 3);
    Append(stream, Prefix(fourthRecord, 10));

    FrameReader reader;
    const OpenOutcome outcome = OpenWithReader(directory, L"session.yrrep.partial", Combine(container, stream), reader);
    Require(outcome.mOpened && outcome.mIsUnfinalized && outcome.mRecoveredFrames == 3,
        "session test source opens as an unfinalized replay");

    FakeGame game;
    game.mInfo.mGameMode = gameMode;
    ReplaySession session(game);
    Require(session.StartPlayback(reader), "an unfinalized source starts playback");
    for (std::uint32_t frame = 0; frame < 3; ++frame)
    {
        game.mFrame = frame;
        Require(session.OnFrameBegin() && session.OnBeforeSimulation() &&
            session.ProcessPlaybackFrame() && session.OnFrameEnd() &&
            session.GetState() == ReplayState::Playing,
            "recovered frames play through the normal phases");
        game.mFrame = frame + 1;
        Require(session.OnAfterFrameLoop(), "prefetch after a recovered frame succeeds");
    }
    Require(session.GetState() == ReplayState::Ended && session.IsPlaybackHeld() &&
        session.GetLastCompletedFrame() == 2 && game.mEndCount == 0 &&
        game.mExecuted.size() == 3,
        "the end of the recovered part is a normal EOF that keeps the last frame");
    for (std::size_t index = 0; index < 3; ++index)
    {
        Require(SameFrame(game.mExecuted[index], sample.mFrames[index], version),
            "the session executed exactly the recovered frames in order");
    }
    // 单机 v4 样本帧 0 速度 2、帧 2 速度 4，逐帧写回；其他样本从不写回。
    Require(game.mAppliedSpeeds == (FrameCodec::CarriesGameSpeed(version, gameMode) ?
        std::vector<std::uint32_t>{2, 2, 4} : std::vector<std::uint32_t>{}),
        "recovered speed frames are written back every frame");
    // EOF 之后继续驱动不会读取被忽略的尾部，也不会进入失败。
    Require(session.OnFrameBegin() && session.ProcessPlaybackFrame() &&
        session.GetState() == ReplayState::Ended && game.mExecuted.size() == 3,
        "the held replay does not execute the ignored tail");
    session.Stop();
    Require(session.GetState() != ReplayState::Failed && game.mEndCount == 1,
        "stopping ends the held replay exactly once without a failure state");
}
}

int main()
{
    // 帧级规则在容器 v2（帧流原样存放）与 v3（帧流切块压缩）上逐一重复，结论必须相同。
    for (const std::uint32_t containerVersion : {2u, 3u})
    {
        for (const std::uint32_t version : {2u, 3u})
        {
            TestTruncationPoints(containerVersion, version);
            TestInvalidTailStopsRecovery(containerVersion, version);
            TestZeroFramesRejected(containerVersion, version);
            TestSessionPlaysUnfinalizedReplayToNormalEnd(containerVersion, version);
        }
        // v4 与 v5（布局相同）：联机帧流不带速度，单机帧流在帧 0 与帧 2 带速度。
        for (const std::uint32_t version : {4u, 5u})
        {
            for (const std::uint32_t gameMode : {ONLINE_MODE, SKIRMISH_MODE})
            {
                TestTruncationPoints(containerVersion, version, gameMode);
                TestInvalidTailStopsRecovery(containerVersion, version, gameMode);
                TestZeroFramesRejected(containerVersion, version, gameMode);
                TestSessionPlaysUnfinalizedReplayToNormalEnd(containerVersion, version, gameMode);
            }
        }
    }
    TestHeaderRulesAndStreamHeader();
    TestArchiveReaderModes();
    TestStreamLengthLimitRejectsInsteadOfTruncating();
    TestBlockStreamLengthLimit();
    TestBlockLevelCutoff();
    TestBlockHeaderRules();
    TestFinalizedRulesIgnorePartialSuffix();
    TestWriterAbortLeavesRecoverablePartial();
}
