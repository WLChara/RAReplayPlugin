#include "../src/replay/ReplaySession.h"

#include <stdexcept>
#include <string>
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
}

int main()
{
    TestRecordCompleteFramesOnly();
    TestRecordingFailureDoesNotEndGame();
    TestPlaybackEndAndNoLiveFallback();
    TestPauseAndPrefetch();
    TestPlaybackFailures();
    TestGroupRecordingPortAndFailure();
    TestCompletedGroupHistoryAndPrefetch();
    TestGroupPlaybackFailuresAndLegacy();
}
