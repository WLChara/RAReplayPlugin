#include "EventCaptureReporter.h"
#include "Diagnostics.h"

#include <cstdio>

namespace rareplay::plugin
{
void EventCaptureReporter::ReportContext(const game_api::EventCaptureContext& context) noexcept
{
    char message[256]{};
    std::snprintf(message, sizeof(message),
        "Replay capture context: sessionGameMode=%u mpGameMode=0x%08llX mpModeIndex=%d eventCapture=%s",
        context.mSessionGameMode, static_cast<unsigned long long>(context.mMpGameModeAddress),
        context.mMpModeIndex, context.mUsesDispatchCapture ? "dispatch" : "entry");
    Log(message);
}

void EventCaptureReporter::ReportFrame(const game_api::EventCaptureFrameStats& stats) noexcept
{
    if (!stats.IsAnomalous() || mFrameLines >= MAX_FRAME_LINES)
        return;
    ++mFrameLines;
    char message[512]{};
    std::snprintf(message, sizeof(message),
        "Replay capture anomaly: frame=%u entry=%d entryEqualN=%u entryStale=%u entryFuture=%u outList=%u dispatched=%u staleDispatched=%u prevFrameDispatched=%u unmarkedDispatches=%u moveFlashBeforeParser=%u moveFlashAfterParser=%u%s",
        stats.mFrameNumber, stats.mHasEntry ? 1 : 0, stats.mEntryCurrent, stats.mEntryStale,
        stats.mEntryFuture, stats.mOutListCount, stats.mDispatched, stats.mStaleDispatched,
        stats.mPreviousFrameDispatched, stats.mUnmarkedDispatches, stats.mMoveFlashBeforeParser,
        stats.mMoveFlashAfterParser,
        mFrameLines == MAX_FRAME_LINES ? " (per-frame line limit reached)" : "");
    Log(message);
}

void EventCaptureReporter::ReportSummary(const game_api::EventCaptureTotals& totals) noexcept
{
    char message[512]{};
    std::snprintf(message, sizeof(message),
        "Replay capture stats: frames=%llu entryFrames=%llu dispatched=%llu staleDispatched=%llu prevFrameDispatched=%llu frameInfoSkipped=%llu unmarkedDispatches=%llu entryEqualN=%llu entryStale=%llu entryFuture=%llu maxOutList=%u moveFlashBeforeParser=%llu moveFlashAfterParser=%llu anomalyFrames=%llu",
        static_cast<unsigned long long>(totals.mFrames),
        static_cast<unsigned long long>(totals.mEntryFrames),
        static_cast<unsigned long long>(totals.mDispatched),
        static_cast<unsigned long long>(totals.mStaleDispatched),
        static_cast<unsigned long long>(totals.mPreviousFrameDispatched),
        static_cast<unsigned long long>(totals.mFrameInfoSkipped),
        static_cast<unsigned long long>(totals.mUnmarkedDispatches),
        static_cast<unsigned long long>(totals.mEntryCurrent),
        static_cast<unsigned long long>(totals.mEntryStale),
        static_cast<unsigned long long>(totals.mEntryFuture), totals.mMaxOutList,
        static_cast<unsigned long long>(totals.mMoveFlashBeforeParser),
        static_cast<unsigned long long>(totals.mMoveFlashAfterParser),
        static_cast<unsigned long long>(totals.mAnomalyFrames));
    Log(message);
}
}
