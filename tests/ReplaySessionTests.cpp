#include "../src/replay/ReplaySession.h"
#include "../src/replay/FrameCodec.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace rareplay::replay;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class FakeGame final : public IReplayGame
{
public:
    ReplaySessionInfo mInfo{2, 1, 0, 6, 42};
    std::uint32_t mFrame = 0;
    std::uint32_t mChecksum = 0x1234;
    bool mHasChecksum = true;
    std::vector<RecordedEvent> mDueEvents;
    std::vector<RecordedInputEffect> mInputEffects;
    std::vector<ReplayFrame> mExecuted;
    std::vector<std::string> mPhases;
    int mEndCount = 0;
    int mDiscardCount = 0;
    int mCopyCount = 0;
    bool mExecutionFails = false;
    bool mCopyEffectsFails = false;
    bool mApplyEffectsFails = false;
    int mApplyCount = 0;
    int mCopyEffectsCount = 0;
    std::optional<RecordedControlGroupSnapshot> mGroupUpdate;
    bool mCopyGroupsFails = false;
    int mCopyGroupsCount = 0;
    std::int32_t mGameSpeed = 2;
    bool mReadSpeedFails = false;
    bool mApplySpeedFails = false;
    int mReadSpeedCount = 0;
    std::vector<std::uint32_t> mAppliedSpeeds;

    bool ReadSessionInfo(ReplaySessionInfo& outInfo, std::string&) override
    {
        outInfo = mInfo;
        return true;
    }
    bool ReadFrameContext(std::uint32_t& outFrameNumber, std::uint32_t& outChecksum,
        bool& outHasChecksum, std::string&) override
    {
        outFrameNumber = mFrame;
        outChecksum = mChecksum;
        outHasChecksum = mHasChecksum;
        return true;
    }
    bool CopyDueEvents(std::vector<RecordedEvent>& outEvents, std::string&) override
    {
        ++mCopyCount;
        outEvents = mDueEvents;
        return true;
    }
    bool CopyPlayerInputEffects(std::vector<RecordedInputEffect>& outEffects,
        std::string& error) override
    {
        ++mCopyEffectsCount;
        mPhases.push_back("copyEffects");
        if (mCopyEffectsFails)
        {
            error = "effect capture failed";
            return false;
        }
        outEffects = mInputEffects;
        mInputEffects.clear();
        return true;
    }
    bool ApplyPlayerInputEffects(const std::vector<RecordedInputEffect>& effects,
        std::string& error) override
    {
        ++mApplyCount;
        mPhases.push_back("applyEffects");
        if (mApplyEffectsFails)
        {
            error = "effect apply failed";
            return false;
        }
        mInputEffects = effects;
        return true;
    }
    bool CopyRecordedControlGroups(std::optional<RecordedControlGroupSnapshot>& outUpdate,
        std::string& error) override
    {
        ++mCopyGroupsCount;
        if (mCopyGroupsFails)
        {
            error = "group capture failed";
            return false;
        }
        outUpdate = mGroupUpdate;
        return true;
    }
    bool ReadGameSpeed(std::int32_t& outSpeed, std::string& error) override
    {
        ++mReadSpeedCount;
        mPhases.push_back("readSpeed");
        if (mReadSpeedFails)
        {
            error = "speed read failed";
            return false;
        }
        outSpeed = mGameSpeed;
        return true;
    }
    bool ApplyGameSpeed(std::uint32_t speed, std::string& error) override
    {
        mPhases.push_back("applySpeed");
        if (mApplySpeedFails)
        {
            error = "speed apply failed";
            return false;
        }
        mAppliedSpeeds.push_back(speed);
        mGameSpeed = static_cast<std::int32_t>(speed);
        return true;
    }
    bool ExecuteReplayFrame(const ReplayFrame& frame, std::string& error) override
    {
        if (mExecutionFails)
        {
            error = "native CRC mismatch";
            return false;
        }
        mExecuted.push_back(frame);
        mPhases.push_back("executeFrame");
        return true;
    }
    void DiscardLocalCommands() override { ++mDiscardCount; mPhases.push_back("discardCommands"); }
    void EndReplay() override { ++mEndCount; }
};

class FakeSink final : public IReplaySink
{
public:
    std::vector<ReplayFrame> mFrames;
    bool mAppendFails = false;
    bool mFinished = false;
    bool mAborted = false;

    bool AppendFrame(const ReplayFrame& frame, std::string& error) override
    {
        if (mAppendFails)
        {
            error = "write failed";
            return false;
        }
        mFrames.push_back(frame);
        return true;
    }
    bool Finish(std::string&) override
    {
        mFinished = true;
        return true;
    }
    void Abort() noexcept override { mAborted = true; }
};

class FakeSource final : public IReplaySource
{
public:
    ReplaySessionInfo mInfo{2, 1, 0, 6, 42};
    std::vector<ReplayFrame> mFrames;
    std::size_t mIndex = 0;
    int mReadCount = 0;
    bool mReadFails = false;

    bool GetSessionInfo(ReplaySessionInfo& outInfo, std::string&) override
    {
        outInfo = mInfo;
        return true;
    }
    ReplayReadResult ReadFrame(ReplayFrame& outFrame, std::string& error) override
    {
        ++mReadCount;
        if (mReadFails)
        {
            error = "corrupt stream";
            return ReplayReadResult::Error;
        }
        if (mIndex == mFrames.size())
        {
            return ReplayReadResult::End;
        }
        outFrame = mFrames[mIndex++];
        return ReplayReadResult::Frame;
    }
};

void TestRecordCompleteFramesOnly()
{
    FakeGame game;
    FakeSink sink;
    ReplaySession session(game);
    Require(session.StartRecording(sink), "recording should start");
    Require(session.OnFrameBegin() && session.OnFrameBegin(), "repeated begin should be idempotent");
    Require(session.OnBeforeEvents() && session.OnBeforeEvents(), "events should copy once");
    Require(game.mCopyCount == 1, "event snapshot must only happen once");
    Require(session.OnFrameEnd() && sink.mFrames.size() == 1 &&
        sink.mFrames[0].mFrameNumber == 0 && sink.mFrames[0].mEvents.empty() &&
        sink.mFrames[0].mHasChecksum, "empty completed frame must be recorded");
    Require(session.OnFrameBegin() && session.OnFrameEnd() && sink.mFrames.size() == 1,
        "paused repeated frame must not fabricate another record");

    game.mFrame = 1;
    RecordedEvent first{};
    RecordedEvent second{};
    first[0] = 1;
    second[0] = 2;
    game.mDueEvents = {first, second};
    RecordedInputEffect feedback;
    feedback.mX = 321;
    game.mInputEffects.push_back(feedback);
    game.mHasChecksum = false;
    Require(session.OnFrameBegin() && session.OnBeforeEvents() && session.OnFrameEnd(),
        "next frame should record");
    Require(sink.mFrames.size() == 2 && sink.mFrames[1].mEvents.size() == 2 &&
        sink.mFrames[1].mEvents[0][0] == 1 && sink.mFrames[1].mEvents[1][0] == 2 &&
        sink.mFrames[1].mInputEffects.size() == 1 &&
        sink.mFrames[1].mInputEffects[0].mX == 321 && game.mInputEffects.empty() &&
        game.mCopyEffectsCount == 2 &&
        !sink.mFrames[1].mHasChecksum, "event order and checksum flag must survive");

    game.mFrame = 2;
    Require(session.OnFrameBegin() && session.Finish(), "finish should discard unfinished frame");
    Require(sink.mFinished && sink.mFrames.size() == 2 && game.mEndCount == 0,
        "recording finish must not end live game");
}

