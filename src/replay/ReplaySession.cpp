#include "ReplaySession.h"

#include "FrameCodec.h"

#include <utility>

namespace rareplay::replay
{
ReplaySession::ReplaySession(IReplayGame& game) : mGame(game) {}

bool ReplaySession::StartRecording(IReplaySink& sink)
{
    if (mState != ReplayState::Inactive)
    {
        mLastError = "Replay session is already active";
        return false;
    }
    mSink = &sink;
    ReplaySessionInfo info;
    if (!mGame.ReadSessionInfo(info, mLastError))
    {
        return FailRecording(mLastError);
    }
    if (info.mFirstFrame != 0)
    {
        return FailRecording("First replay frame must be zero");
    }
    if (info.mStreamVersion != 2 && info.mStreamVersion != 3)
        return FailRecording("不支持此录制帧流版本");
    mStreamVersion = info.mStreamVersion;
    mCurrentGroups.reset();
    mProcessedGroupUpdate.reset();
    mNextFrame = info.mFirstFrame;
    mState = ReplayState::Recording;
    mLastError.clear();
    return true;
}

bool ReplaySession::StartPlayback(IReplaySource& source)
{
    if (mState != ReplayState::Inactive)
    {
        mLastError = "Replay session is already active";
        return false;
    }
    mWasPlayback = true;
    ReplaySessionInfo recorded;
    ReplaySessionInfo runtime;
    if (!source.GetSessionInfo(recorded, mLastError) ||
        !mGame.ReadSessionInfo(runtime, mLastError))
    {
        return FailPlayback(mLastError);
    }
    if (recorded.mStreamVersion < 2)
    {
        return FailPlayback("旧录影缺少玩家输入效果，请重新录制");
    }
    if (recorded.mStreamVersion > 3)
        return FailPlayback("不支持此播放帧流版本");
    if (recorded.mFirstFrame != 0 ||
        recorded.mGameMode != runtime.mGameMode ||
        recorded.mLocalHouseIndex != runtime.mLocalHouseIndex ||
        recorded.mFirstFrame != runtime.mFirstFrame ||
        recorded.mFrameSendRate != runtime.mFrameSendRate ||
        recorded.mRandomSeed != runtime.mRandomSeed)
    {
        std::string error = "Replay session metadata differs from restored game:";
        const auto appendDifference = [&error](const char* field, std::uint32_t recordedValue,
            std::uint32_t restoredValue)
        {
            error += " ";
            error += field;
            error += "(recorded=" + std::to_string(recordedValue) +
                ", restored=" + std::to_string(restoredValue) + ")";
        };
        if (recorded.mGameMode != runtime.mGameMode)
            appendDifference("mode", recorded.mGameMode, runtime.mGameMode);
        if (recorded.mLocalHouseIndex != runtime.mLocalHouseIndex)
            appendDifference("localHouse", recorded.mLocalHouseIndex, runtime.mLocalHouseIndex);
        if (recorded.mFirstFrame != 0 || recorded.mFirstFrame != runtime.mFirstFrame)
        {
            appendDifference("firstFrame", recorded.mFirstFrame, runtime.mFirstFrame);
            if (recorded.mFirstFrame != 0)
                error += "[required=0]";
        }
        if (recorded.mFrameSendRate != runtime.mFrameSendRate)
            appendDifference("frameSendRate", recorded.mFrameSendRate, runtime.mFrameSendRate);
        if (recorded.mRandomSeed != runtime.mRandomSeed)
            appendDifference("randomSeed", recorded.mRandomSeed, runtime.mRandomSeed);
        return FailPlayback(error);
    }
    mSource = &source;
    mStreamVersion = recorded.mStreamVersion;
    mCurrentGroups.reset();
    mProcessedGroupUpdate.reset();
    mNextFrame = recorded.mFirstFrame;
    mState = ReplayState::Playing;
    mLastError.clear();
    return true;
}

bool ReplaySession::OnFrameBegin()
{
    if (mState == ReplayState::Playing)
    {
        std::uint32_t frameNumber = 0;
        std::uint32_t ignoredChecksum = 0;
        bool ignoredHasChecksum = false;
        std::string error;
        if (!mGame.ReadFrameContext(frameNumber, ignoredChecksum, ignoredHasChecksum, error))
        {
            return FailPlayback(error);
        }
        if (frameNumber == mLastProcessedFrame ||
            (mHasReadyFrame && frameNumber == mReadyFrame.mFrameNumber))
        {
            return true;
        }
        if (mNextFrame > UINT32_MAX || frameNumber != mNextFrame)
        {
            return FailPlayback("Playback frame differs from current game frame");
        }
        return PrefetchPlaybackFrame();
    }
    if (mState != ReplayState::Recording)
    {
        return mState != ReplayState::Failed;
    }
    std::uint32_t frameNumber = 0;
    std::uint32_t checksum = 0;
    bool hasChecksum = false;
    std::string error;
    if (!mGame.ReadFrameContext(frameNumber, checksum, hasChecksum, error))
    {
        return FailRecording(error);
    }
    if (mHasPendingFrame)
    {
        if (frameNumber == mPendingFrame.mFrameNumber)
        {
            return true;
        }
        return FailRecording("A replay frame began before the previous frame ended");
    }
    // 暂停时可能反复进入同一帧；已经提交的帧不能再创建第二份记录。
    if (mNextFrame != 0 && frameNumber == mNextFrame - 1)
    {
        return true;
    }
    if (mNextFrame > UINT32_MAX || frameNumber != mNextFrame)
    {
        return FailRecording("Recording frame sequence is not contiguous");
    }
    mPendingFrame = {};
    mPendingFrame.mFrameNumber = frameNumber;
    mHasPendingFrame = true;
    mHasCopiedEvents = false;
    return true;
}

bool ReplaySession::OnBeforeSimulation()
{
    if (mState != ReplayState::Playing)
    {
        return mState != ReplayState::Failed;
    }
    if (!mHasReadyFrame)
    {
        std::uint32_t frameNumber = 0;
        std::uint32_t ignoredChecksum = 0;
        bool ignoredHasChecksum = false;
        std::string error;
        if (!mGame.ReadFrameContext(frameNumber, ignoredChecksum, ignoredHasChecksum, error))
        {
            return FailPlayback(error);
        }
        if (frameNumber == mLastProcessedFrame)
        {
            return true;
        }
        return FailPlayback("Playback frame was not prefetched before simulation");
    }
    if (mHasAppliedInputEffects)
    {
        return true;
    }
    std::string error;
    if (!mGame.ApplyPlayerInputEffects(mReadyFrame.mInputEffects, error))
    {
        return FailPlayback(error);
    }
    mHasAppliedInputEffects = true;
    return true;
}

bool ReplaySession::OnBeforeEvents()
{
    if (mState != ReplayState::Recording || !mHasPendingFrame || mHasCopiedEvents)
    {
        return mState != ReplayState::Failed;
    }
    std::uint32_t frameNumber = 0;
    std::uint32_t checksum = 0;
    bool hasChecksum = false;
    std::string error;
    if (!mGame.ReadFrameContext(frameNumber, checksum, hasChecksum, error) ||
        frameNumber != mPendingFrame.mFrameNumber)
    {
        return FailRecording(error.empty() ? "Event dispatch frame changed" : error);
    }
    std::vector<RecordedEvent> events;
    if (!mGame.CopyDueEvents(events, error))
    {
        return FailRecording(error);
    }
    if (events.size() > FrameCodec::MAX_EVENTS_PER_FRAME)
    {
        return FailRecording("Recording frame exceeds the event limit");
    }
    mPendingFrame.mChecksum = checksum;
    mPendingFrame.mHasChecksum = hasChecksum;
    mPendingFrame.mEvents = std::move(events);
    mHasCopiedEvents = true;
    return true;
}

bool ReplaySession::OnFrameEnd()
{
    if (mWasPlayback && (mState == ReplayState::Playing || mState == ReplayState::Paused))
    {
        std::uint32_t frameNumber = 0;
        std::uint32_t checksum = 0;
        bool hasChecksum = false;
        std::string error;
        if (!mGame.ReadFrameContext(frameNumber, checksum, hasChecksum, error) ||
            frameNumber != mLastProcessedFrame)
            return FailPlayback(error.empty() ? "Playback completed an unprocessed frame" : error);
        mLastCompletedFrame = frameNumber;
        if (mProcessedGroupUpdate)
        {
            mCurrentGroups = std::move(mProcessedGroupUpdate);
            mProcessedGroupUpdate.reset();
        }
        mHasCompletedPlaybackFrame = true;
        return true;
    }
    if (mState != ReplayState::Recording || !mHasPendingFrame)
    {
        return mState != ReplayState::Failed;
    }
    std::uint32_t frameNumber = 0;
    std::uint32_t checksum = 0;
    bool hasChecksum = false;
    std::string error;
    if (!mGame.ReadFrameContext(frameNumber, checksum, hasChecksum, error) ||
        frameNumber != mPendingFrame.mFrameNumber)
    {
        return FailRecording(error.empty() ? "Frame changed before recording commit" : error);
    }
    if (!mGame.CopyPlayerInputEffects(mPendingFrame.mInputEffects, error))
    {
        return FailRecording(error);
    }
    if (mPendingFrame.mInputEffects.size() > FrameCodec::MAX_INPUT_EFFECTS_PER_FRAME)
    {
        return FailRecording("Recording frame exceeds the input effect limit");
    }
    if (mStreamVersion == 3 &&
        !mGame.CopyRecordedControlGroups(mPendingFrame.mRecordedGroups, error))
        return FailRecording(error);
    if (!ValidateRecordedControlGroups(mPendingFrame.mRecordedGroups,
        mPendingFrame.mFrameNumber, mStreamVersion, error))
        return FailRecording(error);
    if (!mSink->AppendFrame(mPendingFrame, error))
    {
        return FailRecording(error);
    }
    ++mNextFrame;
    mPendingFrame = {};
    mHasPendingFrame = false;
    mHasCopiedEvents = false;
    return true;
}

bool ReplaySession::PrefetchPlaybackFrame()
{
    if (mHasReadyFrame)
        return true;
    ReplayFrame frame;
    std::string error;
    const auto result = mSource->ReadFrame(frame, error);
    if (result == ReplayReadResult::End)
    {
        if (mStreamVersion == 3 && mNextFrame == 0)
            return FailPlayback("编队帧流缺少初始完整快照");
        mState = ReplayState::Ended;
        return true;
    }
    if (result == ReplayReadResult::Error || mNextFrame > UINT32_MAX ||
        frame.mFrameNumber != mNextFrame ||
        frame.mEvents.size() > FrameCodec::MAX_EVENTS_PER_FRAME ||
        frame.mInputEffects.size() > FrameCodec::MAX_INPUT_EFFECTS_PER_FRAME)
        return FailPlayback(result == ReplayReadResult::Error ? error :
            "Playback frame record differs from current game frame");
    if (!ValidateRecordedControlGroups(frame.mRecordedGroups, frame.mFrameNumber,
        mStreamVersion, error))
        return FailPlayback(error);
    mReadyFrame = std::move(frame);
    mHasReadyFrame = true;
    mHasAppliedInputEffects = false;
    return true;
}

bool ReplaySession::OnAfterFrameLoop()
{
    if (!mHasCompletedPlaybackFrame)
        return mState != ReplayState::Failed;
    mHasCompletedPlaybackFrame = false;
    return PrefetchPlaybackFrame();
}

bool ReplaySession::SetPaused(bool paused) noexcept
{
    if (paused && mState == ReplayState::Playing)
        mState = ReplayState::Paused;
    else if (!paused && mState == ReplayState::Paused)
        mState = ReplayState::Playing;
    else
        return false;
    return true;
}

bool ReplaySession::IsPlaybackHeld() const noexcept
{
    return mState == ReplayState::Paused || mState == ReplayState::Ended;
}

std::uint32_t ReplaySession::GetLastCompletedFrame() const noexcept
{
    return mLastCompletedFrame;
}

const std::optional<RecordedControlGroupSnapshot>& ReplaySession::GetRecordedControlGroups() const noexcept
{
    return mCurrentGroups;
}

bool ReplaySession::ProcessPlaybackFrame()
{
    if (!mWasPlayback)
    {
        return false;
    }
    if (mState != ReplayState::Playing)
    {
        return true;
    }
    std::uint32_t frameNumber = 0;
    std::uint32_t ignoredChecksum = 0;
    bool ignoredHasChecksum = false;
    std::string error;
    if (!mGame.ReadFrameContext(frameNumber, ignoredChecksum, ignoredHasChecksum, error))
    {
        FailPlayback(error);
        return true;
    }
    if (frameNumber == mLastProcessedFrame)
    {
        return true;
    }
    if (!mHasReadyFrame || !mHasAppliedInputEffects || mNextFrame > UINT32_MAX ||
        frameNumber != mNextFrame || mReadyFrame.mFrameNumber != frameNumber)
    {
        FailPlayback("Playback frame was not prefetched at frame begin");
        return true;
    }
    mGame.DiscardLocalCommands();
    if (!mGame.ExecuteReplayFrame(mReadyFrame, error))
    {
        FailPlayback(error);
        return true;
    }
    mLastProcessedFrame = frameNumber;
    mProcessedGroupUpdate = std::move(mReadyFrame.mRecordedGroups);
    ++mNextFrame;
    mReadyFrame = {};
    mHasReadyFrame = false;
    mHasAppliedInputEffects = false;
    return true;
}

bool ReplaySession::Finish()
{
    if (mState != ReplayState::Recording)
    {
        mLastError = "Only a recording can be finalized";
        return false;
    }
    // 未达到帧尾的内存记录没有进入容器。
    mPendingFrame = {};
    mHasPendingFrame = false;
    mHasCopiedEvents = false;
    std::string error;
    if (!mSink->Finish(error))
    {
        return FailRecording(error);
    }
    mState = ReplayState::Finished;
    mSink = nullptr;
    return true;
}

void ReplaySession::Stop()
{
    if (mState == ReplayState::Recording && mSink != nullptr)
    {
        mSink->Abort();
    }
    if (mWasPlayback)
    {
        EndPlayback();
    }
    mPendingFrame = {};
    mReadyFrame = {};
    mCurrentGroups.reset();
    mProcessedGroupUpdate.reset();
    mHasPendingFrame = false;
    mHasReadyFrame = false;
    mHasAppliedInputEffects = false;
    mHasCopiedEvents = false;
    mHasCompletedPlaybackFrame = false;
    mSink = nullptr;
    mSource = nullptr;
    if (mState == ReplayState::Recording || mState == ReplayState::Playing || IsPlaybackHeld())
    {
        mState = ReplayState::Finished;
    }
}

ReplayState ReplaySession::GetState() const noexcept
{
    return mState;
}

const std::string& ReplaySession::GetLastError() const noexcept
{
    return mLastError;
}

bool ReplaySession::FailRecording(const std::string& error)
{
    mLastError = error.empty() ? "Replay recording failed" : error;
    if (mSink != nullptr)
    {
        mSink->Abort();
        mSink = nullptr;
    }
    mPendingFrame = {};
    mCurrentGroups.reset();
    mProcessedGroupUpdate.reset();
    mHasPendingFrame = false;
    mState = ReplayState::Failed;
    return false;
}

bool ReplaySession::FailPlayback(const std::string& error)
{
    mLastError = error.empty() ? "Replay playback failed" : error;
    mState = ReplayState::Failed;
    mReadyFrame = {};
    mCurrentGroups.reset();
    mProcessedGroupUpdate.reset();
    mHasReadyFrame = false;
    mHasAppliedInputEffects = false;
    EndPlayback();
    return false;
}

void ReplaySession::EndPlayback()
{
    if (!mHasEndedGame)
    {
        mGame.EndReplay();
        mHasEndedGame = true;
    }
}
}
