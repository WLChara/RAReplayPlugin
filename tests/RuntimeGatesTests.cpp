#include "../src/plugin/RuntimeGates.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::plugin::RuntimeGateState;
using rareplay::replay::ReplayState;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

constexpr std::array<ReplayState, 7> ALL_STATES{ReplayState::Inactive, ReplayState::Recording,
    ReplayState::Playing, ReplayState::Paused, ReplayState::Ended, ReplayState::Finished, ReplayState::Failed};

// 8 个布尔 × 7 个会话状态的全部组合，共 1792 种。
template <typename Visit>
void ForEachState(Visit visit)
{
    for (const auto replayState : ALL_STATES)
    {
        for (unsigned bits = 0; bits < (1u << 8); ++bits)
        {
            RuntimeGateState state;
            state.mReplayState = replayState;
            state.mIsStarted = (bits & 1u) != 0;
            state.mIsPlayback = (bits & 2u) != 0;
            state.mIsPlaybackSourceOpen = (bits & 4u) != 0;
            state.mHasStartFailed = (bits & 8u) != 0;
            state.mHasExitRequested = (bits & 16u) != 0;
            state.mHasViewFailed = (bits & 32u) != 0;
            state.mIsHalfShroudEnabled = (bits & 64u) != 0;
            state.mHasTotalFrames = (bits & 128u) != 0;
            visit(state);
        }
    }
}

// 以下参照式逐字取自拆分前 ReplayRuntime 的成员函数（mode 只有 Play 与 Record，Record 即非 Play）。
bool IsViewable(ReplayState state)
{
    return state == ReplayState::Playing || state == ReplayState::Paused || state == ReplayState::Ended;
}

bool ReferenceIsPlaybackViewActive(const RuntimeGateState& s)
{
    return s.mIsStarted && s.mIsPlayback && !s.mHasExitRequested && !s.mHasViewFailed && IsViewable(s.mReplayState);
}

bool ReferenceShouldUseHalfShroud(const RuntimeGateState& s)
{
    return s.mIsStarted && s.mIsPlayback && s.mIsHalfShroudEnabled && !s.mHasExitRequested &&
        IsViewable(s.mReplayState);
}

bool ReferenceIsRecordingActive(const RuntimeGateState& s)
{
    return s.mIsStarted && !s.mIsPlayback && s.mReplayState == ReplayState::Recording;
}

bool ReferenceIsRecordingStarted(const RuntimeGateState& s)
{
    return s.mIsStarted && !s.mIsPlayback;
}

bool ReferenceShouldBlockPlayerCommands(const RuntimeGateState& s)
{
    return s.mIsPlayback && s.mIsPlaybackSourceOpen && !s.mHasStartFailed;
}

bool ReferenceIsPlaybackPlaying(const RuntimeGateState& s)
{
    return s.mIsStarted && s.mIsPlayback && s.mReplayState == ReplayState::Playing;
}

// BEFORE_SIMULATION 门控：原先只在播放中调用会话，现在录制进行中也调用（会话只采样游戏速度）。
bool ReferenceShouldRunBeforeSimulation(const RuntimeGateState& s)
{
    return ReferenceIsPlaybackPlaying(s) ||
        (s.mIsStarted && !s.mIsPlayback && s.mReplayState == ReplayState::Recording);
}

// 可跳转：观看界面可用、播放或暂停（不含结束）、总帧数已知。
bool ReferenceCanSeekPlayback(const RuntimeGateState& s)
{
    return s.mIsStarted && s.mIsPlayback && !s.mHasExitRequested && !s.mHasViewFailed && s.mHasTotalFrames &&
        (s.mReplayState == ReplayState::Playing || s.mReplayState == ReplayState::Paused);
}

