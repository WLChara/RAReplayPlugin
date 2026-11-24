#include "../src/storage/ReplayArchive.h"
#include "../src/storage/ReplayBlockCodec.h"
#include "../src/storage/FrameReader.h"
#include "../src/storage/FrameWriter.h"
#include "../src/replay/FrameCodec.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace rareplay::storage;

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
                (L"RAReplay测试-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(attempt));
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

constexpr std::size_t CONTAINER_HEADER_BYTES = 96;

std::uint32_t LoadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    Require(offset + 4 <= bytes.size(), "load offset out of range");
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
    return value;
}

std::uint64_t LoadU64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return LoadU32(bytes, offset) | (static_cast<std::uint64_t>(LoadU32(bytes, offset + 4)) << 32);
}

void StoreU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    Require(offset + 4 <= bytes.size(), "store offset out of range");
    for (unsigned int index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

void StoreU64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value)
{
    StoreU32(bytes, offset, static_cast<std::uint32_t>(value));
    StoreU32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}

std::array<std::uint8_t, 32> Sha256Of(const std::uint8_t* data, std::size_t sizeBytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<std::uint8_t, 32> digest{};
    const bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(sizeBytes), 0) >= 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash != nullptr) { BCryptDestroyHash(hash); }
    if (algorithm != nullptr) { BCryptCloseAlgorithmProvider(algorithm, 0); }
    Require(ok, "SHA-256 failed");
    return digest;
}

std::string ToHex(const std::array<std::uint8_t, 32>& digest)
{
    std::string text;
    for (const std::uint8_t byte : digest)
    {
        char pair[3]{};
        std::snprintf(pair, sizeof(pair), "%02x", byte);
        text += pair;
    }
    return text;
}

std::string FileSha256(const std::vector<std::uint8_t>& bytes)
{
    return ToHex(Sha256Of(bytes.data(), bytes.size()));
}

/** 按当前磁盘字节重算完成文件的内容哈希（文件偏移 96 到文件尾），用于构造只有结构错误、哈希有效的文件。 */
void RehashArchive(std::vector<std::uint8_t>& bytes)
{
    const auto digest = Sha256Of(bytes.data() + CONTAINER_HEADER_BYTES, bytes.size() - CONTAINER_HEADER_BYTES);
    std::copy(digest.begin(), digest.end(), bytes.begin() + 64);
}

/** v3 文件中一块的位置与块头字段。 */
struct BlockLayout
{
    std::size_t mOffset = 0;
    std::uint32_t mMethod = 0;
    std::uint32_t mRawBytes = 0;
    std::uint32_t mStoredBytes = 0;
    std::size_t GetEnd() const { return mOffset + block::HEADER_BYTES + mStoredBytes; }
};

/** 严格解析 [begin, end) 内首尾相接的块头链（不解码载荷）。 */
std::vector<BlockLayout> ParseBlocks(const std::vector<std::uint8_t>& bytes, std::size_t begin, std::size_t end,
    std::uint32_t maxRawBytes = block::STREAM_RAW_LIMIT)
{
    std::vector<BlockLayout> blocks;
    std::size_t offset = begin;
    while (offset < end)
    {
        Require(end - offset >= block::HEADER_BYTES, "block header is complete");
        block::Header header;
        std::string error;
        Require(block::ParseBlockHeader(bytes.data() + offset, maxRawBytes, header, error), "block header is valid");
        BlockLayout layout{offset, static_cast<std::uint32_t>(header.mMethod), header.mRawBytes, header.mStoredBytes};
        Require(layout.GetEnd() <= end, "block payload is complete");
        blocks.push_back(layout);
        offset = layout.GetEnd();
    }
    return blocks;
}

/** v3 文件帧流段的起点：96 字节头加清单段。 */
std::size_t StreamSegmentStart(const std::vector<std::uint8_t>& bytes)
{
    return CONTAINER_HEADER_BYTES + LoadU32(bytes, 16);
}

std::vector<BlockLayout> ParseStreamBlocks(const std::vector<std::uint8_t>& bytes)
{
    return ParseBlocks(bytes, StreamSegmentStart(bytes), bytes.size());
}

/** 经 ArchiveReader 读出完整逻辑帧流（v2 为帧流原样字节，v3 为各块解压后的拼接）。 */
std::vector<std::uint8_t> ReadLogicalStream(const std::filesystem::path& path)
{
    ArchiveReader reader;
    ArchiveInfo info;
    std::string error;
    Require(reader.OpenByFlags(path.wstring(), info, error), "archive opens for logical stream reading");
    std::vector<std::uint8_t> stream(static_cast<std::size_t>(info.mStreamBytes));
    Require(reader.Read(stream.data(), static_cast<int>(stream.size())) == static_cast<int>(stream.size()) &&
        reader.GetStreamError().empty(), "the whole logical stream reads without a stream error");
    return stream;
}

void WriteArchive(const std::filesystem::path& path)
{
    ArchiveWriter writer;
    std::string error;
    std::array<std::uint8_t, 32> stream{};
    stream[0] = 'R'; stream[1] = 'P'; stream[2] = 'F'; stream[3] = '2';
    Require(writer.Open(path.wstring(), TargetHash(), {0x61, 0x00, 0xff}, error),
        "archive open should succeed");
    Require(writer.Write(stream.data(), static_cast<int>(stream.size())) ==
        static_cast<int>(stream.size()),
        "frame stream should write");
    Require(writer.Finish(error), "archive should finalize");
}

bool OpenFails(const std::filesystem::path& path)
{
    ArchiveReader reader;
    ArchiveInfo info;
    std::string error;
    return !reader.Open(path.wstring(), info, error) && !error.empty();
}

void TestRoundTripAndHeader()
{
    TemporaryDirectory directory;
    const auto path = directory.File(L"完整.rep");
    WriteArchive(path);
    const auto bytes = ReadBytes(path);
    const std::array<std::uint8_t, 8> magic = {'R','A','R','E','P','L','2',0};
    // 3 字节清单压缩后不会更短，原样存放：清单段为 20 字节块头加 3 字节。
    constexpr std::size_t METADATA_SEGMENT_BYTES = block::HEADER_BYTES + 3;
    const std::size_t streamStart = CONTAINER_HEADER_BYTES + METADATA_SEGMENT_BYTES;
    const auto target = TargetHash();
    Require(bytes.size() > streamStart &&
        std::equal(magic.begin(), magic.end(), bytes.begin()) &&
        LoadU32(bytes, 8) == 3 && LoadU32(bytes, 12) == 96 && LoadU32(bytes, 16) == METADATA_SEGMENT_BYTES &&
        LoadU32(bytes, 20) == 1 && LoadU64(bytes, 24) == bytes.size() - streamStart &&
        std::equal(target.begin(), target.end(), bytes.begin() + 32),
        "container header must match the v3 byte contract");
    const auto contentHash = Sha256Of(bytes.data() + CONTAINER_HEADER_BYTES, bytes.size() - CONTAINER_HEADER_BYTES);
    Require(std::equal(contentHash.begin(), contentHash.end(), bytes.begin() + 64),
        "the content hash covers every byte after the header");
    const auto metadataBlocks = ParseBlocks(bytes, CONTAINER_HEADER_BYTES, streamStart, block::METADATA_RAW_LIMIT);
    Require(metadataBlocks.size() == 1 && metadataBlocks[0].mMethod == 0 && metadataBlocks[0].mRawBytes == 3 &&
        bytes[116] == 0x61 && bytes[117] == 0x00 && bytes[118] == 0xff,
        "the metadata segment is exactly one stored block");
    const auto streamBlocks = ParseStreamBlocks(bytes);
    Require(streamBlocks.size() == 1 && streamBlocks[0].mRawBytes == 32, "the 32-byte stream is one block");
    ArchiveReader reader;
    ArchiveInfo info;
    std::string error;
    Require(reader.Open(path.wstring(), info, error), "final archive should reopen");
    Require(info.mTargetHash == TargetHash() &&
        info.mMetadata == std::vector<std::uint8_t>({0x61,0x00,0xff}) && info.mContainerVersion == 3 &&
        info.mStreamBytes == 32 && info.mStoredStreamBytes == bytes.size() - streamStart &&
        reader.GetSize() == 32 && info.mIsFinalized,
        "hash, metadata and stream length must round trip");
    std::array<char, 4> first{};
    reader.GetStream().read(first.data(), first.size());
    Require(std::string(first.data(), first.size()) == "RPF2" &&
        reader.GetPosition() == 4, "bounded stream should start at frame stream");
    Require(reader.Seek(-4, SEEK_END) && reader.GetPosition() == 28,
        "seek must be stream-relative");
    Require(!reader.Seek(1, SEEK_END), "seek past stream boundary must fail");
}

void TestNoOverwriteAndIncomplete()
{
    TemporaryDirectory directory;
    const auto finalPath = directory.File(L"final.rep");
    const auto partialPath = directory.File(L"partial.rep.partial");
    WriteBytes(finalPath, {9,8,7});
    WriteBytes(partialPath, {1,2,3});
    ArchiveWriter writer;
    std::string error;
    Require(!writer.Open(finalPath.wstring(), TargetHash(), {1}, error),
        "existing final must not be overwritten");
    Require(!writer.Open(directory.File(L"partial.rep").wstring(), TargetHash(), {1}, error),
        "existing partial must not be overwritten");
    Require(ReadBytes(finalPath) == std::vector<std::uint8_t>({9,8,7}) &&
        ReadBytes(partialPath) == std::vector<std::uint8_t>({1,2,3}),
        "refusal must preserve original bytes");
    const auto unfinishedPath = directory.File(L"unfinished.rep");
    {
        ArchiveWriter unfinished;
        Require(unfinished.Open(unfinishedPath.wstring(), TargetHash(), {1}, error),
            "partial should open");
        Require(!unfinished.Finish(error), "stream shorter than 32 bytes cannot finalize");
    }
    Require(!std::filesystem::exists(unfinishedPath) &&
        std::filesystem::exists(directory.File(L"unfinished.rep.partial")),
        "unfinalized partial must not publish");
    Require(OpenFails(directory.File(L"unfinished.rep.partial")),
        "partial archive must not play");
}

