#include "FrameReader.h"

#include "../replay/FrameCodec.h"

#include <utility>

namespace rareplay::storage
{
bool FrameReader::Open(const std::wstring& path,
    const std::array<std::uint8_t, 32>& expectedTargetHash,
    std::vector<std::uint8_t>& outMetadata, std::string& error)
{
    Close();
    ArchiveInfo info;
    if (!mArchive.Open(path, info, error))
    {
        return false;
    }
    if (info.mTargetHash != expectedTargetHash)
    {
        error = "Replay target executable hash mismatch";
        Close();
        return false;
    }
    if (!replay::FrameCodec::DecodeHeader(mArchive.GetStream(), mInfo, error))
    {
        Close();
        return false;
    }
    mNextFrame = mInfo.mFirstFrame;
    mIsOpen = true;
    outMetadata = std::move(info.mMetadata);
    error.clear();
    return true;
}

bool FrameReader::GetSessionInfo(replay::ReplaySessionInfo& outInfo, std::string& error)
{
    if (!mIsOpen || mHasFailed)
    {
        error = "Replay reader is not open";
        return false;
    }
    outInfo = mInfo;
    error.clear();
    return true;
}

replay::ReplayReadResult FrameReader::ReadFrame(replay::ReplayFrame& outFrame,
    std::string& error)
{
    if (!mIsOpen || mHasFailed)
    {
        error = "Replay reader is not open";
        return replay::ReplayReadResult::Error;
    }
    replay::ReplayFrame frame;
    const replay::ReplayReadResult result =
        replay::FrameCodec::ReadFrame(mArchive.GetStream(), frame, error, mInfo.mStreamVersion);
    if (result == replay::ReplayReadResult::Error)
    {
        mHasFailed = true;
        return result;
    }
    if (result == replay::ReplayReadResult::End)
    {
        if (mInfo.mStreamVersion == 3 && mNextFrame == 0)
        {
            error = "编队帧流缺少初始完整快照";
            mHasFailed = true;
            return replay::ReplayReadResult::Error;
        }
        return result;
    }
    if (mNextFrame > UINT32_MAX || frame.mFrameNumber != mNextFrame)
    {
        error = "Replay frame sequence is not contiguous";
        mHasFailed = true;
        return replay::ReplayReadResult::Error;
    }
    ++mNextFrame;
    outFrame = std::move(frame);
    error.clear();
    return replay::ReplayReadResult::Frame;
}

void FrameReader::Close()
{
    mArchive.Close();
    mInfo = {};
    mNextFrame = 0;
    mIsOpen = false;
    mHasFailed = false;
}
}
