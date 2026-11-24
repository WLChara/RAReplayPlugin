#include "../src/game_api/capture/DispatchedEvents.h"
#include "../src/game_api/capture/EventCaptureDiagnostics.h"
#include "../src/game_api/capture/DispatchEventCapture.h"
#include "../src/target/TargetAbi.h"

#include <stdexcept>

namespace
{
using namespace rareplay;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

constexpr bool DecodesTo(std::uint32_t ecx, std::uint32_t expected)
{
    std::uint32_t ringIndex = 0xFFFFFFFF;
    return target::TryDecodeEventDispatchIndex(ecx, ringIndex) && ringIndex == expected;
}

constexpr bool Rejects(std::uint32_t ecx)
{
    std::uint32_t ringIndex = 0xFFFFFFFF;
    return !target::TryDecodeEventDispatchIndex(ecx, ringIndex) && ringIndex == 0xFFFFFFFF;
}

// 0x64C63D 的 ECX = 37 × 环索引；事件地址 = 0x8B4204 + 3 × ECX。
static_assert(DecodesTo(0, 0) && DecodesTo(37, 1) && DecodesTo(37 * 16383, 16383));
static_assert(Rejects(1) && Rejects(36) && Rejects(38) && Rejects(37 * 16384) && Rejects(0xFFFFFFFF));
static_assert(target::DO_LIST_EVENTS + 3 * 37 * 5 == target::DO_LIST_EVENTS + target::EVENT_BYTES * 5);

replay::RecordedEvent MakeEvent(std::uint8_t type, std::uint32_t frame)
{
    replay::RecordedEvent event{};
    for (std::size_t index = 0; index < event.size(); ++index)
        event[index] = static_cast<std::uint8_t>(0xA0 + index);
    event[0] = type;
    event[1] = 0x00;
    event[2] = 0x03;
    event[3] = static_cast<std::uint8_t>(frame);
    event[4] = static_cast<std::uint8_t>(frame >> 8);
    event[5] = static_cast<std::uint8_t>(frame >> 16);
    event[6] = static_cast<std::uint8_t>(frame >> 24);
    return event;
}

void TestFrameStampRewritesOnlyFrame()
{
    const auto dispatched = MakeEvent(4, 0x00012344);
    Require(game_api::ReadEventFrame(dispatched) == 0x00012344, "frame is little-endian at offset 3");
    replay::RecordedEvent copy{};
    Require(game_api::TryPrepareDispatchedEvent(dispatched, 0x01020345, copy),
        "player command is captured");
    Require(game_api::ReadEventFrame(copy) == 0x01020345 && copy[3] == 0x45 && copy[4] == 0x03 &&
        copy[5] == 0x02 && copy[6] == 0x01, "copy frame becomes the execution frame");
    for (std::size_t index = 0; index < copy.size(); ++index)
    {
        if (index >= game_api::EVENT_FRAME_OFFSET && index < game_api::EVENT_FRAME_OFFSET + 4)
            continue;
        Require(copy[index] == dispatched[index], "all non-frame bytes stay unchanged");
    }
    Require(game_api::ReadEventFrame(dispatched) == 0x00012344, "source bytes are not modified");

    const auto current = MakeEvent(14, 77);
    Require(game_api::TryPrepareDispatchedEvent(current, 77, copy) && copy == current,
        "current-frame event copy is byte identical");
}

void TestFrameInfoExcluded()
{
    const auto frameInfo = MakeEvent(game_api::EVENT_TYPE_FRAME_INFO, 9);
    Require(game_api::IsFrameInfoEvent(frameInfo), "type 0x1C is FrameInfo");
    const auto sentinel = MakeEvent(4, 1234);
    auto copy = sentinel;
    Require(!game_api::TryPrepareDispatchedEvent(frameInfo, 10, copy) && copy == sentinel,
        "FrameInfo is not recorded and leaves the output untouched");
    Require(!game_api::IsFrameInfoEvent(MakeEvent(12, 9)), "Options is still a player event");
}

void TestFrameRelation()
{
    using game_api::EventFrameRelation;
    Require(game_api::ClassifyEventFrame(9, 10) == EventFrameRelation::Stale &&
        game_api::ClassifyEventFrame(10, 10) == EventFrameRelation::Current &&
        game_api::ClassifyEventFrame(11, 10) == EventFrameRelation::Future,
        "frame relation follows handleAny comparisons");
    Require(game_api::ClassifyEventFrame(0xFFFFFFFF, 0) == EventFrameRelation::Stale,
        "comparison is signed like the native jl");
}

void TestDiagnosticsFrameLifecycle()
{
    game_api::EventCaptureDiagnostics diagnostics;
    Require(!diagnostics.CompleteFrame(), "no frame before begin");
    diagnostics.BeginFrame(100);
    diagnostics.RecordEntry(1, 2, 3, 0);
    diagnostics.RecordDispatch(99);
    diagnostics.RecordDispatch(97);
    diagnostics.RecordDispatch(100);
    diagnostics.RecordFrameInfoSkipped();
    diagnostics.RecordMoveFlash(false);
    // 暂停重入同一帧不清除本帧计数。
    diagnostics.BeginFrame(100);
    diagnostics.RecordMoveFlash(true);
    const auto frame = diagnostics.CompleteFrame();
    Require(frame && frame->mFrameNumber == 100 && frame->mHasEntry && frame->mEntryStale == 1 &&
        frame->mEntryCurrent == 2 && frame->mEntryFuture == 3 && frame->mDispatched == 3 &&
        frame->mStaleDispatched == 2 && frame->mPreviousFrameDispatched == 1 &&
        frame->mFrameInfoSkipped == 1 && frame->mMoveFlashBeforeParser == 1 &&
        frame->mMoveFlashAfterParser == 1 && frame->IsAnomalous(), "frame counters accumulate");
    Require(!diagnostics.CompleteFrame(), "a frame completes only once");

    diagnostics.BeginFrame(101);
    diagnostics.RecordEntry(0, 1, 0, 2);
    diagnostics.RecordDispatch(101);
    diagnostics.RecordMoveFlash(false);
    const auto normal = diagnostics.CompleteFrame();
    Require(normal && !normal->IsAnomalous(), "current dispatch with move feedback is normal");

    diagnostics.BeginFrame(102);
    diagnostics.RecordMoveFlash(true);
    const auto missing = diagnostics.CompleteFrame();
    Require(missing && missing->IsAnomalous() && !missing->mHasEntry,
        "move feedback without dispatch is anomalous");

    diagnostics.BeginFrame(103);
    diagnostics.RecordUnmarkedDispatch();
    const auto unmarked = diagnostics.CompleteFrame();
    Require(unmarked && unmarked->IsAnomalous(), "dispatch outside the entry marker is anomalous");

    diagnostics.BeginFrame(104);
    diagnostics.BeginFrame(105);
    const auto replaced = diagnostics.CompleteFrame();
    Require(replaced && replaced->mFrameNumber == 105 && !replaced->IsAnomalous(),
        "unfinished frame is replaced by the next frame");

    const auto& totals = diagnostics.GetTotals();
    Require(totals.mFrames == 5 && totals.mEntryFrames == 2 && totals.mEntryStale == 1 &&
        totals.mEntryCurrent == 3 && totals.mEntryFuture == 3 && totals.mMaxOutList == 2 &&
        totals.mDispatched == 4 && totals.mStaleDispatched == 2 &&
        totals.mPreviousFrameDispatched == 1 && totals.mFrameInfoSkipped == 1 &&
        totals.mUnmarkedDispatches == 1 && totals.mMoveFlashBeforeParser == 2 &&
        totals.mMoveFlashAfterParser == 2 && totals.mAnomalyFrames == 3,
        "totals include only completed frames");
}

// 以下断言 DispatchEventCapture 的现有行为；GameApi 每次传入的帧号与模式在此直接构造。
game_api::DispatchCaptureFrame SinglePlayerFrame(std::uint32_t activeFrame, std::uint32_t currentFrame)
{
    game_api::DispatchCaptureFrame frame;
    frame.mActiveFrame = activeFrame;
    frame.mIsFrameActive = true;
    frame.mCurrentFrame = currentFrame;
    frame.mUsesDispatchCapture = true;
    return frame;
}

game_api::DispatchCaptureFrame SinglePlayerFrame(std::uint32_t frameNumber)
{
    return SinglePlayerFrame(frameNumber, frameNumber);
}

game_api::DispatchCaptureFrame OnlineFrame(std::uint32_t frameNumber)
{
    auto frame = SinglePlayerFrame(frameNumber);
    frame.mUsesDispatchCapture = false;
    return frame;
}

void TestCaptureMarkerLifecycle()
{
    game_api::DispatchEventCapture capture;
    Require(!capture.IsOpen(), "marker starts closed");
    capture.BeginFrame(100);
    Require(!capture.IsOpen(), "frame begin leaves the marker closed");

    Require(capture.OpenAtEventExecution(SinglePlayerFrame(100)) && capture.IsOpen(),
        "single-player handleAny entry opens the marker");
    capture.CloseAfterFrameParser(100);
    Require(!capture.IsOpen(), "frame parser return closes the marker");

    Require(capture.OpenAtEventExecution(SinglePlayerFrame(100)), "entry reopens within the same frame");
    capture.BeginFrame(101);
    Require(!capture.IsOpen(), "next frame begin closes a marker left open");
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(101)), "entry opens again in the new frame");
    capture.BeginFrame(101);
    Require(!capture.IsOpen(), "re-entering the same frame (pause) also closes the marker");

    auto inactive = SinglePlayerFrame(101);
    inactive.mIsFrameActive = false;
    Require(!capture.OpenAtEventExecution(inactive) && !capture.IsOpen(),
        "inactive frame does not open the marker");
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(101)), "marker opens for the active frame");
    Require(!capture.OpenAtEventExecution(SinglePlayerFrame(101, 102)) && !capture.IsOpen(),
        "entry outside the active frame closes an open marker");
}