void TestCorruptionAndBounds()
{
    TemporaryDirectory directory;
    const auto path = directory.File(L"good.rep");
    WriteArchive(path);
    const auto good = ReadBytes(path);
    const auto reject = [&](const wchar_t* name, std::vector<std::uint8_t> bytes)
    {
        const auto badPath = directory.File(name);
        WriteBytes(badPath, bytes);
        Require(OpenFails(badPath), "corrupt archive must fail");
    };
    auto truncated = good; truncated.pop_back(); reject(L"truncated.rep", truncated);
    auto trailing = good; trailing.push_back(0); reject(L"trailing.rep", trailing);
    auto checksum = good; checksum.back() ^= 1; reject(L"checksum.rep", checksum);
    auto unfinalized = good; unfinalized[20] = 0; reject(L"unfinalized.rep", unfinalized);
    auto zeroMetadata = good; zeroMetadata[16] = 0; reject(L"zero-metadata.rep", zeroMetadata);
    // v3 的清单段至少是 20 字节块头加 1 字节载荷。
    auto headerOnlyMetadata = good; StoreU32(headerOnlyMetadata, 16, 20);
    reject(L"header-only-metadata.rep", headerOnlyMetadata);
    auto tinyStream = good; StoreU64(tinyStream, 24, 20); reject(L"tiny-stream.rep", tinyStream);
    auto tooMuchMetadata = good;
    tooMuchMetadata[16] = 1; tooMuchMetadata[17] = 0;
    tooMuchMetadata[18] = 0; tooMuchMetadata[19] = 1;
    reject(L"oversize-metadata.rep", tooMuchMetadata);
    auto tooMuchStream = good; tooMuchStream[28] = 1;
    reject(L"oversize-stream.rep", tooMuchStream);
    auto oldVersion = good; oldVersion[8] = 1; reject(L"old-version.rep", oldVersion);
}

void TestFrameArchiveRoundTrip()
{
    TemporaryDirectory directory;
    const auto path = directory.File(L"frames.rep");
    rareplay::replay::ReplaySessionInfo sessionInfo{3, 1, 0, 6, 42};
    FrameWriter writer;
    std::string error;
    Require(writer.Open(path.wstring(), TargetHash(), {0x41}, sessionInfo, error),
        "frame writer should open");
    rareplay::replay::ReplayFrame first;
    Require(writer.AppendFrame(first, error), "empty frame should append");
    rareplay::replay::ReplayFrame second;
    second.mFrameNumber = 1;
    second.mHasChecksum = true;
    second.mChecksum = 0x10203040;
    rareplay::replay::RecordedEvent event{};
    event[0] = 0xa5;
    second.mEvents.push_back(event);
    rareplay::replay::RecordedInputEffect effect;
    effect.mAnimationTypeIndex = 11;
    effect.mX = -100;
    second.mInputEffects.push_back(effect);
    Require(writer.AppendFrame(second, error), "event frame should append");
    Require(!writer.AppendFrame(second, error), "duplicate frame should be rejected");
    writer.Abort();
    Require(!std::filesystem::exists(path), "failed sequence must not publish final file");

    const auto completePath = directory.File(L"complete-frames.rep");
    Require(writer.Open(completePath.wstring(), TargetHash(), {0x41}, sessionInfo, error),
        "new frame writer should open");
    Require(writer.AppendFrame(first, error) && writer.AppendFrame(second, error) &&
        writer.Finish(error), "valid sequence should finalize");
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    auto wrongHash = TargetHash();
    wrongHash[0] ^= 1;
    Require(!reader.Open(completePath.wstring(), wrongHash, metadata, error),
        "wrong target binary must be refused");
    Require(reader.Open(completePath.wstring(), TargetHash(), metadata, error) &&
        metadata == std::vector<std::uint8_t>({0x41}),
        "correct target should open with original metadata");
    rareplay::replay::ReplaySessionInfo decodedInfo;
    Require(reader.GetSessionInfo(decodedInfo, error) && decodedInfo.mRandomSeed == 42 &&
        decodedInfo.mStreamVersion == 2 &&
        decodedInfo.mGameMode == 3, "stream session identity must round trip");
    rareplay::replay::ReplaySessionInfo repeatedInfo;
    Require(reader.GetSessionInfo(repeatedInfo, error) &&
        repeatedInfo.mRandomSeed == decodedInfo.mRandomSeed &&
        repeatedInfo.mFirstFrame == decodedInfo.mFirstFrame,
        "repeated header reads must preserve the first frame position");
    rareplay::replay::ReplayFrame decoded;
    Require(reader.ReadFrame(decoded, error) == rareplay::replay::ReplayReadResult::Frame &&
        decoded.mFrameNumber == 0 && decoded.mEvents.empty(), "empty frame must round trip");
    Require(reader.ReadFrame(decoded, error) == rareplay::replay::ReplayReadResult::Frame &&
        decoded.mFrameNumber == 1 && decoded.mHasChecksum &&
        decoded.mChecksum == 0x10203040 && decoded.mEvents.size() == 1 &&
        decoded.mEvents[0][0] == 0xa5 && decoded.mInputEffects.size() == 1 &&
        decoded.mInputEffects[0].mAnimationTypeIndex == 11 &&
        decoded.mInputEffects[0].mX == -100, "event and effect frame must round trip");
    Require(reader.ReadFrame(decoded, error) == rareplay::replay::ReplayReadResult::End,
        "complete frame boundary must be clean EOF");
}

void TestReaderRejectsValidContainerWithSkippedFrame()
{
    TemporaryDirectory directory;
    const auto path = directory.File(L"skipped.rep");
    rareplay::replay::ReplaySessionInfo sessionInfo{3, 1, 0, 6, 42};
    std::array<std::uint8_t, rareplay::replay::FrameCodec::HEADER_BYTES> header{};
    std::string error;
    Require(rareplay::replay::FrameCodec::EncodeHeader(sessionInfo, header, error),
        "stream header should encode");
    rareplay::replay::ReplayFrame skipped;
    skipped.mFrameNumber = 1;
    std::vector<std::uint8_t> frameBytes;
    Require(rareplay::replay::FrameCodec::EncodeFrame(skipped, frameBytes, error),
        "skipped frame should encode as raw bytes");
    ArchiveWriter archive;
    Require(archive.Open(path.wstring(), TargetHash(), {1}, error) &&
        archive.Write(header.data(), static_cast<int>(header.size())) ==
            static_cast<int>(header.size()) &&
        archive.Write(frameBytes.data(), static_cast<int>(frameBytes.size())) ==
            static_cast<int>(frameBytes.size()) && archive.Finish(error),
        "container with structurally valid but skipped stream should finalize");
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    Require(reader.Open(path.wstring(), TargetHash(), metadata, error),
        "SHA-valid archive should open");
    rareplay::replay::ReplayFrame frame;
    Require(reader.ReadFrame(frame, error) == rareplay::replay::ReplayReadResult::Error &&
        !error.empty(), "frame reader must reject a skipped first frame");
}

void TestLegacyFrameArchiveCanBeInspected()
{
    TemporaryDirectory directory;
    const auto path = directory.File(L"legacy-frames.rep");
    rareplay::replay::ReplaySessionInfo sessionInfo{3, 1, 0, 6, 42};
    std::array<std::uint8_t, rareplay::replay::FrameCodec::HEADER_BYTES> header{};
    std::string error;
    Require(rareplay::replay::FrameCodec::EncodeHeader(sessionInfo, header, error),
        "stream header should encode");
    header[4] = 1;
    std::array<std::uint8_t, 16> legacyFrame{};
    ArchiveWriter archive;
    Require(archive.Open(path.wstring(), TargetHash(), {1}, error) &&
        archive.Write(header.data(), static_cast<int>(header.size())) ==
            static_cast<int>(header.size()) &&
        archive.Write(legacyFrame.data(), static_cast<int>(legacyFrame.size())) ==
            static_cast<int>(legacyFrame.size()) && archive.Finish(error),
        "legacy stream should fit the unchanged archive container");
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    Require(reader.Open(path.wstring(), TargetHash(), metadata, error),
        "legacy stream should open for inspection");
    rareplay::replay::ReplaySessionInfo info;
    Require(reader.GetSessionInfo(info, error) && info.mStreamVersion == 1 &&
        reader.GetSessionInfo(info, error) && info.mStreamVersion == 1,
        "repeated legacy header reads should preserve stream version");
    rareplay::replay::ReplayFrame frame;
    Require(reader.ReadFrame(frame, error) == rareplay::replay::ReplayReadResult::Frame &&
        frame.mFrameNumber == 0 && frame.mInputEffects.empty() &&
        reader.ReadFrame(frame, error) == rareplay::replay::ReplayReadResult::End,
        "legacy frame should decode with no input effects");
}