void CheckTruthTables()
{
    int count = 0;
    ForEachState([&count](const RuntimeGateState& s) {
        ++count;
        Require(rareplay::plugin::IsPlaybackViewActive(s) == ReferenceIsPlaybackViewActive(s),
            "IsPlaybackViewActive matches the original expression");
        Require(rareplay::plugin::ShouldUseHalfShroud(s) == ReferenceShouldUseHalfShroud(s),
            "ShouldUseHalfShroud matches the original expression");
        Require(rareplay::plugin::IsRecordingActive(s) == ReferenceIsRecordingActive(s),
            "IsRecordingActive matches the original expression");
        Require(rareplay::plugin::IsRecordingStarted(s) == ReferenceIsRecordingStarted(s),
            "IsRecordingStarted matches the original expression");
        Require(rareplay::plugin::ShouldBlockPlayerCommands(s) == ReferenceShouldBlockPlayerCommands(s),
            "ShouldBlockPlayerCommands matches the original expression");
        Require(rareplay::plugin::IsPlaybackPlaying(s) == ReferenceIsPlaybackPlaying(s),
            "IsPlaybackPlaying matches the original expression");
        Require(rareplay::plugin::ShouldRunBeforeSimulation(s) == ReferenceShouldRunBeforeSimulation(s),
            "ShouldRunBeforeSimulation is playing or active recording");
        // 播放侧的判定与改动前相同：回放模式下仍只在播放中交给会话。
        if (s.mIsPlayback)
            Require(rareplay::plugin::ShouldRunBeforeSimulation(s) == ReferenceIsPlaybackPlaying(s),
                "Playback keeps the original before-simulation gate");
        Require(rareplay::plugin::CanSeekPlayback(s) == ReferenceCanSeekPlayback(s),
            "CanSeekPlayback matches its definition");
        // 总帧数只影响可跳转判定，其余门控与它无关。
        auto flipped = s;
        flipped.mHasTotalFrames = !s.mHasTotalFrames;
        Require(rareplay::plugin::IsPlaybackViewActive(s) == rareplay::plugin::IsPlaybackViewActive(flipped) &&
                rareplay::plugin::ShouldUseHalfShroud(s) == rareplay::plugin::ShouldUseHalfShroud(flipped) &&
                rareplay::plugin::IsPlaybackPlaying(s) == rareplay::plugin::IsPlaybackPlaying(flipped) &&
                rareplay::plugin::ShouldBlockPlayerCommands(s) == rareplay::plugin::ShouldBlockPlayerCommands(flipped) &&
                rareplay::plugin::ShouldRunBeforeSimulation(s) == rareplay::plugin::ShouldRunBeforeSimulation(flipped),
            "The total frame count only affects seeking");
    });
    Require(count == 1792, "Every input combination is visited");
}

void CheckSeekGate()
{
    using rareplay::plugin::CanSeekPlayback;
    RuntimeGateState seekable;
    seekable.mIsStarted = true;
    seekable.mIsPlayback = true;
    seekable.mIsPlaybackSourceOpen = true;
    seekable.mHasTotalFrames = true;
    seekable.mReplayState = ReplayState::Playing;
    Require(CanSeekPlayback(seekable), "Playing with a known total can seek");
    auto paused = seekable;
    paused.mReplayState = ReplayState::Paused;
    Require(CanSeekPlayback(paused), "Paused with a known total can seek");
    // 半黑幕与命令拦截无关。
    auto shrouded = seekable;
    shrouded.mIsHalfShroudEnabled = true;
    Require(CanSeekPlayback(shrouded), "The half shroud does not matter");

    struct BlockedCase
    {
        const char* mName;
        void (*mChange)(RuntimeGateState&);
    };
    const BlockedCase cases[] = {
        {"unknown total", [](RuntimeGateState& s) { s.mHasTotalFrames = false; }},
        {"ended", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Ended; }},
        {"failed", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Failed; }},
        {"finished", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Finished; }},
        {"inactive", [](RuntimeGateState& s) { s.mReplayState = ReplayState::Inactive; }},
        {"exit requested", [](RuntimeGateState& s) { s.mHasExitRequested = true; }},
        {"view failed", [](RuntimeGateState& s) { s.mHasViewFailed = true; }},
        {"not started", [](RuntimeGateState& s) { s.mIsStarted = false; }},
        {"recording", [](RuntimeGateState& s) {
             s.mIsPlayback = false;
             s.mReplayState = ReplayState::Recording;
         }}};
    for (const BlockedCase& item : cases)
    {
        auto s = seekable;
        item.mChange(s);
        if (CanSeekPlayback(s))
            throw std::runtime_error(std::string("Seeking is blocked when ") + item.mName);
    }
}