void TestUnmarkedDispatchNotCaptured()
{
    game_api::DispatchEventCapture capture;
    capture.BeginFrame(200);
    Require(capture.AdmitDispatch(SinglePlayerFrame(200)) == game_api::DispatchAdmission::Unmarked,
        "dispatch before the entry marker is not captured");
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(200)), "entry opens the marker");
    capture.CloseAfterFrameParser(200);
    Require(capture.AdmitDispatch(SinglePlayerFrame(200)) == game_api::DispatchAdmission::Unmarked,
        "dispatch after the parser returned is not captured");
    const auto frame = capture.CompleteFrame(true);
    Require(frame && frame->mUnmarkedDispatches == 2 && frame->mDispatched == 0 &&
        !frame->mHasEntry && frame->IsAnomalous(),
        "single-player unmarked dispatches are counted; opening alone records no entry");
}

void TestOnlineModeNotCaptured()
{
    game_api::DispatchEventCapture capture;
    capture.BeginFrame(300);
    Require(!capture.OpenAtEventExecution(OnlineFrame(300)) && !capture.IsOpen(),
        "online handleAny entry does not open the marker");
    Require(capture.AdmitDispatch(OnlineFrame(300)) == game_api::DispatchAdmission::Unmarked,
        "online dispatch is not captured");
    capture.RecordMoveFlash(OnlineFrame(300));
    Require(!capture.CompleteFrame(false), "online frames produce no capture statistics");
    Require(capture.GetTotals().mFrames == 0, "online frames never reach the totals");
    // 联机帧开始仍让诊断进入该帧但从不完成；这里强制完成，确认联机分派与 MoveFlash 均未计数。
    const auto pending = capture.CompleteFrame(true);
    Require(pending && pending->mFrameNumber == 300 && pending->mUnmarkedDispatches == 0 &&
        pending->mMoveFlashBeforeParser == 0 && pending->mMoveFlashAfterParser == 0,
        "online dispatch and move feedback are not counted");
}

