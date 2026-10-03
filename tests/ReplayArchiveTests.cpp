#include "../src/storage/ReplayArchive.h"
#include "../src/storage/FrameReader.h"
#include "../src/storage/FrameWriter.h"
#include "../src/replay/FrameCodec.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <array>
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
    Require(bytes.size() == 96 + 3 + 32 &&
        std::equal(magic.begin(), magic.end(), bytes.begin()) &&
        bytes[8] == 2 && bytes[12] == 96 && bytes[16] == 3 &&
        bytes[20] == 1 && bytes[24] == 32,
        "container header must match the V2 byte contract");
    ArchiveReader reader;
    ArchiveInfo info;
    std::string error;
    Require(reader.Open(path.wstring(), info, error), "final archive should reopen");
    Require(info.mTargetHash == TargetHash() &&
        info.mMetadata == std::vector<std::uint8_t>({0x61,0x00,0xff}) &&
        info.mStreamBytes == 32 && reader.GetSize() == 32,
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
    const auto bytes = ReadBytes(path);
    Require(bytes[96 + 1 + 4] == 3 && bytes.size() == 96 + 1 + 32 + 4 * 24 + 2 * 12,
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
}

int main()
{
    TestRoundTripAndHeader();
    TestNoOverwriteAndIncomplete();
    TestCorruptionAndBounds();
    TestFrameArchiveRoundTrip();
    TestReaderRejectsValidContainerWithSkippedFrame();
    TestLegacyFrameArchiveCanBeInspected();
    TestGroupFrameArchiveVersionAndInitialSnapshot();
}