void CheckHalfShroudIgnoresViewFailure()
{
    ForEachState([](const RuntimeGateState& s) {
        auto flipped = s;
        flipped.mHasViewFailed = !s.mHasViewFailed;
        Require(rareplay::plugin::ShouldUseHalfShroud(s) == rareplay::plugin::ShouldUseHalfShroud(flipped),
            "Half shroud does not depend on view failure");
    });

    RuntimeGateState s;
    s.mIsStarted = true;
    s.mIsPlayback = true;
    s.mIsHalfShroudEnabled = true;
    s.mReplayState = ReplayState::Paused;
    s.mHasViewFailed = true;
    Require(rareplay::plugin::ShouldUseHalfShroud(s), "Failed HUD keeps the half shroud");
    Require(!rareplay::plugin::IsPlaybackViewActive(s), "Failed HUD makes the view inactive");
}

void CheckEachCondition()
{
    RuntimeGateState active;
    active.mIsStarted = true;
    active.mIsPlayback = true;
    active.mIsPlaybackSourceOpen = true;
    active.mIsHalfShroudEnabled = true;
    active.mReplayState = ReplayState::Playing;
    Require(rareplay::plugin::IsPlaybackViewActive(active), "Playing view is active");
    Require(rareplay::plugin::ShouldUseHalfShroud(active), "Playing with half shroud uses it");
    Require(rareplay::plugin::IsPlaybackPlaying(active), "Playing state is playing");
    Require(rareplay::plugin::ShouldBlockPlayerCommands(active), "Open playback source blocks commands");
    Require(!rareplay::plugin::IsRecordingStarted(active), "Playback is not recording");

    for (const auto state : {ReplayState::Paused, ReplayState::Ended})
    {
        auto s = active;
        s.mReplayState = state;
        Require(rareplay::plugin::IsPlaybackViewActive(s), "Paused or ended view stays active");
        Require(rareplay::plugin::ShouldUseHalfShroud(s), "Paused or ended view keeps half shroud");
        Require(!rareplay::plugin::IsPlaybackPlaying(s), "Paused or ended is not playing");
    }
    for (const auto state : {ReplayState::Inactive, ReplayState::Recording, ReplayState::Finished,
             ReplayState::Failed})
    {
        auto s = active;
        s.mReplayState = state;
        Require(!rareplay::plugin::IsPlaybackViewActive(s), "Other states are not viewable");
        Require(!rareplay::plugin::ShouldUseHalfShroud(s), "Other states do not use half shroud");
    }

    auto s = active;
    s.mHasExitRequested = true;
    Require(!rareplay::plugin::IsPlaybackViewActive(s) && !rareplay::plugin::ShouldUseHalfShroud(s),
        "Exit request ends the view and the half shroud");
    Require(rareplay::plugin::IsPlaybackPlaying(s) && rareplay::plugin::ShouldBlockPlayerCommands(s),
        "Exit request does not change playing or command blocking");

    s = active;
    s.mIsStarted = false;
    Require(!rareplay::plugin::IsPlaybackViewActive(s) && !rareplay::plugin::IsPlaybackPlaying(s),
        "Unstarted playback is neither viewable nor playing");
    Require(rareplay::plugin::ShouldBlockPlayerCommands(s), "Command blocking does not wait for start");

    s = active;
    s.mHasStartFailed = true;
    Require(!rareplay::plugin::ShouldBlockPlayerCommands(s), "Start failure releases player commands");
    s = active;
    s.mIsPlaybackSourceOpen = false;
    Require(!rareplay::plugin::ShouldBlockPlayerCommands(s), "Closed source does not block commands");

    RuntimeGateState recording;
    recording.mIsStarted = true;
    recording.mReplayState = ReplayState::Recording;
    Require(rareplay::plugin::IsRecordingActive(recording) && rareplay::plugin::IsRecordingStarted(recording),
        "Started recording is active");
    recording.mReplayState = ReplayState::Failed;
    Require(!rareplay::plugin::IsRecordingActive(recording) && rareplay::plugin::IsRecordingStarted(recording),
        "Failed recording is started but not active");
    recording.mIsStarted = false;
    recording.mReplayState = ReplayState::Recording;
    Require(!rareplay::plugin::IsRecordingActive(recording) && !rareplay::plugin::IsRecordingStarted(recording),
        "Unstarted recording is not active");
    recording.mIsStarted = true;
    recording.mIsPlaybackSourceOpen = true;
    Require(!rareplay::plugin::ShouldBlockPlayerCommands(recording), "Recording never blocks commands");
}

