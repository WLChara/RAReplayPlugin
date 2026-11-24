#include "../src/storage/ReplayConfig.h"
#include "../src/replay/FrameCodec.h"
#include "../src/storage/FrameReader.h"
#include "../src/storage/FrameWriter.h"
#include "../src/storage/ReplayBlockCodec.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
// 未完成录像样本（容器 v3）：写入 3 帧后中止（待写块写成一块），再追加下一块磁盘字节的前 10 字节。
constexpr std::uint32_t UNFINALIZED_FRAME_COUNT = 3;
constexpr std::size_t UNFINALIZED_TAIL_BYTES = 10;

bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& outBytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }
    outBytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

rareplay::replay::ReplayFrame MakeEventFrame()
{
    rareplay::replay::ReplayFrame eventFrame;
    eventFrame.mFrameNumber = 1;
    eventFrame.mChecksum = 0xdeadbeef;
    eventFrame.mHasChecksum = true;
    rareplay::replay::RecordedEvent event{};
    for (std::size_t index = 0; index < event.size(); ++index)
    {
        event[index] = static_cast<std::uint8_t>(index);
    }
    eventFrame.mEvents.push_back(event);
    rareplay::replay::RecordedInputEffect effect;
    effect.mAnimationTypeIndex = 17;
    effect.mX = -23;
    effect.mY = 45;
    effect.mZ = -67;
    effect.mLoopDelay = 8;
    effect.mLoopCount = 2;
    effect.mFlags = 0x12345678;
    effect.mForceZAdjust = -9;
    effect.mReverse = true;
    eventFrame.mInputEffects.push_back(effect);
    return eventFrame;
}

/**
 * 写出 bridge-unfinalized.yrrep.partial：FrameWriter 写帧后 Abort（不 Finish），待写块写成帧流段的一块，
 * 容器头保持 flags=0、streamBytes=0、内容哈希全零；再把下一帧真实编码成下一块，追加其磁盘字节的前 10 字节
 * （不足 20 字节块头，按块级截止忽略）。
 */
bool WriteUnfinalized(const std::filesystem::path& directory,
    const std::array<std::uint8_t, 32>& targetHash, const std::vector<std::uint8_t>& manifest,
    const rareplay::replay::ReplaySessionInfo& sessionInfo, std::string& error)
{
    const auto finalPath = directory / L"bridge-unfinalized.yrrep";
    rareplay::storage::FrameWriter writer;
    if (!writer.Open(finalPath.wstring(), targetHash, manifest, sessionInfo, error))
    {
        return false;
    }
    rareplay::replay::ReplayFrame empty;
    rareplay::replay::ReplayFrame third;
    third.mFrameNumber = 2;
    third.mChecksum = 0x0badf00d;
    third.mHasChecksum = true;
    if (!writer.AppendFrame(empty, error) || !writer.AppendFrame(MakeEventFrame(), error) ||
        !writer.AppendFrame(third, error))
    {
        writer.Abort();
        return false;
    }
    writer.Abort();

    rareplay::replay::ReplayFrame next;
    next.mFrameNumber = UNFINALIZED_FRAME_COUNT;
    next.mChecksum = 0x0c0ffee0;
    next.mHasChecksum = true;
    std::vector<std::uint8_t> nextRecord;
    std::vector<std::uint8_t> nextBlock;
    if (!rareplay::replay::FrameCodec::EncodeFrame(next, nextRecord, error,
            sessionInfo.mStreamVersion) ||
        !rareplay::storage::block::EncodeBlock(nextRecord.data(), nextRecord.size(), nextBlock, error) ||
        nextBlock.size() < UNFINALIZED_TAIL_BYTES)
    {
        return false;
    }
    const auto partialPath = std::filesystem::path(finalPath.wstring() + L".partial");
    {
        std::ofstream output(partialPath, std::ios::binary | std::ios::app);
        output.write(reinterpret_cast<const char*>(nextBlock.data()),
            static_cast<std::streamsize>(UNFINALIZED_TAIL_BYTES));
        if (!output)
        {
            error = "unable to append the truncated block prefix";
            return false;
        }
    }

    // 先用 C++ 自己的读取端确认样本确实是可恢复的未完成录像，再交给 C# 核对。
    rareplay::storage::FrameReader reader;
    std::vector<std::uint8_t> metadata;
    if (!reader.Open(partialPath.wstring(), targetHash, metadata, error) ||
        !reader.IsUnfinalized() || reader.GetRecoveredFrameCount() != UNFINALIZED_FRAME_COUNT ||
        reader.GetContainerVersion() != 3 || metadata != manifest)
    {
        if (error.empty())
        {
            error = "unfinalized sample was not recovered by the C++ reader";
        }
        return false;
    }
    return true;
}

bool Run(const std::filesystem::path& directory, std::string& error)
{
    rareplay::storage::ReplayRequest request;
    std::vector<std::uint8_t> rawRequest;
    if (!rareplay::storage::ReadReplayRequest(directory / L"request.bin", request,
        rawRequest, error))
    {
        return false;
    }
    std::vector<std::uint8_t> expectedManifest;
    if (!ReadFile(directory / L"manifest.bin", expectedManifest) ||
        request.mMode != rareplay::storage::ReplayMode::Record ||
        request.mReplayPath != directory / L"bridge.yrrep" ||
        request.mManifest != expectedManifest)
    {
        error = "C# request or opaque manifest did not survive C++ parsing";
        return false;
    }
    if (request.mRequestId == std::array<std::uint8_t, 16>{})
    {
        error = "C# request ID is empty";
        return false;
    }

    std::array<std::uint8_t, 32> targetHash{};
    for (std::size_t index = 0; index < targetHash.size(); ++index)
    {
        targetHash[index] = static_cast<std::uint8_t>(index + 1);
    }
    rareplay::replay::ReplaySessionInfo sessionInfo{3, 1, 0, 6, 0x12345678};
    rareplay::storage::FrameWriter writer;
    if (!writer.Open(request.mReplayPath.wstring(), targetHash, request.mManifest,
        sessionInfo, error))
    {
        return false;
    }
    rareplay::replay::ReplayFrame empty;
    if (!writer.AppendFrame(empty, error))
    {
        writer.Abort();
        return false;
    }
    if (!writer.AppendFrame(MakeEventFrame(), error) || !writer.Finish(error))
    {
        writer.Abort();
        return false;
    }
    return WriteUnfinalized(directory, targetHash, request.mManifest, sessionInfo, error);
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 2)
    {
        std::cerr << "usage: ProtocolBridge <temporary directory>\n";
        return 2;
    }
    std::string error;
    if (!Run(argv[1], error))
    {
        std::cerr << "protocol bridge failed: " << error << '\n';
        return 1;
    }
    std::cout << "C++ request parsing, frame writing and unfinalized sample passed\n";
    return 0;
}
