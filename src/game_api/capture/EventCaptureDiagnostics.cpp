#include "EventCaptureDiagnostics.h"

#include <algorithm>

namespace rareplay::game_api
{
bool EventCaptureFrameStats::IsAnomalous() const noexcept
{
    const bool hasMoveFlash = mMoveFlashBeforeParser != 0 || mMoveFlashAfterParser != 0;
    return mStaleDispatched != 0 || mUnmarkedDispatches != 0 || (hasMoveFlash && mDispatched == 0);
}

void EventCaptureDiagnostics::BeginFrame(std::uint32_t frameNumber) noexcept
{
    if (mHasFrame && mFrame.mFrameNumber == frameNumber)
        return;
    mFrame = {};
    mFrame.mFrameNumber = frameNumber;
    mHasFrame = true;
}

void EventCaptureDiagnostics::RecordEntry(std::uint32_t staleCount, std::uint32_t currentCount,
    std::uint32_t futureCount, std::uint32_t outListCount) noexcept
{
    if (!mHasFrame)
        return;
    mFrame.mHasEntry = true;
    mFrame.mEntryStale += staleCount;
    mFrame.mEntryCurrent += currentCount;
    mFrame.mEntryFuture += futureCount;
    mFrame.mOutListCount = std::max(mFrame.mOutListCount, outListCount);
}

void EventCaptureDiagnostics::RecordDispatch(std::uint32_t originalFrame) noexcept
{
    if (!mHasFrame)
        return;
    ++mFrame.mDispatched;
    if (ClassifyEventFrame(originalFrame, mFrame.mFrameNumber) != EventFrameRelation::Stale)
        return;
    ++mFrame.mStaleDispatched;
    if (originalFrame + 1 == mFrame.mFrameNumber)
        ++mFrame.mPreviousFrameDispatched;
}

void EventCaptureDiagnostics::RecordFrameInfoSkipped() noexcept
{
    if (mHasFrame)
        ++mFrame.mFrameInfoSkipped;
}

void EventCaptureDiagnostics::RecordUnmarkedDispatch() noexcept
{
    if (mHasFrame)
        ++mFrame.mUnmarkedDispatches;
}

void EventCaptureDiagnostics::RecordMoveFlash(bool isAfterParser) noexcept
{
    if (!mHasFrame)
        return;
    if (isAfterParser)
        ++mFrame.mMoveFlashAfterParser;
    else
        ++mFrame.mMoveFlashBeforeParser;
}

std::optional<EventCaptureFrameStats> EventCaptureDiagnostics::CompleteFrame() noexcept
{
    if (!mHasFrame)
        return std::nullopt;
    mHasFrame = false;
    ++mTotals.mFrames;
    if (mFrame.mHasEntry)
        ++mTotals.mEntryFrames;
    mTotals.mEntryStale += mFrame.mEntryStale;
    mTotals.mEntryCurrent += mFrame.mEntryCurrent;
    mTotals.mEntryFuture += mFrame.mEntryFuture;
    mTotals.mMaxOutList = std::max(mTotals.mMaxOutList, mFrame.mOutListCount);
    mTotals.mDispatched += mFrame.mDispatched;
    mTotals.mStaleDispatched += mFrame.mStaleDispatched;
    mTotals.mPreviousFrameDispatched += mFrame.mPreviousFrameDispatched;
    mTotals.mFrameInfoSkipped += mFrame.mFrameInfoSkipped;
    mTotals.mUnmarkedDispatches += mFrame.mUnmarkedDispatches;
    mTotals.mMoveFlashBeforeParser += mFrame.mMoveFlashBeforeParser;
    mTotals.mMoveFlashAfterParser += mFrame.mMoveFlashAfterParser;
    if (mFrame.IsAnomalous())
        ++mTotals.mAnomalyFrames;
    return mFrame;
}
}