void TestGroupFrameArchiveVersionAndInitialSnapshot()
{
    using namespace rareplay::replay;
    TemporaryDirectory directory;
    ReplaySessionInfo info{3, 1, 0, 6, 42, 3};
    FrameWriter writer;
    std::string error;
    Require(writer.Open(directory.File(L"empty-v3.rep").wstring(), TargetHash(), {1}, info, error),
        "v3 writer opens");
    Require(!writer.Finish(error), "v3 cannot finish without an initial frame");
    writer.Abort();
    Require(writer.Open(directory.File(L"missing-v3.rep").wstring(), TargetHash(), {1}, info, error),
        "v3 writer opens after abort");
    ReplayFrame first;
    Require(!writer.AppendFrame(first, error) && !writer.Finish(error),
        "v3 writer enforces initial snapshot using its header version");
    writer.Abort();
    const auto path = directory.File(L"groups-v3.rep");
    Require(writer.Open(path.wstring(), TargetHash(), {1}, info, error), "v3 writer reopens");
    first.mRecordedGroups.emplace();
    ReplayFrame update;
    update.mFrameNumber = 1;
    update.mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}, {9, 202, 52}}};
    ReplayFrame unchanged;
    unchanged.mFrameNumber = 2;
    ReplayFrame clear;
    clear.mFrameNumber = 3;
    clear.mRecordedGroups.emplace();
    Require(writer.AppendFrame(first, error) && writer.AppendFrame(update, error) &&
        writer.AppendFrame(unchanged, error) && writer.AppendFrame(clear, error) &&
        writer.Finish(error), "group update sequence finalizes");
    const auto stream = ReadLogicalStream(path);
    Require(stream[4] == 3 && stream.size() == 32 + 4 * 24 + 2 * 12,
        "v3 writer header and frame lengths agree");
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    Require(reader.Open(path.wstring(), TargetHash(), metadata, error) &&
        reader.GetSessionInfo(info, error) && info.mStreamVersion == 3,
        "v3 archive opens with matching version");
    for (const auto& expected : {first, update, unchanged, clear})
    {
        ReplayFrame decoded;
        Require(reader.ReadFrame(decoded, error) == ReplayReadResult::Frame &&
            decoded.mFrameNumber == expected.mFrameNumber &&
            decoded.mRecordedGroups == expected.mRecordedGroups,
            "archive preserves optional group updates");
    }
    ReplayFrame decoded;
    Require(reader.ReadFrame(decoded, error) == ReplayReadResult::End, "v3 ends normally");
    reader.Close();

    // 外层 SHA 正确也不能让被截断的帧载荷变成正常 EOF。
    const auto truncatedPath = directory.File(L"truncated-group.rep");
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    Require(FrameCodec::EncodeHeader(info, header, error), "v3 header encodes");
    std::vector<std::uint8_t> record;
    update.mFrameNumber = 0;
    Require(FrameCodec::EncodeFrame(update, record, error, 3), "v3 initial member frame encodes");
    record.pop_back();
    ArchiveWriter archive;
    Require(archive.Open(truncatedPath.wstring(), TargetHash(), {1}, error) &&
        archive.Write(header.data(), static_cast<int>(header.size())) == static_cast<int>(header.size()) &&
        archive.Write(record.data(), static_cast<int>(record.size())) == static_cast<int>(record.size()) &&
        archive.Finish(error), "truncated frame has a valid outer container");
    Require(reader.Open(truncatedPath.wstring(), TargetHash(), metadata, error) &&
        reader.ReadFrame(decoded, error) == ReplayReadResult::Error &&
        reader.ReadFrame(decoded, error) == ReplayReadResult::Error,
        "truncated group payload fails and poisons reader");
    reader.Close();

    const auto emptyPath = directory.File(L"header-only-v3.rep");
    Require(archive.Open(emptyPath.wstring(), TargetHash(), {1}, error) &&
        archive.Write(header.data(), static_cast<int>(header.size())) == static_cast<int>(header.size()) &&
        archive.Finish(error), "header-only stream fits outer container");
    Require(reader.Open(emptyPath.wstring(), TargetHash(), metadata, error) &&
        reader.ReadFrame(decoded, error) == ReplayReadResult::Error,
        "v3 reader rejects missing initial frame");
    reader.Close();

    info.mStreamVersion = 2;
    Require(writer.Open(directory.File(L"groups-v2.rep").wstring(), TargetHash(), {1}, info, error) &&
        !writer.AppendFrame(first, error), "writer cannot emit groups into v2");
    writer.Abort();
}

/** 用原始帧字节组装完成文件：外层容器与 SHA 都有效，只检验帧流规则。 */
void WriteRawFrames(const std::filesystem::path& path, const rareplay::replay::ReplaySessionInfo& info,
    const std::vector<std::vector<std::uint8_t>>& records)
{
    using rareplay::replay::FrameCodec;
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    std::string error;
    Require(FrameCodec::EncodeHeader(info, header, error), "raw stream header encodes");
    ArchiveWriter archive;
    Require(archive.Open(path.wstring(), TargetHash(), {1}, error) &&
        archive.Write(header.data(), static_cast<int>(header.size())) == static_cast<int>(header.size()),
        "raw archive opens");
    for (const auto& record : records)
        Require(archive.Write(record.data(), static_cast<int>(record.size())) ==
            static_cast<int>(record.size()), "raw record writes");
    Require(archive.Finish(error), "raw archive finalizes");
}

std::vector<std::uint8_t> EncodeRecord(const rareplay::replay::ReplayFrame& frame, std::uint32_t version)
{
    std::vector<std::uint8_t> bytes;
    std::string error;
    Require(rareplay::replay::FrameCodec::EncodeFrame(frame, bytes, error, version), "raw record encodes");
    return bytes;
}

/** 读到第 failingIndex 帧时失败（-1 表示全部读完并正常 EOF），失败后读取端保持失败。 */
void ExpectReadOutcome(const std::filesystem::path& path, std::size_t frameCount, int failingIndex,
    const char* message)
{
    using namespace rareplay::replay;
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    std::string error;
    Require(reader.Open(path.wstring(), TargetHash(), metadata, error), "SHA-valid speed archive opens");
    for (std::size_t index = 0; index < frameCount; ++index)
    {
        ReplayFrame frame;
        const auto result = reader.ReadFrame(frame, error);
        if (static_cast<int>(index) == failingIndex)
        {
            Require(result == ReplayReadResult::Error && !error.empty() &&
                reader.ReadFrame(frame, error) == ReplayReadResult::Error, message);
            return;
        }
        Require(result == ReplayReadResult::Frame && frame.mFrameNumber == index, message);
    }
    ReplayFrame frame;
    Require(failingIndex < 0 && reader.ReadFrame(frame, error) == ReplayReadResult::End, message);
}

void TestGameSpeedArchiveRules()
{
    using namespace rareplay::replay;
    TemporaryDirectory directory;
    ReplaySessionInfo offline{5, 1, 0, 6, 42, 4};
    ReplaySessionInfo online{3, 1, 0, 6, 42, 4};
    ReplayFrame first;
    first.mRecordedGroups.emplace();
    first.mGameSpeed = 2;
    ReplayFrame plain;
    plain.mFrameNumber = 1;
    ReplayFrame changed;
    changed.mFrameNumber = 2;
    changed.mGameSpeed = 4;

    // 写入端：单机帧流按读取端同一规则写 v4，联机帧流不得带速度。
    FrameWriter writer;
    std::string error;
    const auto path = directory.File(L"speed-v4.rep");
    Require(writer.Open(path.wstring(), TargetHash(), {1}, offline, error) &&
        writer.AppendFrame(first, error) && writer.AppendFrame(plain, error) &&
        writer.AppendFrame(changed, error) && writer.Finish(error), "offline v4 speed sequence finalizes");
    const auto stream = ReadLogicalStream(path);
    Require(stream[4] == 4 && stream.size() == 32 + 28 + 24 + 28,
        "v4 writer header and record lengths agree");
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    ReplaySessionInfo decodedInfo;
    Require(reader.Open(path.wstring(), TargetHash(), metadata, error) &&
        reader.GetSessionInfo(decodedInfo, error) && decodedInfo.mStreamVersion == 4 &&
        decodedInfo.mGameMode == 5, "v4 archive opens with its header");
    for (const auto& expected : {first, plain, changed})
    {
        ReplayFrame decoded;
        Require(reader.ReadFrame(decoded, error) == ReplayReadResult::Frame &&
            decoded.mFrameNumber == expected.mFrameNumber && decoded.mGameSpeed == expected.mGameSpeed &&
            decoded.mRecordedGroups == expected.mRecordedGroups, "archive preserves optional speeds");
    }
    ReplayFrame end;
    Require(reader.ReadFrame(end, error) == ReplayReadResult::End, "v4 archive ends normally");
    reader.Close();

    ReplayFrame missingSpeed;
    missingSpeed.mRecordedGroups.emplace();
    Require(writer.Open(directory.File(L"missing-speed.rep").wstring(), TargetHash(), {1}, offline, error) &&
        !writer.AppendFrame(missingSpeed, error) && !error.empty(),
        "offline v4 writer refuses frame 0 without speed");
    writer.Abort();
    ReplayFrame plainWithSpeed = plain;
    plainWithSpeed.mGameSpeed = 4;
    Require(writer.Open(directory.File(L"online-speed.rep").wstring(), TargetHash(), {1}, online, error) &&
        writer.AppendFrame(missingSpeed, error) && !writer.AppendFrame(plainWithSpeed, error) &&
        !error.empty(), "online v4 writer refuses any speed");
    writer.Abort();
    Require(writer.Open(directory.File(L"online-first-speed.rep").wstring(), TargetHash(), {1}, online,
        error) && !writer.AppendFrame(first, error), "online v4 writer refuses speed on frame 0");
    writer.Abort();
    ReplaySessionInfo legacy = offline;
    legacy.mStreamVersion = 3;
    Require(writer.Open(directory.File(L"v3-speed.rep").wstring(), TargetHash(), {1}, legacy, error) &&
        !writer.AppendFrame(first, error), "v3 writer refuses speed");
    writer.Abort();

    // 读取端：外层 SHA 有效时，帧流规则仍逐帧执行。
    const auto firstRecord = EncodeRecord(first, 4);
    const auto missingRecord = EncodeRecord(missingSpeed, 4);
    const auto plainRecord = EncodeRecord(plain, 4);
    const auto changedRecord = EncodeRecord(changed, 4);
    const auto offlineMissing = directory.File(L"offline-missing.rep");
    WriteRawFrames(offlineMissing, offline, {missingRecord, plainRecord});
    ExpectReadOutcome(offlineMissing, 2, 0, "offline v4 frame 0 without bit2 is rejected");
    for (const std::uint32_t mode : {0u, 5u})
    {
        auto info = offline;
        info.mGameMode = mode;
        const auto accepted = directory.File(L"offline-sparse.rep");
        std::filesystem::remove(accepted);
        WriteRawFrames(accepted, info, {firstRecord, plainRecord, changedRecord});
        ExpectReadOutcome(accepted, 3, -1, "offline later frames may omit bit2");
    }
    const auto onlineFirst = directory.File(L"online-first.rep");
    WriteRawFrames(onlineFirst, online, {firstRecord});
    ExpectReadOutcome(onlineFirst, 1, 0, "online frame 0 with bit2 is rejected");
    const auto onlineLater = directory.File(L"online-later.rep");
    WriteRawFrames(onlineLater, online, {missingRecord, plainRecord, changedRecord});
    ExpectReadOutcome(onlineLater, 3, 2, "online later frame with bit2 is rejected");
    const auto onlinePlain = directory.File(L"online-plain.rep");
    WriteRawFrames(onlinePlain, online, {missingRecord, plainRecord});
    ExpectReadOutcome(onlinePlain, 2, -1, "online v4 without speeds is accepted");
    auto tooFast = changedRecord;
    tooFast[24] = 7;
    const auto speedSeven = directory.File(L"speed-seven.rep");
    WriteRawFrames(speedSeven, offline, {firstRecord, plainRecord, tooFast});
    ExpectReadOutcome(speedSeven, 3, 2, "speed 7 is rejected by the finalized reader");
}