void TestRecordingFailureDoesNotEndGame()
{
    FakeGame game;
    FakeSink sink;
    sink.mAppendFails = true;
    ReplaySession session(game);
    Require(session.StartRecording(sink) && session.OnFrameBegin(), "recording should begin");
    Require(!session.OnFrameEnd() && session.GetState() == ReplayState::Failed &&
        sink.mAborted && game.mEndCount == 0, "I/O failure must leave live simulation alone");

    FakeGame failedCapture;
    failedCapture.mCopyEffectsFails = true;
    FakeSink captureSink;
    ReplaySession captureSession(failedCapture);
    Require(captureSession.StartRecording(captureSink) && captureSession.OnFrameBegin() &&
        !captureSession.OnFrameEnd() && captureSession.GetState() == ReplayState::Failed &&
        captureSink.mFrames.empty() && captureSink.mAborted &&
        failedCapture.mEndCount == 0 &&
        captureSession.GetLastError() == "effect capture failed",
        "effect capture failure must abort before append without ending live game");
}

RecordedEvent MakeEvent(std::uint8_t marker)
{
    RecordedEvent event{};
    event[0] = marker;
    event[110] = marker;
    return event;
}

void TestDispatchCaptureCommitsAtFrameEnd()
{
    FakeGame game;
    game.mInfo.mGameMode = 5;
    FakeSink sink;
    ReplaySession session(game);
    Require(session.StartRecording(sink), "offline recording should start");

    // 空帧：入口不采，没有分派，帧尾仍提交一条空记录。
    Require(session.OnFrameBegin() && session.OnBeforeEvents() && session.OnFrameEnd() &&
        sink.mFrames.size() == 1 && sink.mFrames[0].mEvents.empty(),
        "empty offline frame is still committed");

    game.mFrame = 1;
    Require(session.OnFrameBegin() && session.OnBeforeEvents(), "entry snapshot copies nothing");
    Require(session.OnEventDispatched(MakeEvent(1)) && session.OnEventDispatched(MakeEvent(2)) &&
        sink.mFrames.size() == 1, "dispatched events wait for frame end");
    // 暂停重入与第二次入口都不能清掉已分派的事件。
    Require(session.OnFrameBegin() && session.OnBeforeEvents() && game.mCopyCount == 2,
        "entry copies once per frame");
    Require(session.OnEventDispatched(MakeEvent(3)) && session.OnFrameEnd() &&
        sink.mFrames.size() == 2 && sink.mFrames[1].mFrameNumber == 1 &&
        sink.mFrames[1].mEvents.size() == 3 && sink.mFrames[1].mEvents[0] == MakeEvent(1) &&
        sink.mFrames[1].mEvents[1] == MakeEvent(2) && sink.mFrames[1].mEvents[2] == MakeEvent(3),
        "multi-event frame keeps dispatch order");

    game.mFrame = 2;
    game.mDueEvents = {MakeEvent(9)};
    Require(session.OnFrameBegin() && session.OnBeforeEvents() &&
        session.OnEventDispatched(MakeEvent(4)) && session.OnFrameEnd() &&
        sink.mFrames[2].mEvents.size() == 2 && sink.mFrames[2].mEvents[0] == MakeEvent(9) &&
        sink.mFrames[2].mEvents[1] == MakeEvent(4), "dispatch appends after entry snapshot");

    game.mFrame = 3;
    game.mDueEvents.clear();
    Require(session.OnFrameBegin() && session.OnBeforeEvents() && session.OnFrameEnd() &&
        sink.mFrames[3].mEvents.empty(), "previous frame dispatches never leak forward");
    Require(session.Finish() && sink.mFinished && sink.mFrames.size() == 4, "recording finishes");
}

void TestDispatchCaptureFailures()
{
    {
        FakeGame game;
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && session.OnFrameBegin() &&
            !session.OnEventDispatched(MakeEvent(1)) &&
            session.GetState() == ReplayState::Failed && sink.mAborted && sink.mFrames.empty() &&
            game.mEndCount == 0, "dispatch before entry fails without ending live game");
    }
    {
        FakeGame game;
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && session.OnFrameBegin() && session.OnBeforeEvents(),
            "frame begins");
        game.mFrame = 1;
        Require(!session.OnEventDispatched(MakeEvent(1)) &&
            session.GetLastError() == "Event dispatch frame changed" && sink.mAborted,
            "dispatch in another frame fails");
    }
    {
        FakeGame game;
        ReplaySession session(game);
        Require(session.OnEventDispatched(MakeEvent(1)) && session.GetState() == ReplayState::Inactive,
            "inactive session ignores dispatch");
    }
    {
        FakeGame game;
        FakeSource source;
        source.mFrames.resize(1);
        ReplaySession session(game);
        Require(session.StartPlayback(source) && session.OnFrameBegin() &&
            session.OnEventDispatched(MakeEvent(1)) && session.GetState() == ReplayState::Playing &&
            game.mExecuted.empty(), "playback ignores live dispatch");
    }
}

