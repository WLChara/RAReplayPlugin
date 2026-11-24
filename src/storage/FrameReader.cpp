#include "FrameReader.h"

#include "../replay/FrameCodec.h"

#include <cstdio>
#include <utility>

namespace rareplay::storage
{
bool FrameReader::Open(const std::wstring& path,
    const std::array<std::uint8_t, 32>& expectedTargetHash,
    std::vector<std::uint8_t>& outMetadata, std::string& error)
{
    Close();
    ArchiveInfo info;
    if (!mArchive.OpenByFlags(path, info, error))
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
        // 帧流头所在的块无法解码或读文件失败时，报告真实原因而不是“帧流头不完整”。
        const std::string streamError = mArchive.GetStreamError();
        if (!streamError.empty())
        {
            error = streamError;
        }
        Close();
        return false;
    }
    mContainerVersion = info.mContainerVersion;
    mLogicalStreamBytes = info.mStreamBytes;
    mStoredStreamBytes = info.mStoredStreamBytes;
    mNextFrame = mInfo.mFirstFrame;
    mIsUnfinalized = !info.mIsFinalized;
    if (mIsUnfinalized && !ScanRecoverableFrames(error))
    {
        Close();
        return false;
    }
    mIsOpen = true;
    outMetadata = std::move(info.mMetadata);
    error.clear();
    return true;
}

bool FrameReader::ScanRecoverableFrames(std::string& error)
{
    // 逐帧解码复用 FrameCodec 的有效性规则与按帧流头校验的游戏速度规则，与完成文件逐帧解码一致；
    // 遇到不完整、无效或不连续的记录即停止，具体原因不影响恢复结果，只有"没有任何完整帧"才视为失败。
    std::uint64_t nextFrame = mInfo.mFirstFrame;
    while (nextFrame <= UINT32_MAX)
    {
        replay::ReplayFrame frame;
        std::string ignoredError;
        if (replay::FrameCodec::ReadFrame(mArchive.GetStream(), frame, ignoredError,
                mInfo.mStreamVersion) != replay::ReplayReadResult::Frame ||
            frame.mFrameNumber != nextFrame ||
            !replay::FrameCodec::ValidateGameSpeed(frame, mInfo.mStreamVersion, mInfo.mGameMode,
                ignoredError))
        {
            break;
        }
        ++nextFrame;
    }
    // 截止点只能由帧内容决定；读文件或解码失败而提前停止时整份文件按失败处理，不能少恢复几帧了事。
    const std::string streamError = mArchive.GetStreamError();
    if (!streamError.empty())
    {
        error = streamError;
        return false;
    }
    if (nextFrame == mInfo.mFirstFrame)
    {
        error = "Unfinalized replay has no complete valid frame";
        return false;
    }
    // 帧流只有 1 GiB 上限，按最小帧长不可能超过 uint32 帧号范围。
    mRecoveredFrameCount = static_cast<std::uint32_t>(nextFrame - mInfo.mFirstFrame);
    // 回到帧流头之后，让 ReadFrame 从帧 0 起重新读取已确认有效的部分。
    if (!mArchive.Seek(static_cast<int>(replay::FrameCodec::HEADER_BYTES), SEEK_SET))
    {
        error = "Unable to rewind the unfinalized replay stream";
        return false;
    }
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
    // 未完成录像在可恢复部分末尾即为正常 EOF，不解码其后的任何字节。
    if (mIsUnfinalized &&
        mNextFrame >= static_cast<std::uint64_t>(mInfo.mFirstFrame) + mRecoveredFrameCount)
    {
        error.clear();
        return replay::ReplayReadResult::End;
    }
    replay::ReplayFrame frame;
    const replay::ReplayReadResult result =
        replay::FrameCodec::ReadFrame(mArchive.GetStream(), frame, error, mInfo.mStreamVersion);
    // 逻辑流因块解码失败、CRC 不符或读文件失败而停止时，FrameCodec 看到的是 EOF 或短读；
    // 即使块边界恰好落在帧边界上（FrameCodec 返回 End），也必须按 Error 报告真实原因。
    if (result != replay::ReplayReadResult::Frame)
    {
        const std::string streamError = mArchive.GetStreamError();
        if (!streamError.empty())
        {
            error = streamError;
            mHasFailed = true;
            return replay::ReplayReadResult::Error;
        }
    }
    if (result == replay::ReplayReadResult::Error)
    {
        mHasFailed = true;
        return result;
    }
    if (result == replay::ReplayReadResult::End)
    {
        if (mInfo.mStreamVersion >= 3 && mNextFrame == 0)
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
    if (!replay::FrameCodec::ValidateGameSpeed(frame, mInfo.mStreamVersion, mInfo.mGameMode, error))
    {
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
    mLogicalStreamBytes = 0;
    mStoredStreamBytes = 0;
    mContainerVersion = 0;
    mRecoveredFrameCount = 0;
    mIsOpen = false;
    mIsUnfinalized = false;
    mHasFailed = false;
}
}