/** 经正式写入端写出一份完成文件并返回其逻辑帧流；帧流头版本位于逻辑偏移 4。 */
std::vector<std::uint8_t> WriteFramesThroughWriter(const std::filesystem::path& path,
    const rareplay::replay::ReplaySessionInfo& info, const std::vector<rareplay::replay::ReplayFrame>& frames)
{
    FrameWriter writer;
    std::string error;
    Require(writer.Open(path.wstring(), TargetHash(), {1}, info, error), "writer opens");
    for (const auto& frame : frames)
        Require(writer.AppendFrame(frame, error), "writer appends the frame");
    Require(writer.Finish(error), "writer finalizes");
    return ReadLogicalStream(path);
}

// v5 写读往返：布局与 v4 相同，单机帧流照常带速度，联机帧流不带；版本 6 在写入端与读取端都被拒绝。
void TestVersion5ArchiveRoundTrip()
{
    using namespace rareplay::replay;
    TemporaryDirectory directory;
    ReplayFrame first;
    first.mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}}};
    first.mGameSpeed = 2;
    ReplayFrame plain;
    plain.mFrameNumber = 1;
    ReplayFrame changed;
    changed.mFrameNumber = 2;
    changed.mGameSpeed = 4;
    ReplayFrame onlineFirst;
    onlineFirst.mRecordedGroups.emplace();
    onlineFirst.mHasChecksum = true;
    onlineFirst.mChecksum = 0x01020304;
    const std::vector<ReplayFrame> offlineFrames = {first, plain, changed};
    const std::vector<ReplayFrame> onlineFrames = {onlineFirst, plain};
    constexpr std::size_t VERSION_OFFSET = 4;
    for (const std::uint32_t mode : {0u, 5u, 3u})
    {
        const bool isOffline = FrameCodec::IsOfflineGameMode(mode);
        const auto& frames = isOffline ? offlineFrames : onlineFrames;
        const ReplaySessionInfo info{mode, 1, 0, 6, 42, 5};
        const auto path = directory.File(isOffline ? (mode == 0 ? L"v5-campaign.rep" : L"v5-skirmish.rep") :
            L"v5-online.rep");
        const auto bytes = WriteFramesThroughWriter(path, info, frames);
        Require(bytes[VERSION_OFFSET] == 5, "v5 writer stores version 5 in the stream header");

        FrameReader reader;
        std::vector<std::uint8_t> metadata;
        ReplaySessionInfo decodedInfo;
        std::string error;
        Require(reader.Open(path.wstring(), TargetHash(), metadata, error) &&
            reader.GetSessionInfo(decodedInfo, error) && decodedInfo.mStreamVersion == 5 &&
            decodedInfo.mGameMode == mode && !reader.IsUnfinalized(), "v5 archive opens with its header");
        for (const auto& expected : frames)
        {
            ReplayFrame decoded;
            Require(reader.ReadFrame(decoded, error) == ReplayReadResult::Frame &&
                decoded.mFrameNumber == expected.mFrameNumber && decoded.mGameSpeed == expected.mGameSpeed &&
                decoded.mHasChecksum == expected.mHasChecksum && decoded.mChecksum == expected.mChecksum &&
                decoded.mRecordedGroups == expected.mRecordedGroups, "v5 archive round trips every frame");
        }
        ReplayFrame end;
        Require(reader.ReadFrame(end, error) == ReplayReadResult::End, "v5 archive ends normally");
        reader.Close();

        // 同一内容的 v4 逻辑帧流只在帧流头版本字节上不同（压缩后的块与内容哈希随之不同，不在此比较）。
        auto v4Info = info;
        v4Info.mStreamVersion = 4;
        const auto v4Bytes = WriteFramesThroughWriter(directory.File(L"v4-same-content.rep"), v4Info, frames);
        std::filesystem::remove(directory.File(L"v4-same-content.rep"));
        Require(v4Bytes.size() == bytes.size(), "v4 and v5 streams of the same frames have the same size");
        for (std::size_t index = 0; index < bytes.size(); ++index)
            Require(bytes[index] == v4Bytes[index] || index == VERSION_OFFSET,
                "v4 and v5 streams differ only in the stream version");
    }

    FrameWriter writer;
    std::string error;
    Require(!writer.Open(directory.File(L"v6.rep").wstring(), TargetHash(), {1},
        ReplaySessionInfo{5, 1, 0, 6, 42, 6}, error) && !error.empty(), "the writer refuses version 6");
    // 外层容器与哈希都有效、只有帧流头版本为 6 的文件也必须被读取端拒绝。
    std::array<std::uint8_t, FrameCodec::HEADER_BYTES> header{};
    Require(FrameCodec::EncodeHeader(ReplaySessionInfo{5, 1, 0, 6, 42, 5}, header, error), "v5 header encodes");
    header[4] = 6;
    const auto record = EncodeRecord(first, 5);
    const auto v6Path = directory.File(L"v6-read.rep");
    ArchiveWriter archive;
    Require(archive.Open(v6Path.wstring(), TargetHash(), {1}, error) &&
        archive.Write(header.data(), static_cast<int>(header.size())) == static_cast<int>(header.size()) &&
        archive.Write(record.data(), static_cast<int>(record.size())) == static_cast<int>(record.size()) &&
        archive.Finish(error), "a SHA-valid version 6 archive is written");
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    Require(!reader.Open(v6Path.wstring(), TargetHash(), metadata, error) && !error.empty(),
        "the reader refuses version 6");
}
}