void TestDispatchCaptureEventLimit()
{
    FakeGame game;
    FakeSink sink;
    ReplaySession session(game);
    Require(session.StartRecording(sink) && session.OnFrameBegin() && session.OnBeforeEvents(),
        "limit frame begins");
    bool accepted = true;
    for (std::uint32_t index = 0; index < 16'384; ++index)
        accepted = accepted && session.OnEventDispatched(MakeEvent(static_cast<std::uint8_t>(index)));
    Require(accepted && session.OnFrameEnd() && sink.mFrames.size() == 1 &&
        sink.mFrames[0].mEvents.size() == 16'384, "exactly 16384 events fit one frame");

    game.mFrame = 1;
    game.mDueEvents.assign(16'384, MakeEvent(7));
    Require(session.OnFrameBegin() && session.OnBeforeEvents() &&
        !session.OnEventDispatched(MakeEvent(8)) &&
        session.GetLastError() == "Recording frame exceeds the event limit" &&
        session.GetState() == ReplayState::Failed && sink.mAborted && sink.mFrames.size() == 1,
        "entry and dispatched events share the frame limit");
}

void TestPlaybackEndAndNoLiveFallback()
{
    FakeGame game;
    FakeSource source;
    ReplayFrame empty;
    ReplayFrame ordered;
    ordered.mFrameNumber = 1;
    ordered.mChecksum = 0x777;
    ordered.mHasChecksum = true;
    RecordedEvent first{};
    RecordedEvent second{};
    first[0] = 3;
    second[0] = 4;
    ordered.mEvents = {first, second};
    RecordedInputEffect feedback;
    feedback.mX = -77;
    ordered.mInputEffects.push_back(feedback);
    source.mFrames = {empty, ordered};
    ReplaySession session(game);
    Require(session.StartPlayback(source), "matching metadata should start playback");
    Require(session.OnFrameBegin() && session.OnFrameBegin() && source.mReadCount == 1 &&
        game.mApplyCount == 0 && session.OnBeforeSimulation() &&
        session.OnBeforeSimulation() && game.mApplyCount == 1 &&
        session.ProcessPlaybackFrame() && session.ProcessPlaybackFrame() &&
        session.OnFrameBegin() && session.OnBeforeSimulation() && game.mApplyCount == 1 &&
        game.mExecuted.size() == 1, "paused frame must not reread or execute twice");
    game.mFrame = 1;
    Require(session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && game.mExecuted.size() == 2 &&
        game.mApplyCount == 2 && game.mInputEffects.size() == 1 &&
        game.mInputEffects[0].mX == -77 &&
        game.mExecuted[1].mEvents[0][0] == 3 && game.mExecuted[1].mEvents[1][0] == 4 &&
        game.mExecuted[1].mChecksum == 0x777, "playback hands ordered frame to engine");
    Require(game.mPhases.size() == 6 && game.mPhases[0] == "applyEffects" &&
        game.mPhases[1] == "discardCommands" && game.mPhases[2] == "executeFrame" &&
        game.mPhases[3] == "applyEffects" && game.mPhases[4] == "discardCommands" &&
        game.mPhases[5] == "executeFrame", "effects must apply once before Logic execution");
    Require(session.OnFrameEnd(), "last frame completes before EOF probing");
    game.mFrame = 2;
    Require(session.OnAfterFrameLoop() && session.GetState() == ReplayState::Ended &&
        game.mEndCount == 0 && source.mReadCount == 3 &&
        game.mExecuted.size() == 2 && session.OnFrameBegin() && source.mReadCount == 3 &&
        session.ProcessPlaybackFrame() && session.IsPlaybackHeld() &&
        !session.SetPaused(false) && session.GetLastCompletedFrame() == 1,
        "EOF must hold the last complete frame without ending the game or resuming");
    session.Stop();
    session.Stop();
    Require(game.mEndCount == 1, "explicit stop ends a held replay exactly once");
}

void TestPauseAndPrefetch()
{
    FakeGame game;
    FakeSource source;
    source.mFrames.resize(2);
    source.mFrames[1].mFrameNumber = 1;
    ReplaySession session(game);
    Require(session.StartPlayback(source) && session.OnFrameBegin() && session.SetPaused(true),
        "playback pauses with the original prefetched frame");
    for (int iteration = 0; iteration < 10; ++iteration)
        Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.ProcessPlaybackFrame(),
            "held hooks remain playback without simulation");
    Require(source.mReadCount == 1 && game.mApplyCount == 0 && game.mExecuted.empty(),
        "pause does not reread, apply effects or execute events");
    Require(session.SetPaused(false) && session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.OnFrameEnd(), "resume executes original pending frame");
    game.mFrame = 1;
    Require(session.OnAfterFrameLoop() && session.OnAfterFrameLoop() && source.mReadCount == 2 &&
        session.SetPaused(true) && session.OnFrameBegin() && session.SetPaused(false) &&
        session.OnFrameBegin() && source.mReadCount == 2 && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.OnFrameEnd(),
        "completed-frame prefetch survives pause and runs once");
    game.mFrame = 2;
    source.mReadFails = true;
    Require(!session.OnAfterFrameLoop() && session.GetState() == ReplayState::Failed &&
        game.mEndCount == 1, "post-frame corruption exits with an error instead of normal EOF");
}

void RequireMetadataFailure(const ReplaySessionInfo& recorded,
    const ReplaySessionInfo& restored, const char* expectedDetail)
{
    FakeGame game;
    game.mInfo = restored;
    FakeSource source;
    source.mInfo = recorded;
    ReplaySession session(game);
    Require(!session.StartPlayback(source) && session.GetState() == ReplayState::Failed &&
        game.mEndCount == 1 && session.ProcessPlaybackFrame() &&
        session.GetLastError().find(expectedDetail) != std::string::npos,
        "mismatched metadata must identify recorded and restored values and end replay");
}

void TestPlaybackFailures()
{
    RequireMetadataFailure({2, 1, 0, 6, 42, 1}, {2, 1, 0, 6, 42},
        "旧录影缺少玩家输入效果");
    RequireMetadataFailure({3, 1, 0, 6, 42}, {2, 1, 0, 6, 42},
        "mode(recorded=3, restored=2)");
    RequireMetadataFailure({2, 2, 0, 6, 42}, {2, 1, 0, 6, 42},
        "localHouse(recorded=2, restored=1)");
    RequireMetadataFailure({2, 1, 1, 6, 42}, {2, 1, 0, 6, 42},
        "firstFrame(recorded=1, restored=0)[required=0]");
    RequireMetadataFailure({2, 1, 1, 6, 42}, {2, 1, 1, 6, 42},
        "firstFrame(recorded=1, restored=1)[required=0]");
    RequireMetadataFailure({2, 1, 0, 7, 42}, {2, 1, 0, 6, 42},
        "frameSendRate(recorded=7, restored=6)");
    RequireMetadataFailure({2, 1, 0, 6, 43}, {2, 1, 0, 6, 42},
        "randomSeed(recorded=43, restored=42)");
    {
        FakeGame game;
        FakeSource source;
        source.mFrames.resize(1);
        ReplaySession session(game);
        Require(session.StartPlayback(source) && session.OnFrameBegin() &&
            session.ProcessPlaybackFrame() && session.GetState() == ReplayState::Failed &&
            game.mApplyCount == 0 && game.mExecuted.empty() && game.mEndCount == 1,
            "frame execution must require the pre-simulation effect phase");
    }
    {
        FakeGame game;
        FakeSource source;
        source.mFrames.resize(1);
        game.mApplyEffectsFails = true;
        ReplaySession session(game);
        Require(session.StartPlayback(source) && session.OnFrameBegin() &&
            !session.OnBeforeSimulation() &&
            session.GetState() == ReplayState::Failed && game.mEndCount == 1 &&
            game.mExecuted.empty() && game.mApplyCount == 1 &&
            session.GetLastError() == "effect apply failed",
            "effect application failure must stop before frame execution");
    }
    {
        FakeGame game;
        FakeSource source;
        ReplayFrame skipped;
        skipped.mFrameNumber = 1;
        source.mFrames = {skipped};
        ReplaySession session(game);
        Require(session.StartPlayback(source) && !session.OnFrameBegin() &&
            session.GetState() == ReplayState::Failed && game.mEndCount == 1 &&
            game.mExecuted.empty() && session.ProcessPlaybackFrame(),
            "missing frame must fail at begin without live fallback");
    }
    {
        FakeGame game;
        FakeSource source;
        source.mReadFails = true;
        ReplaySession session(game);
        Require(session.StartPlayback(source) && !session.OnFrameBegin() &&
            session.GetState() == ReplayState::Failed && game.mEndCount == 1,
            "source error must end replay");
    }
    {
        FakeGame game;
        FakeSource source;
        source.mFrames.resize(1);
        game.mExecutionFails = true;
        ReplaySession session(game);
        Require(session.StartPlayback(source) && session.OnFrameBegin() &&
            session.OnBeforeSimulation() &&
            session.ProcessPlaybackFrame() &&
            session.GetState() == ReplayState::Failed && game.mEndCount == 1 &&
            session.ProcessPlaybackFrame(), "engine CRC failure must not enter live network");
    }
}

void TestGroupRecordingPortAndFailure()
{
    FakeGame game;
    game.mInfo.mStreamVersion = 3;
    game.mGroupUpdate.emplace();
    FakeSink sink;
    ReplaySession session(game);
    Require(session.StartRecording(sink) && session.OnFrameBegin() && session.OnFrameEnd() &&
        sink.mFrames.size() == 1 && sink.mFrames[0].mRecordedGroups &&
        sink.mFrames[0].mRecordedGroups->mMembers.empty() && game.mCopyGroupsCount == 1,
        "v3 recording appends an explicit initial empty snapshot");
    game.mFrame = 1;
    game.mGroupUpdate = RecordedControlGroupSnapshot{{{0, 101, 52}}};
    Require(session.OnFrameBegin() && session.OnFrameEnd() &&
        sink.mFrames[1].mRecordedGroups == game.mGroupUpdate,
        "group capture completes before frame append");
    game.mFrame = 2;
    game.mCopyGroupsFails = true;
    Require(session.OnFrameBegin() && !session.OnFrameEnd() && sink.mAborted &&
        sink.mFrames.size() == 2 && session.GetState() == ReplayState::Failed &&
        game.mEndCount == 0 && session.GetLastError() == "group capture failed",
        "group capture failure aborts recording without touching live simulation");

    FakeGame missing;
    missing.mInfo.mStreamVersion = 3;
    FakeSink missingSink;
    ReplaySession missingSession(missing);
    Require(missingSession.StartRecording(missingSink) && missingSession.OnFrameBegin() &&
        !missingSession.OnFrameEnd() && missingSink.mAborted && missingSink.mFrames.empty(),
        "session validates missing initial snapshot even with a fake sink");

    FakeGame legacy;
    legacy.mCopyGroupsFails = true;
    FakeSink legacySink;
    ReplaySession legacySession(legacy);
    Require(legacySession.StartRecording(legacySink) && legacySession.OnFrameBegin() &&
        legacySession.OnFrameEnd() && legacy.mCopyGroupsCount == 0 &&
        !legacySink.mFrames[0].mRecordedGroups, "v2 recording never calls new capture port");
}

void TestCompletedGroupHistoryAndPrefetch()
{
    FakeGame game;
    FakeSource source;
    source.mInfo.mStreamVersion = 3;
    source.mFrames.resize(4);
    for (std::size_t index = 0; index < source.mFrames.size(); ++index)
        source.mFrames[index].mFrameNumber = static_cast<std::uint32_t>(index);
    source.mFrames[0].mRecordedGroups.emplace();
    source.mFrames[1].mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}, {9, 202, 52}}};
    source.mFrames[3].mRecordedGroups.emplace();
    ReplaySession session(game);
    Require(session.StartPlayback(source) && !session.GetRecordedControlGroups() &&
        session.OnFrameBegin() && !session.GetRecordedControlGroups() &&
        session.OnBeforeSimulation() && session.ProcessPlaybackFrame() &&
        !session.GetRecordedControlGroups(), "prefetch and execution cannot publish initial groups early");
    Require(session.OnFrameEnd() && session.GetRecordedControlGroups() &&
        session.GetRecordedControlGroups()->mMembers.empty(), "initial empty history appears at frame end");
    game.mFrame = 1;
    Require(session.OnAfterFrameLoop() && session.OnAfterFrameLoop() && source.mReadCount == 2 &&
        session.GetRecordedControlGroups()->mMembers.empty() && session.SetPaused(true),
        "post-frame prefetch leaves previous history untouched");
    for (int poll = 0; poll < 3; ++poll)
        Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.ProcessPlaybackFrame() &&
            session.GetRecordedControlGroups()->mMembers.empty(), "pause cannot apply next frame groups");
    Require(session.SetPaused(false) && session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.GetRecordedControlGroups()->mMembers.empty() &&
        session.OnFrameEnd() && session.GetRecordedControlGroups() == source.mFrames[1].mRecordedGroups,
        "group update is published only after successful full frame completion");
    const auto members = session.GetRecordedControlGroups();
    Require(session.OnFrameEnd() && session.GetRecordedControlGroups() == members,
        "repeated frame-end notification is idempotent");
    game.mFrame = 2;
    Require(session.OnAfterFrameLoop() && session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.OnFrameEnd() &&
        session.GetRecordedControlGroups() == members, "missing update keeps previous group table");
    game.mFrame = 3;
    Require(session.OnAfterFrameLoop() && session.GetRecordedControlGroups() == members &&
        session.OnFrameBegin() && session.OnBeforeSimulation() && session.ProcessPlaybackFrame() &&
        session.GetRecordedControlGroups() == members && session.OnFrameEnd() &&
        session.GetRecordedControlGroups() && session.GetRecordedControlGroups()->mMembers.empty(),
        "empty present snapshot clears only at completion");
    game.mFrame = 4;
    Require(session.OnAfterFrameLoop() && session.GetState() == ReplayState::Ended &&
        session.GetRecordedControlGroups() && session.GetRecordedControlGroups()->mMembers.empty() &&
        game.mCopyGroupsCount == 0, "EOF retains final history and playback never captures live groups");
    session.Stop();
    Require(!session.GetRecordedControlGroups(), "stop resets group history");
}