void TestOutsideActiveFrame()
{
    game_api::DispatchEventCapture capture;
    capture.BeginFrame(400);
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(400)), "entry opens the marker");
    Require(capture.AdmitDispatch(SinglePlayerFrame(400, 401)) ==
        game_api::DispatchAdmission::OutsideActiveFrame, "frame number moved while open");
    auto inactive = SinglePlayerFrame(400);
    inactive.mIsFrameActive = false;
    Require(capture.AdmitDispatch(inactive) == game_api::DispatchAdmission::OutsideActiveFrame,
        "frame became inactive while open");
    Require(capture.IsOpen(), "rejection leaves the marker open");
    Require(capture.AdmitDispatch(SinglePlayerFrame(400)) == game_api::DispatchAdmission::Admitted,
        "dispatch inside the active frame is admitted");
    const auto frame = capture.CompleteFrame(true);
    Require(frame && frame->mUnmarkedDispatches == 0 && frame->mDispatched == 0 &&
        frame->mFrameInfoSkipped == 0, "admission alone records no dispatch counters");
}

void TestCaptureCopiesAndSkipsFrameInfo()
{
    game_api::DispatchEventCapture capture;
    capture.BeginFrame(500);
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(500)), "entry opens the marker");

    const auto frameInfo = MakeEvent(game_api::EVENT_TYPE_FRAME_INFO, 500);
    const auto sentinel = MakeEvent(4, 1);
    auto copy = sentinel;
    Require(capture.AdmitDispatch(SinglePlayerFrame(500)) == game_api::DispatchAdmission::Admitted &&
        !capture.Capture(frameInfo, 500, copy) && copy == sentinel,
        "FrameInfo is admitted but not captured, output untouched");

    const auto stale = MakeEvent(4, 499);
    Require(capture.Capture(stale, 500, copy) && game_api::ReadEventFrame(copy) == 500 &&
        copy[0] == 4, "stale command is captured with the execution frame");
    const auto current = MakeEvent(14, 500);
    Require(capture.Capture(current, 500, copy) && copy == current, "current command is captured as is");

    const auto frame = capture.CompleteFrame(true);
    Require(frame && frame->mFrameInfoSkipped == 1 && frame->mDispatched == 2 &&
        frame->mStaleDispatched == 1 && frame->mPreviousFrameDispatched == 1,
        "capture counts dispatches by original frame");
}