namespace
{
// 写入端的切块策略：生产值按 600 个单元或 64 KiB 写出一块，块只在单元边界切分。
void TestWriterBlockPolicy()
{
    TemporaryDirectory directory;
    std::string error;
    std::array<std::uint8_t, 32> streamHeader{};
    streamHeader[0] = 'R'; streamHeader[1] = 'P'; streamHeader[2] = 'F'; streamHeader[3] = '2';

    // 600 个单元：帧流头加 599 条 24 字节记录写出第一块，再 600 条写出第二块。
    {
        const auto path = directory.File(L"units.rep");
        ArchiveWriter writer;
        Require(writer.Open(path.wstring(), TargetHash(), {1}, error) &&
            writer.Write(streamHeader.data(), 32) == 32, "default policy writer opens");
        std::vector<std::uint8_t> record(24, 0x5a);
        for (int unit = 1; unit < 600; ++unit)
        {
            Require(writer.GetStoredSize() == 0, "no block is written before the 600th unit");
            Require(writer.Write(record.data(), 24) == 24, "record unit writes");
        }
        Require(writer.GetStoredSize() > 0 && writer.GetSize() == 32 + 599 * 24, "the 600th unit writes a block");
        const int firstBlockBytes = writer.GetStoredSize();
        for (int unit = 0; unit < 600; ++unit)
            Require(writer.Write(record.data(), 24) == 24, "record unit writes");
        Require(writer.GetStoredSize() > firstBlockBytes && writer.GetSize() == 32 + 1199 * 24,
            "the next 600 units write the second block");
        Require(writer.Finish(error) && writer.GetStoredSize() > firstBlockBytes, "writer finalizes");
        const auto bytes = ReadBytes(path);
        const auto blocks = ParseStreamBlocks(bytes);
        Require(blocks.size() == 2 && blocks[0].mRawBytes == 32 + 599 * 24 && blocks[1].mRawBytes == 600 * 24 &&
            LoadU64(bytes, 24) == static_cast<std::uint64_t>(writer.GetStoredSize()),
            "blocks are cut every 600 units and streamBytes is the stored segment size");
    }
    // 64 KiB：帧流头加 7 个 10000 字节单元达到 70032 字节即写出，剩余单元由 Finish 写出。
    {
        const auto path = directory.File(L"bytes.rep");
        ArchiveWriter writer;
        std::vector<std::uint8_t> unit(10000);
        for (std::size_t index = 0; index < unit.size(); ++index)
            unit[index] = static_cast<std::uint8_t>(index * 7);
        Require(writer.Open(path.wstring(), TargetHash(), {1}, error) &&
            writer.Write(streamHeader.data(), 32) == 32, "byte policy writer opens");
        for (int count = 0; count < 8; ++count)
            Require(writer.Write(unit.data(), 10000) == 10000, "large unit writes");
        Require(writer.Finish(error), "byte policy writer finalizes");
        const auto blocks = ParseStreamBlocks(ReadBytes(path));
        Require(blocks.size() == 2 && blocks[0].mRawBytes == 32 + 7 * 10000 && blocks[1].mRawBytes == 10000,
            "a block is written once the pending block reaches 64 KiB");
    }
    // 测试用大目标值：追加会使待写块超过 4 MiB 时先写出待写块；单个单元超过 4 MiB 无法表示，写入失败。
    {
        const auto path = directory.File(L"large.rep");
        ArchiveWriter writer(ArchiveBlockPolicy{block::STREAM_RAW_LIMIT, 1000});
        const std::vector<std::uint8_t> unit(3 * 1024 * 1024, 0);
        Require(writer.Open(path.wstring(), TargetHash(), {1}, error) &&
            writer.Write(streamHeader.data(), 32) == 32 &&
            writer.Write(unit.data(), static_cast<int>(unit.size())) == static_cast<int>(unit.size()) &&
            writer.GetStoredSize() == 0 &&
            writer.Write(unit.data(), static_cast<int>(unit.size())) == static_cast<int>(unit.size()) &&
            writer.GetStoredSize() > 0 && writer.Finish(error), "large units split before the 4 MiB block limit");
        const auto blocks = ParseStreamBlocks(ReadBytes(path));
        Require(blocks.size() == 2 && blocks[0].mRawBytes == 32 + unit.size() && blocks[1].mRawBytes == unit.size(),
            "the pending block is written before it would exceed 4 MiB");
        const std::vector<std::uint8_t> oversized(block::STREAM_RAW_LIMIT + 1, 0);
        Require(writer.Open(directory.File(L"oversized.rep").wstring(), TargetHash(), {1}, error) &&
            writer.Write(oversized.data(), static_cast<int>(oversized.size())) == 0 && writer.Failed() &&
            !writer.Finish(error), "a unit larger than one block is refused");
        writer.Abort();
    }
    // 无效策略在打开时拒绝，不创建 .partial。
    for (const auto& policy : {ArchiveBlockPolicy{0, 600}, ArchiveBlockPolicy{block::STREAM_RAW_LIMIT + 1, 600},
             ArchiveBlockPolicy{64 * 1024, 0}})
    {
        ArchiveWriter writer(policy);
        Require(!writer.Open(directory.File(L"bad-policy.rep").wstring(), TargetHash(), {1}, error) && !error.empty() &&
            !std::filesystem::exists(directory.File(L"bad-policy.rep.partial")), "an invalid block policy is refused");
    }
    // 清单上限：16 MiB 的可压缩清单可以写入；超过 16 MiB 拒绝。
    {
        ArchiveWriter writer;
        std::vector<std::uint8_t> metadata(block::METADATA_RAW_LIMIT, 0x20);
        Require(writer.Open(directory.File(L"big-metadata.rep").wstring(), TargetHash(), metadata, error),
            "a compressible 16 MiB manifest is accepted");
        writer.Abort();
        metadata.push_back(0x20);
        Require(!writer.Open(directory.File(L"huge-metadata.rep").wstring(), TargetHash(), metadata, error) &&
            !std::filesystem::exists(directory.File(L"huge-metadata.rep.partial")), "a manifest over 16 MiB is refused");
    }
}

// v4、v5 调速跨语言样例（容器 v2，已冻结），各文件的字节数与 SHA-256 记录在下表中。
// 写入端改写容器 v3 后无法再重新生成它们：常规测试核对文件 SHA-256 等于下表记录的值，再逐帧解码。
struct FrozenFixture
{
    const wchar_t* mName;
    std::uint32_t mStreamVersion;
    bool mIsUnfinalized;
    std::size_t mFileBytes;
    const char* mSha256;
};
constexpr std::array<FrozenFixture, 4> FROZEN_V2_FIXTURES = {{
    {L"replay-v4-speed.yrrep", 4, false, 583, "790f7150123f63d40ea78ff51ff5bafa321d17bd279c79aeabea4bdbb4862562"},
    {L"replay-v4-speed-partial.yrrep", 4, true, 509, "3b81229a7b2136ae56401a70bffa2b0c4cc96ac8bc5057d337e172781200a081"},
    {L"replay-v5-speed.yrrep", 5, false, 583, "1477c66c51623024588115036321f98bd7766acf36bda8efccfda3c6a9c9680d"},
    {L"replay-v5-speed-partial.yrrep", 5, true, 509, "2660ded69dc819d5de8e1c6cf30fc9c02cdce2b641f68bc58026c7324596acfd"},
}};
const std::vector<std::uint8_t> SPEED_FIXTURE_METADATA = {0x41, 0x00, 0xfe, 0x42, 0x43};
constexpr std::size_t SPEED_PARTIAL_COMPLETE_FRAMES = 5;
// v2 样例中帧流起点与帧流头 version 字段的文件偏移：96 字节容器头、5 字节 metadata（再加 4 字节 magic）。
constexpr std::size_t SPEED_FIXTURE_STREAM_OFFSET = 96 + 5;
constexpr std::size_t SPEED_FIXTURE_VERSION_OFFSET = SPEED_FIXTURE_STREAM_OFFSET + 4;

// 容器 v3 跨语言样例：帧内容与 v5 样例相同，测试用切块策略每块最多 3 个单元，
// 得到 3 块：帧流头加帧 0–1、帧 2–4、帧 5。由 `ReplayArchiveTests --generate <目录>` 经正式 FrameWriter 生成，
// 常规测试重新生成并逐字节比较已跟踪文件。
constexpr const wchar_t* BLOCK_FIXTURE_COMPLETE = L"replay-c3-v5-speed.yrrep";
constexpr const wchar_t* BLOCK_FIXTURE_PARTIAL = L"replay-c3-v5-speed-partial.yrrep";
constexpr std::uint32_t BLOCK_FIXTURE_STREAM_VERSION = 5;
constexpr ArchiveBlockPolicy BLOCK_FIXTURE_POLICY{64 * 1024, 3};
// 各块解压后的长度：32+52+135（32 字节帧流头加帧 0–1）、64+28+24（帧 2–4）、147（帧 5）；帧长与 v5 样例相同。
constexpr std::array<std::uint32_t, 3> BLOCK_FIXTURE_RAW_BYTES = {219, 116, 147};

rareplay::replay::ReplaySessionInfo SpeedFixtureInfo(std::uint32_t streamVersion)
{
    return {5, 1, 0, 6, 0x12345678, streamVersion};
}

rareplay::replay::RecordedEvent SpeedFixtureEvent(std::uint8_t first, std::uint32_t frame, std::uint8_t last)
{
    rareplay::replay::RecordedEvent event{};
    event[0] = first;
    // 单机分派采集把事件 Frame 字段（偏移 3）记为执行帧。
    for (unsigned int index = 0; index < 4; ++index)
        event[3 + index] = static_cast<std::uint8_t>(frame >> (index * 8));
    event[110] = last;
    return event;
}

std::vector<rareplay::replay::ReplayFrame> SpeedFixtureFrames()
{
    using namespace rareplay::replay;
    std::vector<ReplayFrame> frames(6);
    for (std::size_t index = 0; index < frames.size(); ++index)
        frames[index].mFrameNumber = static_cast<std::uint32_t>(index);
    frames[0].mGameSpeed = 2;
    frames[0].mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}, {9, 202, 52}}};
    frames[1].mEvents.push_back(SpeedFixtureEvent(0xa1, 1, 0xb2));
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
    frames[2].mInputEffects.push_back(effect);
    frames[3].mGameSpeed = 4;
    frames[5].mEvents.push_back(SpeedFixtureEvent(0xc3, 5, 0xd4));
    frames[5].mRecordedGroups = RecordedControlGroupSnapshot{{{1, 101, 52}}};
    return frames;
}

bool SameSpeedFixtureFrame(const rareplay::replay::ReplayFrame& actual,
    const rareplay::replay::ReplayFrame& expected, std::uint32_t streamVersion)
{
    return actual.mFrameNumber == expected.mFrameNumber && actual.mGameSpeed == expected.mGameSpeed &&
        actual.mHasChecksum == expected.mHasChecksum && actual.mEvents == expected.mEvents &&
        actual.mRecordedGroups == expected.mRecordedGroups &&
        EncodeRecord(actual, streamVersion) == EncodeRecord(expected, streamVersion);
}

void VerifySpeedFixture(const std::filesystem::path& path, std::uint32_t containerVersion, std::uint32_t streamVersion,
    bool isUnfinalized, std::size_t frameCount)
{
    using namespace rareplay::replay;
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    std::string error;
    Require(reader.Open(path.wstring(), TargetHash(), metadata, error) && metadata == SPEED_FIXTURE_METADATA,
        "tracked speed fixture opens with its metadata");
    Require(reader.IsUnfinalized() == isUnfinalized &&
        reader.GetRecoveredFrameCount() == (isUnfinalized ? frameCount : 0) &&
        reader.GetContainerVersion() == containerVersion,
        "tracked speed fixture container kind, version and recovered frames match");
    ReplaySessionInfo info;
    const auto expectedInfo = SpeedFixtureInfo(streamVersion);
    Require(reader.GetSessionInfo(info, error) && info.mStreamVersion == streamVersion &&
        info.mGameMode == expectedInfo.mGameMode && info.mLocalHouseIndex == expectedInfo.mLocalHouseIndex &&
        info.mFrameSendRate == expectedInfo.mFrameSendRate && info.mRandomSeed == expectedInfo.mRandomSeed,
        "tracked speed fixture stream header matches");
    const auto frames = SpeedFixtureFrames();
    for (std::size_t index = 0; index < frameCount; ++index)
    {
        ReplayFrame frame;
        Require(reader.ReadFrame(frame, error) == ReplayReadResult::Frame &&
            SameSpeedFixtureFrame(frame, frames[index], streamVersion), "tracked speed fixture frame matches");
    }
    ReplayFrame end;
    Require(reader.ReadFrame(end, error) == ReplayReadResult::End && error.empty(),
        "tracked speed fixture ends normally");
}

void WriteFixtureFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    WriteBytes(path, bytes);
    Require(ReadBytes(path) == bytes, "fixture generation writes all bytes");
}