void TestGroupPlaybackFailuresAndLegacy()
{
    for (const bool emptyStream : {false, true})
    {
        FakeGame game;
        FakeSource source;
        source.mInfo.mStreamVersion = 3;
        if (!emptyStream) source.mFrames.resize(1);
        ReplaySession session(game);
        Require(session.StartPlayback(source) && !session.OnFrameBegin() &&
            session.GetState() == ReplayState::Failed && !session.GetRecordedControlGroups() &&
            game.mExecuted.empty(), "v3 empty stream or missing initial update fails before simulation");
    }
    FakeGame game;
    FakeSource source;
    source.mInfo.mStreamVersion = 3;
    source.mFrames.resize(2);
    source.mFrames[0].mRecordedGroups = RecordedControlGroupSnapshot{{{0, 101, 52}}};
    source.mFrames[1].mFrameNumber = 1;
    source.mFrames[1].mRecordedGroups = RecordedControlGroupSnapshot{{{9, 202, 52}}};
    ReplaySession session(game);
    Require(session.StartPlayback(source) && session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.OnFrameEnd() && session.GetRecordedControlGroups(),
        "first group frame completes");
    game.mFrame = 1;
    game.mExecutionFails = true;
    Require(session.OnAfterFrameLoop() && session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.GetState() == ReplayState::Failed &&
        !session.GetRecordedControlGroups(), "execution failure resets history without partial update publication");

    FakeGame legacy;
    FakeSource oldSource;
    oldSource.mFrames.resize(1);
    ReplaySession oldSession(legacy);
    Require(oldSession.StartPlayback(oldSource) && oldSession.OnFrameBegin() &&
        oldSession.OnBeforeSimulation() && oldSession.ProcessPlaybackFrame() && oldSession.OnFrameEnd() &&
        !oldSession.GetRecordedControlGroups(), "old v2 playback has no group history");
    FakeGame injected;
    FakeSource invalidSource;
    invalidSource.mFrames.resize(1);
    invalidSource.mFrames[0].mRecordedGroups.emplace();
    ReplaySession invalidSession(injected);
    Require(invalidSession.StartPlayback(invalidSource) && !invalidSession.OnFrameBegin() &&
        injected.mExecuted.empty(), "v2 fake source cannot smuggle in group history");
}

