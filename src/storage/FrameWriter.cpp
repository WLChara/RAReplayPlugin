#include "FrameWriter.h"

#include "../replay/FrameCodec.h"

namespace rareplay::storage
{
FrameWriter::FrameWriter(const ArchiveBlockPolicy& policy) : mArchive(policy) {}

bool FrameWriter::Open(const std::wstring& path,
    const std::array<std::uint8_t, 32>& targetHash,
    const std::vector<std::uint8_t>& metadata,
    const replay::ReplaySessionInfo& sessionInfo, std::string& error)
{
    Abort();
    std::array<std::uint8_t, replay::FrameCodec::HEADER_BYTES> header{};
    if (!replay::FrameCodec::EncodeHeader(sessionInfo, header, error) ||
        !mArchive.Open(path, targetHash, metadata, error))
    {
        return false;
    }
    if (mArchive.Write(header.data(), static_cast<int>(header.size())) !=
        static_cast<int>(header.size()))
    {
        error = "Unable to write replay stream header";
        mHasFailed = true;
        return false;
    }
    mFirstFrame = sessionInfo.mFirstFrame;
    mNextFrame = sessionInfo.mFirstFrame;
    mStreamVersion = sessionInfo.mStreamVersion;
    mGameMode = sessionInfo.mGameMode;
    mIsOpen = true;
    error.clear();
    return true;
}

bool FrameWriter::AppendFrame(const replay::ReplayFrame& frame, std::string& error)
{
    if (!mIsOpen || mHasFailed || mNextFrame > UINT32_MAX ||
        frame.mFrameNumber != mNextFrame)
    {
        error = "Replay writer state or frame sequence is invalid";
        mHasFailed = true;
        return false;
    }
    std::vector<std::uint8_t> bytes;
    // 与读取端相同的帧流级速度规则：写入端不产生读取端会拒绝的记录。
    if (!replay::FrameCodec::ValidateGameSpeed(frame, mStreamVersion, mGameMode, error) ||
        !replay::FrameCodec::EncodeFrame(frame, bytes, error, mStreamVersion) ||
        mArchive.Write(bytes.data(), static_cast<int>(bytes.size())) !=
            static_cast<int>(bytes.size()))
    {
        if (error.empty())
        {
            error = "Unable to append replay frame";
        }
        mHasFailed = true;
        return false;
    }
    ++mNextFrame;
    error.clear();
    return true;
}

bool FrameWriter::Finish(std::string& error)
{
    if (!mIsOpen || mHasFailed || (mStreamVersion >= 3 && mNextFrame == 0))
    {
        error = "Replay writer is not finishable";
        return false;
    }
    if (!mArchive.Finish(error))
    {
        mHasFailed = true;
        return false;
    }
    mIsOpen = false;
    error.clear();
    return true;
}

void FrameWriter::Abort() noexcept
{
    mArchive.Abort();
    mFirstFrame = 0;
    mNextFrame = 0;
    mStreamVersion = 2;
    mGameMode = 0;
    mIsOpen = false;
    mHasFailed = false;
}

std::uint64_t FrameWriter::GetLogicalStreamBytes() const
{
    return static_cast<std::uint64_t>(mArchive.GetSize());
}

std::uint64_t FrameWriter::GetStoredStreamBytes() const
{
    return static_cast<std::uint64_t>(mArchive.GetStoredSize());
}
}