/** 两份字节除给定位置外逐字节相同；hashBegin == hashEnd 表示不排除内容哈希。 */
bool DifferOnlyInVersionAndHash(const std::vector<std::uint8_t>& left, const std::vector<std::uint8_t>& right,
    std::size_t hashBegin, std::size_t hashEnd)
{
    if (left.size() != right.size() || left[SPEED_FIXTURE_VERSION_OFFSET] == right[SPEED_FIXTURE_VERSION_OFFSET])
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (left[index] != right[index] && index != SPEED_FIXTURE_VERSION_OFFSET &&
            (index < hashBegin || index >= hashEnd))
            return false;
    }
    return true;
}

void TestFrozenV2Fixtures(const std::filesystem::path& fixtureDirectory)
{
    std::array<std::vector<std::uint8_t>, FROZEN_V2_FIXTURES.size()> files;
    for (std::size_t index = 0; index < FROZEN_V2_FIXTURES.size(); ++index)
    {
        const auto& fixture = FROZEN_V2_FIXTURES[index];
        const auto path = fixtureDirectory / fixture.mName;
        files[index] = ReadBytes(path);
        Require(files[index].size() == fixture.mFileBytes && FileSha256(files[index]) == fixture.mSha256,
            "the frozen v2 fixture matches its documented size and SHA-256");
        Require(LoadU32(files[index], 8) == 2 && files[index][SPEED_FIXTURE_VERSION_OFFSET] == fixture.mStreamVersion,
            "the frozen fixture is a container v2 file with the documented stream version");
        VerifySpeedFixture(path, 2, fixture.mStreamVersion, fixture.mIsUnfinalized,
            fixture.mIsUnfinalized ? SPEED_PARTIAL_COMPLETE_FRAMES : 6);
        ArchiveReader archive;
        ArchiveInfo info;
        std::string error;
        Require(archive.OpenByFlags(path.wstring(), info, error) && info.mContainerVersion == 2 &&
            info.mStreamBytes == fixture.mFileBytes - SPEED_FIXTURE_STREAM_OFFSET &&
            info.mStoredStreamBytes == info.mStreamBytes, "v2 logical and stored stream lengths are equal");
    }
    // v5 样例的帧内容与 v4 样例相同：完成文件只差帧流头版本与内容哈希（偏移 64..95），未完成录像只差版本。
    Require(DifferOnlyInVersionAndHash(files[0], files[2], 64, 96),
        "the v5 complete fixture differs from v4 only in the stream version and the content hash");
    Require(DifferOnlyInVersionAndHash(files[1], files[3], 0, 0),
        "the v5 partial fixture differs from v4 only in the stream version");
}

// 冻结的 v2 完成样例上的容器级损坏：读取端对 v2 的规则不变。
void TestFrozenV2Corruption(const std::filesystem::path& fixtureDirectory)
{
    TemporaryDirectory directory;
    const auto good = ReadBytes(fixtureDirectory / L"replay-v5-speed.yrrep");
    const auto reject = [&](const wchar_t* name, std::vector<std::uint8_t> bytes)
    {
        const auto badPath = directory.File(name);
        WriteBytes(badPath, bytes);
        Require(OpenFails(badPath), "corrupt v2 archive must fail");
    };
    auto truncated = good; truncated.pop_back(); reject(L"truncated.rep", truncated);
    auto trailing = good; trailing.push_back(0); reject(L"trailing.rep", trailing);
    auto checksum = good; checksum.back() ^= 1; reject(L"checksum.rep", checksum);
    auto unfinalized = good; unfinalized[20] = 0; reject(L"unfinalized.rep", unfinalized);
    auto zeroMetadata = good; zeroMetadata[16] = 0; reject(L"zero-metadata.rep", zeroMetadata);
    auto tooMuchMetadata = good; StoreU32(tooMuchMetadata, 16, 16 * 1024 * 1024 + 1);
    reject(L"oversize-metadata.rep", tooMuchMetadata);
    auto tooMuchStream = good; tooMuchStream[28] = 1; reject(L"oversize-stream.rep", tooMuchStream);
    auto shortStream = good; StoreU64(shortStream, 24, 31); reject(L"short-stream.rep", shortStream);
    for (const std::uint32_t version : {1u, 3u, 4u})
    {
        auto otherVersion = good;
        StoreU32(otherVersion, 8, version);
        reject(L"other-version.rep", otherVersion);
    }
}

/** 经正式 FrameWriter 与测试切块策略写出容器 v3 完成样例，返回全部字节。 */
std::vector<std::uint8_t> WriteBlockFixtureComplete(const TemporaryDirectory& directory)
{
    const auto path = directory.File(BLOCK_FIXTURE_COMPLETE);
    FrameWriter writer(BLOCK_FIXTURE_POLICY);
    std::string error;
    Require(writer.Open(path.wstring(), TargetHash(), SPEED_FIXTURE_METADATA,
        SpeedFixtureInfo(BLOCK_FIXTURE_STREAM_VERSION), error), "block fixture writer opens");
    for (const auto& frame : SpeedFixtureFrames())
        Require(writer.AppendFrame(frame, error), "block fixture frame appends");
    Require(writer.Finish(error) && writer.GetFrameCount() == 6, "block fixture finalizes");
    return ReadBytes(path);
}

/**
 * 写入帧 0–4 后 Abort（前两块已写出，待写块为空，容器头保持 flags=0），
 * 再追加完成样例第三块磁盘字节的前一半。
 */
std::vector<std::uint8_t> WriteBlockFixturePartial(const TemporaryDirectory& directory,
    const std::vector<std::uint8_t>& complete)
{
    const auto path = directory.File(BLOCK_FIXTURE_PARTIAL);
    const auto frames = SpeedFixtureFrames();
    FrameWriter writer(BLOCK_FIXTURE_POLICY);
    std::string error;
    Require(writer.Open(path.wstring(), TargetHash(), SPEED_FIXTURE_METADATA,
        SpeedFixtureInfo(BLOCK_FIXTURE_STREAM_VERSION), error), "partial block fixture writer opens");
    for (std::size_t index = 0; index < SPEED_PARTIAL_COMPLETE_FRAMES; ++index)
        Require(writer.AppendFrame(frames[index], error), "partial block fixture frame appends");
    Require(writer.GetLogicalStreamBytes() == BLOCK_FIXTURE_RAW_BYTES[0] + BLOCK_FIXTURE_RAW_BYTES[1],
        "frames 0-4 fill exactly the first two blocks");
    const auto storedBeforeAbort = writer.GetStoredStreamBytes();
    writer.Abort();
    auto bytes = ReadBytes(path.wstring() + L".partial");
    Require(bytes.size() == StreamSegmentStart(complete) + storedBeforeAbort,
        "the pending block is empty when frames 0-4 are aborted");
    const auto blocks = ParseStreamBlocks(complete);
    const auto third = blocks.at(2);
    const std::size_t thirdBytes = third.GetEnd() - third.mOffset;
    bytes.insert(bytes.end(), complete.begin() + static_cast<std::ptrdiff_t>(third.mOffset),
        complete.begin() + static_cast<std::ptrdiff_t>(third.mOffset + thirdBytes / 2));
    return bytes;
}

void PrintFixtureLayout(const wchar_t* name, const std::vector<std::uint8_t>& bytes)
{
    std::printf("%ls: %zu bytes, sha256 %s, container v%u flags=%u streamBytes=%llu\n", name, bytes.size(),
        FileSha256(bytes).c_str(), LoadU32(bytes, 8), LoadU32(bytes, 20),
        static_cast<unsigned long long>(LoadU64(bytes, 24)));
    std::printf("  metadata segment: offset 96, %u bytes\n", LoadU32(bytes, 16));
    std::size_t offset = StreamSegmentStart(bytes);
    unsigned int index = 0;
    while (bytes.size() - offset >= block::HEADER_BYTES && LoadU32(bytes, offset) == block::MAGIC &&
        offset + block::HEADER_BYTES + LoadU32(bytes, offset + 12) <= bytes.size())
    {
        std::printf("  block %u: offset %zu, method %u, raw %u, stored %u, crc32 %08x\n", index++, offset,
            LoadU32(bytes, offset + 4), LoadU32(bytes, offset + 8), LoadU32(bytes, offset + 12),
            LoadU32(bytes, offset + 16));
        offset += block::HEADER_BYTES + LoadU32(bytes, offset + 12);
    }
    if (offset != bytes.size())
        std::printf("  trailing bytes: offset %zu, %zu bytes\n", offset, bytes.size() - offset);
}