class CountingObserver final : public IReplayFrameObserver
{
public:
    std::vector<std::uint32_t> mFrames;
    std::vector<std::size_t> mEventCounts;
    void OnPlaybackFrameExecuted(const ReplayFrame& frame) noexcept override
    {
        mFrames.push_back(frame.mFrameNumber);
        mEventCounts.push_back(frame.mEvents.size());
    }
};

// 观察者在每个帧被成功执行后通知一次：重复进入同一帧不重复通知，执行失败不通知，取消后不再通知。
void TestFrameObserverNotifiedAfterExecution()
{
    FakeGame game;
    FakeSource source;
    source.mFrames.resize(3);
    source.mFrames[0].mEvents = {MakeEvent(1), MakeEvent(2)};
    source.mFrames[1].mFrameNumber = 1;
    source.mFrames[2].mFrameNumber = 2;
    CountingObserver observer;
    ReplaySession session(game);
    session.SetFrameObserver(&observer);
    Require(session.StartPlayback(source) && session.OnFrameBegin() && session.OnBeforeSimulation(),
        "observer playback starts");
    Require(session.ProcessPlaybackFrame() && session.ProcessPlaybackFrame() &&
        observer.mFrames == std::vector<std::uint32_t>{0} && observer.mEventCounts[0] == 2,
        "executed frame is reported exactly once with its events");
    Require(session.OnFrameEnd() && session.OnAfterFrameLoop(), "first frame completes");

    game.mFrame = 1;
    Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.ProcessPlaybackFrame() &&
        observer.mFrames.size() == 2 && observer.mFrames[1] == 1, "second frame is reported");
    Require(session.OnFrameEnd() && session.OnAfterFrameLoop(), "second frame completes");

    session.SetFrameObserver(nullptr);
    game.mFrame = 2;
    Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.ProcessPlaybackFrame() &&
        observer.mFrames.size() == 2, "detached observer is not called");

    FakeGame failing;
    failing.mExecutionFails = true;
    FakeSource failingSource;
    failingSource.mFrames.resize(1);
    CountingObserver failedObserver;
    ReplaySession failedSession(failing);
    failedSession.SetFrameObserver(&failedObserver);
    Require(failedSession.StartPlayback(failingSource) && failedSession.OnFrameBegin() &&
        failedSession.OnBeforeSimulation() && failedSession.ProcessPlaybackFrame() &&
        failedSession.GetState() == ReplayState::Failed && failedObserver.mFrames.empty(),
        "failed execution is not reported");
}
}

namespace
{
ReplaySessionInfo OfflineV4Info()
{
    return {5, 1, 0, 6, 42, 4};
}

/** 按原生帧序驱动一帧录制：帧开始 → 模拟前 → 事件入口 → 帧尾。 */
bool RecordFrame(ReplaySession& session, FakeGame& game, std::uint32_t frame)
{
    game.mFrame = frame;
    return session.OnFrameBegin() && session.OnBeforeSimulation() && session.OnBeforeEvents() &&
        session.OnFrameEnd();
}

/** 按原生帧序驱动一帧播放并预取下一帧。 */
bool PlayFrame(ReplaySession& session, FakeGame& game, std::uint32_t frame)
{
    game.mFrame = frame;
    const bool executed = session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.OnFrameEnd();
    game.mFrame = frame + 1;
    return executed && session.OnAfterFrameLoop() && session.GetState() != ReplayState::Failed;
}

bool OnlyRecordingPhases(const FakeGame& game)
{
    for (const auto& phase : game.mPhases)
    {
        if (phase != "readSpeed" && phase != "copyEffects")
            return false;
    }
    return true;
}

void TestRecordingSamplesGameSpeed()
{
    FakeGame game;
    game.mInfo = OfflineV4Info();
    game.mHasChecksum = false;
    game.mGroupUpdate.emplace();
    FakeSink sink;
    ReplaySession session(game);
    Require(session.StartRecording(sink), "offline v4 recording starts");

    // 帧 0：重复进入同一帧与重复模拟前调用都只形成一条记录。
    Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.OnFrameBegin() &&
        session.OnBeforeSimulation() && session.OnBeforeEvents() &&
        session.OnEventDispatched(MakeEvent(1)) && session.OnFrameEnd(), "frame 0 records");
    Require(sink.mFrames.size() == 1 && sink.mFrames[0].mGameSpeed == 2u &&
        sink.mFrames[0].mEvents.size() == 1, "frame 0 always carries the sampled speed");
    game.mGroupUpdate.reset();
    Require(RecordFrame(session, game, 1) && RecordFrame(session, game, 2) &&
        !sink.mFrames[1].mGameSpeed && !sink.mFrames[2].mGameSpeed, "an unchanged speed is not repeated");

    // 第 3 帧开始前（帧间的模态对话框里）速度由 2 改为 4。
    game.mGameSpeed = 4;
    game.mFrame = 3;
    Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.OnBeforeEvents() &&
        session.OnEventDispatched(MakeEvent(3)) && session.OnFrameEnd(), "frame 3 records");
    Require(sink.mFrames[3].mGameSpeed == 4u && sink.mFrames[3].mEvents.size() == 1 &&
        sink.mFrames[3].mEvents[0] == MakeEvent(3), "the event entry keeps the speed sampled before simulation");
    Require(RecordFrame(session, game, 4) && !sink.mFrames[4].mGameSpeed, "the new speed is written once");

    // 同一帧重复进入时以最后一次采样为准；回到已提交的值就不写。
    game.mFrame = 5;
    game.mGameSpeed = 6;
    Require(session.OnFrameBegin() && session.OnBeforeSimulation(), "frame 5 samples");
    game.mGameSpeed = 4;
    Require(session.OnBeforeSimulation() && session.OnBeforeEvents() && session.OnFrameEnd() &&
        !sink.mFrames[5].mGameSpeed, "the last sample of a re-entered frame wins");
    Require(session.Finish() && sink.mFrames.size() == 6 && game.mEndCount == 0, "recording finishes");

    const std::vector<std::optional<std::uint32_t>> expected = {2u, std::nullopt, std::nullopt, 4u,
        std::nullopt, std::nullopt};
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        std::vector<std::uint8_t> bytes;
        std::string error;
        Require(sink.mFrames[index].mGameSpeed == expected[index] &&
            FrameCodec::ValidateGameSpeed(sink.mFrames[index], 4, 5, error) &&
            FrameCodec::EncodeFrame(sink.mFrames[index], bytes, error, 4),
            "recorded speed sequence is 2, -, -, 4, -, - and encodes as v4");
    }
    // 录制时模拟前只读速度，不写速度、不应用输入效果、不碰事件队列。
    Require(game.mAppliedSpeeds.empty() && game.mApplyCount == 0 && game.mDiscardCount == 0 &&
        game.mReadSpeedCount == 8 && OnlyRecordingPhases(game), "recording sampling has no other side effect");
}

