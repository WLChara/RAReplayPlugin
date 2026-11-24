#include "DispatchEventCapture.h"

#include "DispatchedEvents.h"

namespace rareplay::game_api
{
void DispatchEventCapture::BeginFrame(std::uint32_t activeFrame) noexcept
{
    mIsOpen = false;
    mDiagnostics.BeginFrame(activeFrame);
}

bool DispatchEventCapture::OpenAtEventExecution(const DispatchCaptureFrame& frame) noexcept
{
    mIsOpen = frame.mIsFrameActive && frame.mUsesDispatchCapture &&
        frame.mCurrentFrame == frame.mActiveFrame;
    return mIsOpen;
}

void DispatchEventCapture::RecordEntry(std::uint32_t staleCount, std::uint32_t currentCount,
    std::uint32_t futureCount, std::uint32_t outListCount) noexcept
{
    mDiagnostics.RecordEntry(staleCount, currentCount, futureCount, outListCount);
}

void DispatchEventCapture::CloseAfterFrameParser(std::uint32_t currentFrame) noexcept
{
    mIsOpen = false;
    mHasParserReturned = true;
    mParserReturnedFrame = currentFrame;
}

DispatchAdmission DispatchEventCapture::AdmitDispatch(const DispatchCaptureFrame& frame) noexcept
{
    if (!mIsOpen)
    {
        if (frame.mUsesDispatchCapture)
            mDiagnostics.RecordUnmarkedDispatch();
        return DispatchAdmission::Unmarked;
    }
    if (!frame.mIsFrameActive || frame.mCurrentFrame != frame.mActiveFrame)
        return DispatchAdmission::OutsideActiveFrame;
    return DispatchAdmission::Admitted;
}

bool DispatchEventCapture::Capture(const replay::RecordedEvent& dispatched,
    std::uint32_t activeFrame, replay::RecordedEvent& outEvent) noexcept
{
    if (!TryPrepareDispatchedEvent(dispatched, activeFrame, outEvent))
    {
        mDiagnostics.RecordFrameInfoSkipped();
        return false;
    }
    mDiagnostics.RecordDispatch(ReadEventFrame(dispatched));
    return true;
}

void DispatchEventCapture::RecordMoveFlash(const DispatchCaptureFrame& frame) noexcept
{
    if (frame.mUsesDispatchCapture)
        mDiagnostics.RecordMoveFlash(mHasParserReturned && mParserReturnedFrame == frame.mActiveFrame);
}

std::optional<EventCaptureFrameStats> DispatchEventCapture::CompleteFrame(
    bool usesDispatchCapture) noexcept
{
    if (!usesDispatchCapture)
        return std::nullopt;
    return mDiagnostics.CompleteFrame();
}
}