void TestBlockFixtures(const std::filesystem::path& fixtureDirectory, bool generate)
{
    TemporaryDirectory directory;
    const auto complete = WriteBlockFixtureComplete(directory);
    const auto partial = WriteBlockFixturePartial(directory, complete);

    // 结构：清单原样存放（5 字节），帧流 3 块，解压后依次为帧流头加帧 0–1、帧 2–4、帧 5。
    Require(LoadU32(complete, 8) == 3 && LoadU32(complete, 16) == block::HEADER_BYTES + 5 &&
        LoadU32(complete, 20) == 1, "block fixture header matches");
    const auto blocks = ParseStreamBlocks(complete);
    Require(blocks.size() == BLOCK_FIXTURE_RAW_BYTES.size(), "block fixture has three stream blocks");
    for (std::size_t index = 0; index < blocks.size(); ++index)
        Require(blocks[index].mRawBytes == BLOCK_FIXTURE_RAW_BYTES[index] && blocks[index].mMethod == 1,
            "each block fixture block is a deflate block of the documented raw size");
    // 未完成样例：头为 flags=0、streamBytes=0、内容哈希全零，其余字节与完成样例的前两块相同，再接第三块的前一半。
    const std::size_t partialEnd = blocks[1].GetEnd() + (blocks[2].GetEnd() - blocks[2].mOffset) / 2;
    Require(partial.size() == partialEnd && LoadU32(partial, 20) == 0 && LoadU64(partial, 24) == 0,
        "partial block fixture layout matches");
    for (std::size_t index = 0; index < partial.size(); ++index)
    {
        const bool isHeaderField = (index >= 20 && index < 32) || (index >= 64 && index < 96);
        Require(isHeaderField ? partial[index] == 0 : partial[index] == complete[index],
            "the partial block fixture is a prefix of the complete one apart from the finishing header fields");
    }

    const auto completePath = fixtureDirectory / BLOCK_FIXTURE_COMPLETE;
    const auto partialPath = fixtureDirectory / BLOCK_FIXTURE_PARTIAL;
    if (generate)
    {
        WriteFixtureFile(completePath, complete);
        WriteFixtureFile(partialPath, partial);
        PrintFixtureLayout(BLOCK_FIXTURE_COMPLETE, complete);
        PrintFixtureLayout(BLOCK_FIXTURE_PARTIAL, partial);
    }
    Require(ReadBytes(completePath) == complete, "C++ writer must reproduce the tracked complete block fixture");
    Require(ReadBytes(partialPath) == partial, "C++ writer must reproduce the tracked partial block fixture");
    VerifySpeedFixture(completePath, 3, BLOCK_FIXTURE_STREAM_VERSION, false, 6);
    VerifySpeedFixture(partialPath, 3, BLOCK_FIXTURE_STREAM_VERSION, true, SPEED_PARTIAL_COMPLETE_FRAMES);

    // 逻辑帧流与冻结的 v5 完成样例的帧流逐字节相同。
    const auto frozen = ReadBytes(fixtureDirectory / L"replay-v5-speed.yrrep");
    const std::vector<std::uint8_t> frozenStream(frozen.begin() + SPEED_FIXTURE_STREAM_OFFSET, frozen.end());
    Require(ReadLogicalStream(completePath) == frozenStream,
        "the block fixture's logical stream equals the frozen v5 fixture stream");
    ArchiveReader archive;
    ArchiveInfo info;
    std::string error;
    Require(archive.OpenByFlags(partialPath.wstring(), info, error) && !info.mIsFinalized &&
        info.mStreamBytes == BLOCK_FIXTURE_RAW_BYTES[0] + BLOCK_FIXTURE_RAW_BYTES[1] &&
        info.mStoredStreamBytes == blocks[1].GetEnd() - StreamSegmentStart(complete),
        "the partial block fixture recovers exactly the first two blocks");
}

/** 写入后经 FrameReader 打开必须失败（打开时发现的结构错误）。 */
void ExpectFrameReaderOpenFails(const TemporaryDirectory& directory, const wchar_t* name,
    const std::vector<std::uint8_t>& bytes, const char* message)
{
    const auto path = directory.File(name);
    WriteBytes(path, bytes);
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    std::string error;
    Require(!reader.Open(path.wstring(), TargetHash(), metadata, error) && !error.empty(), message);
}

/** 打开成功，前 failingFrame 帧正常，读第 failingFrame 帧时以带块错误原因的 Error 结束且保持失败。 */
void ExpectBlockErrorAtFrame(const TemporaryDirectory& directory, const wchar_t* name,
    const std::vector<std::uint8_t>& bytes, std::size_t failingFrame, const char* message)
{
    using namespace rareplay::replay;
    const auto path = directory.File(name);
    WriteBytes(path, bytes);
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    std::string error;
    Require(reader.Open(path.wstring(), TargetHash(), metadata, error) && !reader.IsUnfinalized(),
        "a SHA-valid archive with a damaged later block opens");
    for (std::size_t index = 0; index < failingFrame; ++index)
    {
        ReplayFrame frame;
        Require(reader.ReadFrame(frame, error) == ReplayReadResult::Frame && frame.mFrameNumber == index, message);
    }
    ReplayFrame frame;
    Require(reader.ReadFrame(frame, error) == ReplayReadResult::Error && error.find("block") != std::string::npos,
        message);
    Require(reader.ReadFrame(frame, error) == ReplayReadResult::Error, "the reader stays failed after a block error");
}

// v3 完成文件的结构错误：内容哈希按改动后的字节重算，只检验结构规则；在打开或播放时以 Error 结束，不得表现为 End。
void TestFinalizedBlockStructureErrors()
{
    TemporaryDirectory directory;
    const auto complete = WriteBlockFixtureComplete(directory);
    const auto blocks = ParseStreamBlocks(complete);
    Require(blocks.size() == 3 && blocks[2].mMethod == 1, "structure tests start from the three-block fixture");
    const std::size_t streamStart = StreamSegmentStart(complete);
    const auto rehashed = [](std::vector<std::uint8_t> bytes)
    {
        RehashArchive(bytes);
        return bytes;
    };
    const auto controlPath = directory.File(L"control.rep");
    WriteBytes(controlPath, complete);
    ExpectReadOutcome(controlPath, 6, -1, "the control file plays to a normal end");

    // 块头链与 streamBytes 不符：第一块的 storedBytes 改小 1，下一块头错位。
    {
        auto bytes = complete;
        const std::uint32_t stored = LoadU32(bytes, blocks[0].mOffset + 12);
        if (blocks[0].mMethod == 0)
            StoreU32(bytes, blocks[0].mOffset + 8, stored - 1);
        StoreU32(bytes, blocks[0].mOffset + 12, stored - 1);
        ExpectFrameReaderOpenFails(directory, L"chain.rep", rehashed(bytes), "a misaligned block chain is rejected");
    }
    // streamBytes 比块头链短：最后一块越过帧流段末尾（文件同步截短，长度与头一致）。
    {
        auto bytes = complete;
        bytes.pop_back();
        StoreU64(bytes, 24, bytes.size() - streamStart);
        ExpectFrameReaderOpenFails(directory, L"short-chain.rep", rehashed(bytes),
            "a last block extending past streamBytes is rejected");
    }
    // 帧流段尾部多余字节（计入 streamBytes）：不足一个块头或是无效块头都拒绝。
    for (const std::size_t extra : {std::size_t{5}, std::size_t{20}, std::size_t{64}})
    {
        auto bytes = complete;
        bytes.insert(bytes.end(), extra, 0);
        StoreU64(bytes, 24, bytes.size() - streamStart);
        ExpectFrameReaderOpenFails(directory, L"trailing-segment.rep", rehashed(bytes),
            "trailing bytes inside the stream segment are rejected");
    }
    // 第二块损坏（块边界恰好落在帧 1 与帧 2 之间）：打开时块头链有效，读到帧 2 时必须是 Error 而不是 End。
    {
        auto badCrc = complete;
        badCrc[blocks[1].mOffset + 16] ^= 1;
        ExpectBlockErrorAtFrame(directory, L"second-crc.rep", rehashed(badCrc), 2,
            "a CRC mismatch in the second block fails playback at frame 2");
        auto badPayload = complete;
        badPayload[blocks[1].mOffset + block::HEADER_BYTES + blocks[1].mStoredBytes / 2] ^= 0x10;
        ExpectBlockErrorAtFrame(directory, L"second-payload.rep", rehashed(badPayload), 2,
            "a damaged second block payload fails playback at frame 2");

        // 容器层：错误由 GetStreamError 报告并保持，回到开头也不清除。
        const auto path = directory.File(L"second-crc-archive.rep");
        WriteBytes(path, rehashed(badCrc));
        ArchiveReader archive;
        ArchiveInfo info;
        std::string error;
        Require(archive.Open(path.wstring(), info, error) && archive.GetStreamError().empty(),
            "the archive opens before any block is decoded");
        std::vector<std::uint8_t> stream(static_cast<std::size_t>(info.mStreamBytes));
        Require(archive.Read(stream.data(), static_cast<int>(stream.size())) ==
                static_cast<int>(BLOCK_FIXTURE_RAW_BYTES[0]) && !archive.GetStreamError().empty(),
            "reading stops at the damaged block and reports why");
        Require(archive.Rewind() && archive.Read(stream.data(), 4) == 0 && !archive.GetStreamError().empty(),
            "a stream error is sticky");
    }
    // 最后一块的 rawBytes 比实际解出的多 1：帧 0–4 正常，帧 5 所在块解码不足，Error。
    {
        auto bytes = complete;
        StoreU32(bytes, blocks[2].mOffset + 8, blocks[2].mRawBytes + 1);
        ExpectBlockErrorAtFrame(directory, L"third-raw.rep", rehashed(bytes), 5,
            "a block decoding to fewer bytes than recorded fails playback");
    }
    // 第一块损坏：帧流头无法读出，打开即失败并报告块错误。
    {
        auto bytes = complete;
        bytes[blocks[0].mOffset + 16] ^= 1;
        const auto path = directory.File(L"first-crc.rep");
        WriteBytes(path, rehashed(bytes));
        FrameReader reader;
        std::vector<std::uint8_t> metadata;
        std::string error;
        Require(!reader.Open(path.wstring(), TargetHash(), metadata, error) && error.find("block") != std::string::npos,
            "a damaged first block fails the open with the block error");
    }
    // 清单块损坏：打开即失败。
    {
        auto bytes = complete;
        bytes[CONTAINER_HEADER_BYTES + 16] ^= 1;
        ExpectFrameReaderOpenFails(directory, L"metadata-crc.rep", rehashed(bytes), "a damaged metadata block is rejected");
        auto shortMetadata = complete;
        StoreU32(shortMetadata, CONTAINER_HEADER_BYTES + 12, LoadU32(complete, CONTAINER_HEADER_BYTES + 12) - 1);
        StoreU32(shortMetadata, CONTAINER_HEADER_BYTES + 8, LoadU32(complete, CONTAINER_HEADER_BYTES + 8) - 1);
        ExpectFrameReaderOpenFails(directory, L"metadata-size.rep", rehashed(shortMetadata),
            "a metadata block that does not fill metadataBytes is rejected");
    }
    // 同样的第二块损坏在未完成录像中是块级截止：只恢复第一块中的帧 0–1。
    {
        auto bytes = complete;
        bytes[blocks[1].mOffset + 16] ^= 1;
        StoreU32(bytes, 20, 0);
        StoreU64(bytes, 24, 0);
        std::fill(bytes.begin() + 64, bytes.begin() + 96, std::uint8_t{0});
        const auto path = directory.File(L"second-crc.yrrep.partial");
        WriteBytes(path, bytes);
        FrameReader reader;
        std::vector<std::uint8_t> metadata;
        std::string error;
        Require(reader.Open(path.wstring(), TargetHash(), metadata, error) && reader.IsUnfinalized() &&
            reader.GetRecoveredFrameCount() == 2, "an unfinalized file stops before the damaged block");
    }
}