void TestOnlineAndLegacyRecordingNeverSample()
{
    for (const ReplaySessionInfo& info : {ReplaySessionInfo{3, 1, 0, 6, 42, 4},
             ReplaySessionInfo{5, 1, 0, 6, 42, 3}, ReplaySessionInfo{5, 1, 0, 6, 42, 2},
             ReplaySessionInfo{0, 1, 0, 6, 42, 3}})
    {
        FakeGame game;
        game.mInfo = info;
        game.mGroupUpdate.emplace();
        game.mGameSpeed = 7;
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && RecordFrame(session, game, 0) &&
            RecordFrame(session, game, 1) && session.Finish(), "online or legacy recording runs");
        Require(game.mReadSpeedCount == 0 && sink.mFrames.size() == 2 && !sink.mFrames[0].mGameSpeed &&
            !sink.mFrames[1].mGameSpeed, "online and pre-v4 recordings never sample or write a speed");
    }
}

void TestRecordingRejectsInvalidSpeed()
{
    for (const auto& [speed, detail] : {std::pair<std::int32_t, const char*>{7, "speed=7"},
             std::pair<std::int32_t, const char*>{-1, "speed=-1"}})
    {
        FakeGame game;
        game.mInfo = OfflineV4Info();
        game.mGroupUpdate.emplace();
        game.mGameSpeed = speed;
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && session.OnFrameBegin() && !session.OnBeforeSimulation() &&
            session.GetState() == ReplayState::Failed && sink.mAborted && sink.mFrames.empty() &&
            game.mEndCount == 0 && session.GetLastError().find(detail) != std::string::npos &&
            session.GetLastError().find("frame=0") != std::string::npos,
            "an out-of-range speed fails recording with the frame and value instead of clamping");
    }
    {
        FakeGame game;
        game.mInfo = OfflineV4Info();
        game.mGroupUpdate.emplace();
        game.mFrame = 0;
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && RecordFrame(session, game, 0), "first frame records");
        game.mGameSpeed = 9;
        game.mFrame = 1;
        Require(session.OnFrameBegin() && !session.OnBeforeSimulation() && sink.mAborted &&
            sink.mFrames.size() == 1 && session.GetLastError().find("frame=1 speed=9") != std::string::npos,
            "a later out-of-range speed also fails recording");
    }
    {
        FakeGame game;
        game.mInfo = OfflineV4Info();
        game.mGroupUpdate.emplace();
        game.mReadSpeedFails = true;
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && session.OnFrameBegin() && !session.OnBeforeSimulation() &&
            session.GetLastError() == "speed read failed" && sink.mAborted, "a speed read failure fails recording");
    }
    {
        FakeGame game;
        game.mInfo = OfflineV4Info();
        game.mGroupUpdate.emplace();
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && session.OnFrameBegin() && session.OnBeforeEvents() &&
            !session.OnFrameEnd() && sink.mAborted && sink.mFrames.empty(),
            "an offline v4 frame 0 that skipped the sampling point is not committed");
    }
}

ReplayFrame SpeedFrame(std::uint32_t frameNumber, std::optional<std::uint32_t> speed)
{
    ReplayFrame frame;
    frame.mFrameNumber = frameNumber;
    frame.mGameSpeed = speed;
    if (frameNumber == 0)
        frame.mRecordedGroups.emplace();
    RecordedInputEffect effect;
    effect.mX = static_cast<std::int32_t>(frameNumber);
    frame.mInputEffects.push_back(effect);
    return frame;
}

void TestPlaybackAppliesSpeedBeforeEffects()
{
    FakeGame game;
    game.mInfo = OfflineV4Info();
    FakeSource source;
    source.mInfo = OfflineV4Info();
    source.mFrames = {SpeedFrame(0, 2), SpeedFrame(1, std::nullopt), SpeedFrame(2, std::nullopt),
        SpeedFrame(3, 4), SpeedFrame(4, std::nullopt)};
    ReplaySession session(game);
    Require(session.StartPlayback(source), "offline v4 playback starts");
    Require(PlayFrame(session, game, 0) && !session.TakeInitialGameSpeedDifference(),
        "frame 0 plays without a speed difference");
    // 帧 1 在模拟前重复进入：速度与输入效果都只写一次。
    game.mFrame = 1;
    Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.OnBeforeSimulation() &&
        session.ProcessPlaybackFrame() && session.OnFrameEnd(), "frame 1 plays");
    game.mFrame = 2;
    Require(session.OnAfterFrameLoop() && PlayFrame(session, game, 2) && PlayFrame(session, game, 3) &&
        PlayFrame(session, game, 4) && session.GetState() == ReplayState::Ended, "all speed frames play");
    Require(game.mAppliedSpeeds == std::vector<std::uint32_t>({2, 2, 2, 4, 4}),
        "the recorded speed is written every frame from frame 0 and follows the change");

    std::vector<std::string> expected = {"readSpeed"};
    for (int frame = 0; frame < 5; ++frame)
        for (const char* phase : {"applySpeed", "applyEffects", "discardCommands", "executeFrame"})
            expected.push_back(phase);
    Require(game.mPhases == expected, "speed is written before the input effects of every frame");
    Require(!session.TakeInitialGameSpeedDifference(), "no difference is reported later");
}

void TestPlaybackSpeedDifferenceReportedOnce()
{
    FakeGame game;
    game.mInfo = OfflineV4Info();
    game.mGameSpeed = 3;
    FakeSource source;
    source.mInfo = OfflineV4Info();
    source.mFrames = {SpeedFrame(0, 2), SpeedFrame(1, std::nullopt), SpeedFrame(2, 3)};
    ReplaySession session(game);
    Require(session.StartPlayback(source) && session.OnFrameBegin() && session.OnBeforeSimulation() &&
        session.OnBeforeSimulation(), "frame 0 applies the recorded speed");
    const auto difference = session.TakeInitialGameSpeedDifference();
    Require(difference && difference->mRecorded == 2 && difference->mCurrent == 3 &&
        game.mAppliedSpeeds == std::vector<std::uint32_t>({2}), "frame 0 reports the restored speed difference");
    Require(!session.TakeInitialGameSpeedDifference(), "the difference is taken only once");
    Require(session.ProcessPlaybackFrame() && session.OnFrameEnd(), "frame 0 completes");
    game.mFrame = 1;
    Require(session.OnAfterFrameLoop(), "frame 1 prefetches");
    game.mGameSpeed = 5;
    Require(PlayFrame(session, game, 1) && PlayFrame(session, game, 2) &&
        !session.TakeInitialGameSpeedDifference() && game.mReadSpeedCount == 1,
        "later frames neither read nor report differences");
    Require(game.mAppliedSpeeds == std::vector<std::uint32_t>({2, 2, 3}),
        "later frames overwrite stray writes with the recorded speed");
}