void TestMoveFlashAroundParser()
{
    game_api::DispatchEventCapture capture;
    // 尚无解析器返回时，即使帧号与解析器返回帧的初值 0 相等，也计为解析器之前。
    capture.BeginFrame(0);
    capture.RecordMoveFlash(SinglePlayerFrame(0));
    capture.CloseAfterFrameParser(0);
    capture.RecordMoveFlash(SinglePlayerFrame(0));
    auto frame = capture.CompleteFrame(true);
    Require(frame && frame->mMoveFlashBeforeParser == 1 && frame->mMoveFlashAfterParser == 1,
        "parser return switches move feedback to after-parser");

    // 解析器返回帧不随帧开始清除；新帧在本帧解析器返回前按帧号比较，计为之前。
    capture.BeginFrame(1);
    capture.RecordMoveFlash(SinglePlayerFrame(1));
    capture.CloseAfterFrameParser(1);
    capture.RecordMoveFlash(SinglePlayerFrame(1));
    capture.RecordMoveFlash(SinglePlayerFrame(1));
    frame = capture.CompleteFrame(true);
    Require(frame && frame->mMoveFlashBeforeParser == 1 && frame->mMoveFlashAfterParser == 2,
        "after-parser is decided by the parser return frame");
}

void TestCaptureFrameStatsAndTotals()
{
    game_api::DispatchEventCapture capture;
    Require(!capture.CompleteFrame(true), "no frame before the first frame begin");

    capture.BeginFrame(600);
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(600)), "entry opens frame 600");
    capture.RecordEntry(1, 2, 0, 3);
    Require(capture.AdmitDispatch(SinglePlayerFrame(600)) == game_api::DispatchAdmission::Admitted,
        "frame 600 dispatch admitted");
    replay::RecordedEvent copy{};
    Require(capture.Capture(MakeEvent(4, 599), 600, copy), "stale dispatch captured");
    Require(capture.Capture(MakeEvent(4, 600), 600, copy), "current dispatch captured");
    capture.RecordMoveFlash(SinglePlayerFrame(600));
    capture.CloseAfterFrameParser(600);
    capture.RecordMoveFlash(SinglePlayerFrame(600));
    const auto first = capture.CompleteFrame(true);
    Require(first && first->mFrameNumber == 600 && first->mHasEntry && first->mEntryStale == 1 &&
        first->mEntryCurrent == 2 && first->mEntryFuture == 0 && first->mOutListCount == 3 &&
        first->mDispatched == 2 && first->mStaleDispatched == 1 &&
        first->mPreviousFrameDispatched == 1 && first->mMoveFlashBeforeParser == 1 &&
        first->mMoveFlashAfterParser == 1 && first->IsAnomalous(), "frame 600 counters");
    Require(!capture.CompleteFrame(true), "a frame completes only once");

    // 暂停重入同一帧：标记关闭，本帧计数保留。
    capture.BeginFrame(601);
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(601)), "entry opens frame 601");
    capture.RecordEntry(0, 1, 1, 1);
    Require(capture.Capture(MakeEvent(4, 601), 601, copy), "frame 601 dispatch captured");
    capture.BeginFrame(601);
    Require(capture.AdmitDispatch(SinglePlayerFrame(601)) == game_api::DispatchAdmission::Unmarked,
        "after re-entering, dispatch is unmarked");
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(601)), "entry reopens frame 601");
    capture.RecordEntry(0, 1, 0, 0);
    const auto second = capture.CompleteFrame(true);
    Require(second && second->mFrameNumber == 601 && second->mEntryCurrent == 2 &&
        second->mEntryFuture == 1 && second->mOutListCount == 1 && second->mDispatched == 1 &&
        second->mUnmarkedDispatches == 1 && second->IsAnomalous(), "re-entered frame keeps its counters");

    capture.BeginFrame(602);
    Require(capture.OpenAtEventExecution(SinglePlayerFrame(602)), "entry opens frame 602");
    Require(!capture.Capture(MakeEvent(game_api::EVENT_TYPE_FRAME_INFO, 602), 602, copy),
        "FrameInfo skipped");
    capture.CloseAfterFrameParser(602);
    const auto third = capture.CompleteFrame(true);
    Require(third && !third->mHasEntry && third->mFrameInfoSkipped == 1 && !third->IsAnomalous(),
        "frame 602 is normal");

    const auto& totals = capture.GetTotals();
    Require(totals.mFrames == 3 && totals.mEntryFrames == 2 && totals.mEntryStale == 1 &&
        totals.mEntryCurrent == 4 && totals.mEntryFuture == 1 && totals.mMaxOutList == 3 &&
        totals.mDispatched == 3 && totals.mStaleDispatched == 1 &&
        totals.mPreviousFrameDispatched == 1 && totals.mFrameInfoSkipped == 1 &&
        totals.mUnmarkedDispatches == 1 && totals.mMoveFlashBeforeParser == 1 &&
        totals.mMoveFlashAfterParser == 1 && totals.mAnomalyFrames == 2,
        "totals accumulate completed frames");
}
}

int main()
{
    TestFrameStampRewritesOnlyFrame();
    TestFrameInfoExcluded();
    TestFrameRelation();
    TestDiagnosticsFrameLifecycle();
    TestCaptureMarkerLifecycle();
    TestUnmarkedDispatchNotCaptured();
    TestOnlineModeNotCaptured();
    TestOutsideActiveFrame();
    TestCaptureCopiesAndSkipsFrameInfo();
    TestMoveFlashAroundParser();
    TestCaptureFrameStatsAndTotals();
}