// C# 转换样例（冻结）：DTAClient 测试程序 `--generate-conversion-fixtures <目录>` 用“压缩旧回放”的转换器
// （测试切块策略每块最多 3 个单元）把 v2 源样例转成 v3。字节取决于 .NET 的 deflate 实现，常规测试不重新生成，
// 只核对字节数、SHA-256 与块布局，并证明插件能读取 C# 写出的 v3。
constexpr const wchar_t* CONVERSION_SOURCE = L"replay-conversion-source-v2.yrrep";
constexpr const wchar_t* CONVERSION_RESULT = L"replay-conversion-dotnet-v3.yrrep";
constexpr std::size_t CONVERSION_SOURCE_BYTES = 803;
constexpr std::size_t CONVERSION_RESULT_BYTES = 500;
constexpr const char* CONVERSION_SOURCE_SHA256 = "89d0e8b528bd0f14376c333643140809be5cc79ca54b6b34e32cb67b1a852ad6";
constexpr const char* CONVERSION_RESULT_SHA256 = "cc9732ca868a3f1e6b0a4cf03ce1706f383e4bd585725df9c9e94ba188ff514e";
// 源样例的清单是 225 字节的有效 RPLM 清单，帧流取自 replay-v5-speed.yrrep（482 字节）。
constexpr std::uint32_t CONVERSION_METADATA_BYTES = 225;
constexpr std::uint64_t CONVERSION_STREAM_BYTES = 482;

struct ExpectedBlock
{
    std::size_t mOffset;
    std::uint32_t mMethod;
    std::uint32_t mRawBytes;
    std::uint32_t mStoredBytes;
    std::uint32_t mCrc32;
};
constexpr ExpectedBlock CONVERSION_METADATA_BLOCK = {96, 1, 225, 199, 0xEEE5CE3Au};
constexpr std::array<ExpectedBlock, 3> CONVERSION_STREAM_BLOCKS = {{
    {315, 1, 219, 55, 0x5EEAAB7Au},
    {390, 1, 116, 44, 0x223FA013u},
    {454, 1, 147, 26, 0x467749F0u},
}};

bool MatchesExpectedBlock(const std::vector<std::uint8_t>& bytes, const BlockLayout& actual, const ExpectedBlock& expected)
{
    return actual.mOffset == expected.mOffset && actual.mMethod == expected.mMethod &&
        actual.mRawBytes == expected.mRawBytes && actual.mStoredBytes == expected.mStoredBytes &&
        LoadU32(bytes, actual.mOffset + 16) == expected.mCrc32;
}

/** 以文件头中的 gamemd.exe SHA-256 打开完成文件并逐帧解码到正常 End，返回解出的帧。 */
std::vector<rareplay::replay::ReplayFrame> DecodeAllFrames(const std::filesystem::path& path,
    const std::vector<std::uint8_t>& fileBytes, std::uint32_t expectedContainerVersion)
{
    using namespace rareplay::replay;
    std::array<std::uint8_t, 32> targetHash{};
    std::copy(fileBytes.begin() + 32, fileBytes.begin() + 64, targetHash.begin());
    FrameReader reader;
    std::vector<std::uint8_t> metadata;
    std::string error;
    Require(reader.Open(path.wstring(), targetHash, metadata, error) && !reader.IsUnfinalized() &&
        reader.GetContainerVersion() == expectedContainerVersion, "the conversion fixture opens with FrameReader");
    std::vector<ReplayFrame> frames;
    for (;;)
    {
        ReplayFrame frame;
        const auto result = reader.ReadFrame(frame, error);
        if (result == ReplayReadResult::End)
            break;
        Require(result == ReplayReadResult::Frame && frame.mFrameNumber == frames.size(),
            "every conversion fixture frame decodes in order");
        frames.push_back(std::move(frame));
    }
    Require(error.empty(), "the conversion fixture ends with a normal EOF");
    return frames;
}

void TestDotNetConversionFixtures(const std::filesystem::path& fixtureDirectory)
{
    const auto sourcePath = fixtureDirectory / CONVERSION_SOURCE;
    const auto resultPath = fixtureDirectory / CONVERSION_RESULT;
    const auto source = ReadBytes(sourcePath);
    const auto result = ReadBytes(resultPath);
    Require(source.size() == CONVERSION_SOURCE_BYTES && FileSha256(source) == CONVERSION_SOURCE_SHA256,
        "the frozen conversion source matches its documented size and SHA-256");
    Require(result.size() == CONVERSION_RESULT_BYTES && FileSha256(result) == CONVERSION_RESULT_SHA256,
        "the frozen .NET conversion result matches its documented size and SHA-256");

    // 块布局：清单段恰为一块，帧流段 3 块首尾相接正好结束在文件尾。
    Require(LoadU32(result, 16) == CONVERSION_METADATA_BLOCK.mStoredBytes + block::HEADER_BYTES,
        "the converted metadata segment is exactly one block");
    const auto metadataBlocks = ParseBlocks(result, CONTAINER_HEADER_BYTES, StreamSegmentStart(result),
        block::METADATA_RAW_LIMIT);
    Require(metadataBlocks.size() == 1 && MatchesExpectedBlock(result, metadataBlocks[0], CONVERSION_METADATA_BLOCK),
        "the converted metadata block matches the documented layout");
    const auto streamBlocks = ParseStreamBlocks(result);
    Require(streamBlocks.size() == CONVERSION_STREAM_BLOCKS.size(), "the converted stream has three blocks");
    for (std::size_t index = 0; index < streamBlocks.size(); ++index)
        Require(MatchesExpectedBlock(result, streamBlocks[index], CONVERSION_STREAM_BLOCKS[index]),
            "each converted stream block matches the documented layout");

    // 容器层：v2 与 v3 都是完成文件，gamemd.exe SHA-256 相同，解压后的清单与逻辑帧流逐字节相同。
    ArchiveReader sourceArchive;
    ArchiveReader resultArchive;
    ArchiveInfo sourceInfo;
    ArchiveInfo resultInfo;
    std::string error;
    Require(sourceArchive.Open(sourcePath.wstring(), sourceInfo, error) && sourceInfo.mContainerVersion == 2 &&
        sourceInfo.mIsFinalized && sourceInfo.mMetadata.size() == CONVERSION_METADATA_BYTES &&
        sourceInfo.mStreamBytes == CONVERSION_STREAM_BYTES, "the conversion source is a finalized container v2 file");
    Require(resultArchive.Open(resultPath.wstring(), resultInfo, error) && resultInfo.mContainerVersion == 3 &&
        resultInfo.mIsFinalized && resultInfo.mStreamBytes == CONVERSION_STREAM_BYTES &&
        resultInfo.mStoredStreamBytes == LoadU64(result, 24), "the conversion result is a finalized container v3 file");
    Require(sourceInfo.mTargetHash == resultInfo.mTargetHash, "conversion keeps the gamemd.exe SHA-256");
    Require(sourceInfo.mMetadata == resultInfo.mMetadata &&
        std::equal(sourceInfo.mMetadata.begin(), sourceInfo.mMetadata.end(), source.begin() + CONTAINER_HEADER_BYTES),
        "conversion keeps the original metadata bytes");
    sourceArchive.Close();
    resultArchive.Close();
    const auto sourceStream = ReadLogicalStream(sourcePath);
    Require(sourceStream.size() == CONVERSION_STREAM_BYTES && ReadLogicalStream(resultPath) == sourceStream,
        "conversion keeps the logical frame stream byte for byte");

    // 帧层：转换结果逐帧解码到正常 End，帧数与源文件相同，内容与 v5 样例的帧 0–5 相同。
    const auto sourceFrames = DecodeAllFrames(sourcePath, source, 2);
    const auto resultFrames = DecodeAllFrames(resultPath, result, 3);
    const auto expectedFrames = SpeedFixtureFrames();
    Require(sourceFrames.size() == expectedFrames.size() && resultFrames.size() == sourceFrames.size(),
        "the converted file decodes the same number of frames as its source");
    for (std::size_t index = 0; index < resultFrames.size(); ++index)
        Require(SameSpeedFixtureFrame(resultFrames[index], sourceFrames[index], BLOCK_FIXTURE_STREAM_VERSION) &&
            SameSpeedFixtureFrame(resultFrames[index], expectedFrames[index], BLOCK_FIXTURE_STREAM_VERSION),
            "each converted frame equals the source frame");
}
}

int main(int argc, char** argv)
{
    const bool generate = argc == 3 && std::string(argv[1]) == "--generate";
    Require(argc == 2 || generate, "supply fixture directory or --generate directory");
    const std::filesystem::path fixtureDirectory(argv[generate ? 2 : 1]);
    TestRoundTripAndHeader();
    TestNoOverwriteAndIncomplete();
    TestCorruptionAndBounds();
    TestFrameArchiveRoundTrip();
    TestReaderRejectsValidContainerWithSkippedFrame();
    TestLegacyFrameArchiveCanBeInspected();
    TestGroupFrameArchiveVersionAndInitialSnapshot();
    TestGameSpeedArchiveRules();
    TestVersion5ArchiveRoundTrip();
    TestWriterBlockPolicy();
    TestFinalizedBlockStructureErrors();
    TestFrozenV2Fixtures(fixtureDirectory);
    TestFrozenV2Corruption(fixtureDirectory);
    TestBlockFixtures(fixtureDirectory, generate);
    TestDotNetConversionFixtures(fixtureDirectory);
}