void TestLegacyAndOnlinePlaybackNeverApplySpeed()
{
    for (const ReplaySessionInfo& info : {ReplaySessionInfo{5, 1, 0, 6, 42, 3},
             ReplaySessionInfo{5, 1, 0, 6, 42, 2}, ReplaySessionInfo{3, 1, 0, 6, 42, 4}})
    {
        FakeGame game;
        game.mInfo = info;
        FakeSource source;
        source.mInfo = info;
        source.mFrames = {SpeedFrame(0, std::nullopt), SpeedFrame(1, std::nullopt)};
        if (info.mStreamVersion < 3)
            source.mFrames[0].mRecordedGroups.reset();
        ReplaySession session(game);
        Require(session.StartPlayback(source) && PlayFrame(session, game, 0) && PlayFrame(session, game, 1) &&
            session.GetState() == ReplayState::Ended, "legacy or online playback runs");
        Require(game.mAppliedSpeeds.empty() && game.mReadSpeedCount == 0 &&
            !session.TakeInitialGameSpeedDifference() && game.mApplyCount == 2,
            "v3 and earlier or online recordings never touch the game speed");
    }
}

void TestPlaybackSpeedFailures()
{
    {
        FakeGame game;
        game.mInfo = OfflineV4Info();
        game.mApplySpeedFails = true;
        FakeSource source;
        source.mInfo = OfflineV4Info();
        source.mFrames = {SpeedFrame(0, 2)};
        ReplaySession session(game);
        Require(session.StartPlayback(source) && session.OnFrameBegin() && !session.OnBeforeSimulation() &&
            session.GetState() == ReplayState::Failed && game.mEndCount == 1 && game.mApplyCount == 0 &&
            game.mExecuted.empty() && session.GetLastError() == "speed apply failed",
            "a speed write failure stops before the input effects");
    }
    {
        FakeGame game;
        game.mInfo = OfflineV4Info();
        game.mReadSpeedFails = true;
        FakeSource source;
        source.mInfo = OfflineV4Info();
        source.mFrames = {SpeedFrame(0, 2)};
        ReplaySession session(game);
        Require(session.StartPlayback(source) && session.OnFrameBegin() && !session.OnBeforeSimulation() &&
            session.GetState() == ReplayState::Failed && game.mAppliedSpeeds.empty() && game.mApplyCount == 0,
            "a frame 0 speed read failure follows the playback failure path");
    }
    const auto rejectsAtFrame = [](const ReplaySessionInfo& info, std::vector<ReplayFrame> frames,
        std::uint32_t failingFrame, const char* message)
    {
        FakeGame game;
        game.mInfo = info;
        FakeSource source;
        source.mInfo = info;
        source.mFrames = std::move(frames);
        ReplaySession session(game);
        Require(session.StartPlayback(source), message);
        for (std::uint32_t frame = 0; frame < failingFrame; ++frame)
        {
            game.mFrame = frame;
            Require(session.OnFrameBegin() && session.OnBeforeSimulation() && session.ProcessPlaybackFrame() &&
                session.OnFrameEnd(), message);
            // 帧尾之后预取下一帧，无效记录在这里即被拒绝。
            game.mFrame = frame + 1;
            session.OnAfterFrameLoop();
        }
        game.mFrame = failingFrame;
        Require(session.GetState() == ReplayState::Failed || !session.OnFrameBegin(), message);
        Require(session.GetState() == ReplayState::Failed && game.mEndCount == 1 &&
            game.mExecuted.size() == failingFrame, message);
    };
    rejectsAtFrame(OfflineV4Info(), {SpeedFrame(0, std::nullopt)}, 0,
        "an offline v4 source without a frame 0 speed fails before simulation");
    rejectsAtFrame(ReplaySessionInfo{5, 1, 0, 6, 42, 3}, {SpeedFrame(0, std::nullopt), SpeedFrame(1, 4)}, 1,
        "a v3 fake source cannot smuggle in a speed");
    rejectsAtFrame(ReplaySessionInfo{3, 1, 0, 6, 42, 4}, {SpeedFrame(0, std::nullopt), SpeedFrame(1, 4)}, 1,
        "an online v4 source cannot carry a speed");
    rejectsAtFrame(OfflineV4Info(), {SpeedFrame(0, 2), SpeedFrame(1, 7)}, 1,
        "an out-of-range speed from a fake source is rejected");
}

// v5 的会话行为与 v4 相同（单机采样与写回速度，联机不碰速度）；版本 6 在两端都被拒绝。
void TestVersion5RecordingSamplesOfflineSpeedOnly()
{
    for (const std::uint32_t mode : {0u, 5u})
    {
        FakeGame game;
        game.mInfo = ReplaySessionInfo{mode, 1, 0, 6, 42, 5};
        game.mHasChecksum = false;
        game.mGroupUpdate.emplace();
        FakeSink sink;
        ReplaySession session(game);
        Require(session.StartRecording(sink) && RecordFrame(session, game, 0), "offline v5 recording starts");
        game.mGroupUpdate.reset();
        game.mGameSpeed = 4;
        Require(RecordFrame(session, game, 1) && RecordFrame(session, game, 2) && session.Finish(),
            "offline v5 recording finishes");
        Require(sink.mFrames.size() == 3 && sink.mFrames[0].mGameSpeed == 2u && sink.mFrames[1].mGameSpeed == 4u &&
            !sink.mFrames[2].mGameSpeed && game.mReadSpeedCount == 3, "offline v5 recording samples the speed");
        for (const auto& frame : sink.mFrames)
        {
            std::vector<std::uint8_t> v4Bytes;
            std::vector<std::uint8_t> v5Bytes;
            std::string error;
            Require(FrameCodec::ValidateGameSpeed(frame, 5, mode, error) &&
                FrameCodec::EncodeFrame(frame, v5Bytes, error, 5) &&
                FrameCodec::EncodeFrame(frame, v4Bytes, error, 4) && v5Bytes == v4Bytes,
                "offline v5 frames validate and encode exactly as v4");
        }
    }
    FakeGame game;
    game.mInfo = ReplaySessionInfo{3, 1, 0, 6, 42, 5};
    game.mGroupUpdate.emplace();
    game.mGameSpeed = 7;
    FakeSink sink;
    ReplaySession session(game);
    Require(session.StartRecording(sink) && RecordFrame(session, game, 0) && RecordFrame(session, game, 1) &&
        session.Finish(), "online v5 recording runs");
    Require(game.mReadSpeedCount == 0 && sink.mFrames.size() == 2 && !sink.mFrames[0].mGameSpeed &&
        !sink.mFrames[1].mGameSpeed, "online v5 recording never samples or writes a speed");
}