void CheckBeforeSimulationGate()
{
    using rareplay::plugin::ShouldRunBeforeSimulation;
    RuntimeGateState playing;
    playing.mIsStarted = true;
    playing.mIsPlayback = true;
    playing.mReplayState = ReplayState::Playing;
    Require(ShouldRunBeforeSimulation(playing), "Playing runs the before-simulation step");
    for (const auto state : {ReplayState::Paused, ReplayState::Ended, ReplayState::Failed,
             ReplayState::Finished, ReplayState::Inactive})
    {
        auto s = playing;
        s.mReplayState = state;
        Require(!ShouldRunBeforeSimulation(s), "Held, ended or failed playback skips the step");
    }
    auto exiting = playing;
    exiting.mHasExitRequested = true;
    exiting.mHasViewFailed = true;
    Require(ShouldRunBeforeSimulation(exiting), "Exit request and view failure do not change the step");

    RuntimeGateState recording;
    recording.mIsStarted = true;
    recording.mReplayState = ReplayState::Recording;
    Require(ShouldRunBeforeSimulation(recording), "Active recording runs the step to sample the game speed");
    for (const auto state : {ReplayState::Failed, ReplayState::Finished, ReplayState::Inactive})
    {
        auto s = recording;
        s.mReplayState = state;
        Require(!ShouldRunBeforeSimulation(s), "Failed or finished recording skips the step");
    }
    auto unstarted = recording;
    unstarted.mIsStarted = false;
    Require(!ShouldRunBeforeSimulation(unstarted), "Unstarted recording skips the step");
}

// 飞机影子绘制的中和：播放请求 × 全部模式都中和；录制请求只在单机（0 Campaign、5 Skirmish）中和。
void CheckAircraftShadowNeutralization()
{
    using rareplay::plugin::ShouldNeutralizeAircraftShadowSideEffects;
    for (std::uint32_t gameMode = 0; gameMode <= 7; ++gameMode)
    {
        Require(ShouldNeutralizeAircraftShadowSideEffects(true, gameMode), "Playback neutralizes in every mode");
        Require(ShouldNeutralizeAircraftShadowSideEffects(false, gameMode) == (gameMode == 0 || gameMode == 5),
            "Recording neutralizes only Campaign and Skirmish");
    }
    Require(ShouldNeutralizeAircraftShadowSideEffects(true, 0xFFFFFFFFu) &&
        !ShouldNeutralizeAircraftShadowSideEffects(false, 0xFFFFFFFFu), "Unknown modes record without neutralizing");
}
}

int main()
{
    try
    {
        CheckTruthTables();
        CheckHalfShroudIgnoresViewFailure();
        CheckEachCondition();
        CheckBeforeSimulationGate();
        CheckAircraftShadowNeutralization();
        CheckSeekGate();
        std::cout << "Runtime gate tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