void TestVersion5PlaybackAndVersion6Rejection()
{
    {
        const ReplaySessionInfo info{5, 1, 0, 6, 42, 5};
        FakeGame game;
        game.mInfo = info;
        FakeSource source;
        source.mInfo = info;
        source.mFrames = {SpeedFrame(0, 2), SpeedFrame(1, std::nullopt), SpeedFrame(2, 4)};
        ReplaySession session(game);
        Require(session.StartPlayback(source) && PlayFrame(session, game, 0) && PlayFrame(session, game, 1) &&
            PlayFrame(session, game, 2) && session.GetState() == ReplayState::Ended, "offline v5 playback runs");
        Require(game.mAppliedSpeeds == std::vector<std::uint32_t>({2, 2, 4}) && game.mExecuted.size() == 3,
            "offline v5 playback writes the recorded speed every frame");
    }
    {
        const ReplaySessionInfo info{3, 1, 0, 6, 42, 5};
        FakeGame game;
        game.mInfo = info;
        FakeSource source;
        source.mInfo = info;
        source.mFrames = {SpeedFrame(0, std::nullopt), SpeedFrame(1, std::nullopt)};
        ReplaySession session(game);
        Require(session.StartPlayback(source) && PlayFrame(session, game, 0) && PlayFrame(session, game, 1) &&
            session.GetState() == ReplayState::Ended && game.mAppliedSpeeds.empty() && game.mReadSpeedCount == 0,
            "online v5 playback runs without touching the speed");
    }
    RequireMetadataFailure({5, 1, 0, 6, 42, 6}, {5, 1, 0, 6, 42, 5}, "不支持此播放帧流版本");
    FakeGame game;
    game.mInfo = ReplaySessionInfo{5, 1, 0, 6, 42, 6};
    FakeSink sink;
    ReplaySession session(game);
    Require(!session.StartRecording(sink) && session.GetState() == ReplayState::Failed && sink.mAborted &&
        session.GetLastError() == "不支持此录制帧流版本", "version 6 recording is rejected");
}

// 进程内重开：播放、暂停或结束的会话复位后与新会话行为相同，复位本身从不调用 EndReplay；其余状态不复位。
void TestResetForRestart()
{
    {
        FakeGame game;
        ReplaySession session(game);
        Require(!session.ResetForRestart() && session.GetState() == ReplayState::Inactive,
            "an inactive session is not reset");
        FakeSink sink;
        Require(session.StartRecording(sink) && !session.ResetForRestart() &&
            session.GetState() == ReplayState::Recording && !sink.mAborted, "a recording is never reset");
    }
    {
        FakeGame game;
        FakeSource source;
        source.mReadFails = true;
        ReplaySession session(game);
        Require(session.StartPlayback(source) && !session.OnFrameBegin() &&
            session.GetState() == ReplayState::Failed && !session.ResetForRestart() &&
            session.GetState() == ReplayState::Failed, "a failed playback is not reset");
    }
    {
        FakeGame game;
        FakeSource source;
        source.mFrames.resize(1);
        ReplaySession session(game);
        Require(session.StartPlayback(source), "playback starts before the stop");
        session.Stop();
        Require(session.GetState() == ReplayState::Finished && !session.ResetForRestart(),
            "a stopped playback is not reset");
    }
    const char* const states[] = {"playing", "paused", "ended"};
    for (const char* state : states)
    {
        const std::string name = state;
        FakeGame game;
        game.mInfo = OfflineV4Info();
        game.mGameSpeed = 3;
        FakeSource source;
        source.mInfo = OfflineV4Info();
        source.mFrames = {SpeedFrame(0, 2), SpeedFrame(1, std::nullopt), SpeedFrame(2, std::nullopt)};
        CountingObserver observer;
        ReplaySession session(game);
        session.SetFrameObserver(&observer);
        Require(session.StartPlayback(source) && PlayFrame(session, game, 0) && PlayFrame(session, game, 1) &&
            session.TakeInitialGameSpeedDifference().has_value(), (name + ": the first run plays two frames").c_str());
        if (name == "paused")
            Require(session.SetPaused(true), "the first run pauses");
        if (name == "ended")
            Require(PlayFrame(session, game, 2) && session.GetState() == ReplayState::Ended, "the first run ends");

        Require(session.ResetForRestart(), (name + ": the session resets").c_str());
        Require(session.GetState() == ReplayState::Inactive && session.GetLastCompletedFrame() == 0 &&
            !session.GetRecordedControlGroups() && !session.IsPlaybackHeld() && game.mEndCount == 0,
            (name + ": the reset session is fresh and the game keeps running").c_str());
        Require(!session.ResetForRestart(), (name + ": a fresh session is not reset again").c_str());

        // 原生重开后帧号回到 0，读取器重新打开到第 0 帧，游戏速度又是开局时的值。
        FakeSource reopened;
        reopened.mInfo = OfflineV4Info();
        reopened.mFrames = {SpeedFrame(0, 2), SpeedFrame(1, std::nullopt)};
        game.mGameSpeed = 3;
        const auto executedBefore = game.mExecuted.size();
        const auto observedBefore = observer.mFrames.size();
        Require(session.StartPlayback(reopened) && PlayFrame(session, game, 0),
            (name + ": playback starts again from frame 0").c_str());
        Require(game.mExecuted.size() == executedBefore + 1 && game.mExecuted.back().mFrameNumber == 0,
            (name + ": frame 0 executes again").c_str());
        Require(observer.mFrames.size() == observedBefore,
            (name + ": the frame observer stays detached until viewing begins again").c_str());
        const auto difference = session.TakeInitialGameSpeedDifference();
        Require(difference && difference->mRecorded == 2 && difference->mCurrent == 3,
            (name + ": the frame 0 speed check runs again").c_str());
        Require(session.GetLastCompletedFrame() == 0 && session.GetRecordedControlGroups().has_value() &&
            session.GetState() == ReplayState::Playing, (name + ": frame 0 completes with its groups").c_str());
        session.Stop();
        Require(game.mEndCount == 1, (name + ": a later stop ends the game exactly once").c_str());
    }
}
}

int main()
{
    TestRecordCompleteFramesOnly();
    TestRecordingFailureDoesNotEndGame();
    TestDispatchCaptureCommitsAtFrameEnd();
    TestDispatchCaptureFailures();
    TestDispatchCaptureEventLimit();
    TestPlaybackEndAndNoLiveFallback();
    TestPauseAndPrefetch();
    TestPlaybackFailures();
    TestGroupRecordingPortAndFailure();
    TestCompletedGroupHistoryAndPrefetch();
    TestGroupPlaybackFailuresAndLegacy();
    TestFrameObserverNotifiedAfterExecution();
    TestRecordingSamplesGameSpeed();
    TestOnlineAndLegacyRecordingNeverSample();
    TestRecordingRejectsInvalidSpeed();
    TestPlaybackAppliesSpeedBeforeEffects();
    TestPlaybackSpeedDifferenceReportedOnce();
    TestLegacyAndOnlinePlaybackNeverApplySpeed();
    TestPlaybackSpeedFailures();
    TestVersion5RecordingSamplesOfflineSpeedOnly();
    TestVersion5PlaybackAndVersion6Rejection();
    TestResetForRestart();
}
