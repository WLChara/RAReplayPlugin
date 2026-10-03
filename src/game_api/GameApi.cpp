#include "GameApi.h"

#include "native/NativeGame.h"
#include "native/NativeShroud.h"
#include "native/NativeControlGroups.h"

// 此版 YRpp 的 ProdFailType 仍使用 Win32 的小写 byte 基类型。
using byte = unsigned char;

#include <EventClass.h>
#include <Fundamentals.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <Unsorted.h>

#include <bit>
#include <cstring>

namespace rareplay::game_api
{
namespace
{
static_assert(sizeof(EventClass) == 111);
static_assert(offsetof(EventClass, Frame) == 3);
}

GameApi::GameApi() : mControlGroupRecorder(native::ReadRecordedControlGroups) {}

bool GameApi::IsWorldReady() const
{
    return Game::IsActive && ScenarioClass::Instance && HouseClass::CurrentPlayer &&
        native::IsWorldReady();
}

bool GameApi::ReadSessionInfo(replay::ReplaySessionInfo& outInfo, std::string& error)
{
    if (!IsWorldReady() || SessionClass::Instance.FrameSendRate <= 0 ||
        HouseClass::CurrentPlayer->ArrayIndex < 0)
    {
        error = "Game session is not ready";
        return false;
    }
    outInfo.mGameMode = static_cast<std::uint32_t>(SessionClass::Instance.GameMode);
    outInfo.mLocalHouseIndex = static_cast<std::uint32_t>(HouseClass::CurrentPlayer->ArrayIndex);
    outInfo.mFirstFrame = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    outInfo.mFrameSendRate = static_cast<std::uint32_t>(SessionClass::Instance.FrameSendRate);
    outInfo.mRandomSeed = static_cast<std::uint32_t>(Game::Seed);
    outInfo.mStreamVersion = 3;
    error.clear();
    return true;
}

bool GameApi::ReadFrameContext(std::uint32_t& outFrameNumber, std::uint32_t& outChecksum,
    bool& outHasChecksum, std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady())
    {
        error = mPlayerInputEffectsError.empty() ?
            "No active game frame" : mPlayerInputEffectsError;
        return false;
    }
    outFrameNumber = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    outChecksum = mHasNativeChecksum ? EventClass::CurrentFrameCRC : 0;
    outHasChecksum = mHasNativeChecksum;
    error.clear();
    return true;
}

bool GameApi::CopyDueEvents(std::vector<replay::RecordedEvent>& outEvents, std::string& error)
{
    outEvents.clear();
    if (!mIsFrameActive || EventClass::DoList.Count < 0 ||
        EventClass::DoList.Count > EventClass::MAX_EVENTS * 128)
    {
        error = "Invalid native event queue";
        return false;
    }
    for (int index = 0; index < EventClass::DoList.Count; ++index)
    {
        const EventClass& event = EventClass::DoList[index];
        if (event.Frame != mActiveFrame ||
            (reinterpret_cast<const std::uint8_t*>(&event)[1] & 1) != 0)
            continue;
        if (outEvents.size() >= 16384)
        {
            error = "Too many events in frame";
            return false;
        }
        replay::RecordedEvent bytes{};
        std::memcpy(bytes.data(), &event, bytes.size());
        outEvents.push_back(bytes);
    }
    error.clear();
    return true;
}

bool GameApi::CopyPlayerInputEffects(std::vector<replay::RecordedInputEffect>& outEffects,
    std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady() ||
        mActiveFrame != static_cast<std::uint32_t>(Unsorted::CurrentFrame))
    {
        error = "No active game frame for player input effects";
        return false;
    }
    return mPlayerInputEffects.Drain(mActiveFrame, outEffects, error);
}

bool GameApi::ApplyPlayerInputEffects(
    const std::vector<replay::RecordedInputEffect>& effects, std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady())
    {
        error = "No active game frame for player input effect playback";
        return false;
    }
    return PlayerInputEffects::Apply(effects, error);
}

bool GameApi::CopyRecordedControlGroups(
    std::optional<replay::RecordedControlGroupSnapshot>& outUpdate, std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady() ||
        mActiveFrame != static_cast<std::uint32_t>(Unsorted::CurrentFrame))
    {
        error = "没有可提交编队数据的当前游戏帧";
        return false;
    }
    return mControlGroupRecorder.CopyFrameUpdate(mActiveFrame, outUpdate, error);
}

bool GameApi::RecordControlGroupUpdate(std::string& error)
{
    if (!IsWorldReady())
    {
        error = "游戏世界未就绪，无法采集编队";
        return false;
    }
    return mControlGroupRecorder.CaptureAfterNativeUpdate(error);
}

ControlGroupCaptureStats GameApi::GetControlGroupCaptureStats() const noexcept
{
    return mControlGroupRecorder.GetStats();
}

bool GameApi::RecordPlayerInputEffect(const replay::RecordedInputEffect& effect,
    std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady() ||
        mActiveFrame != static_cast<std::uint32_t>(Unsorted::CurrentFrame))
    {
        error = "No active game frame for player input effect capture";
        return false;
    }
    return mPlayerInputEffects.Append(effect, error);
}

bool GameApi::ExecuteReplayFrame(const replay::ReplayFrame& frame, std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady() ||
        frame.mFrameNumber != static_cast<std::uint32_t>(Unsorted::CurrentFrame))
    {
        error = "Replay frame does not match game frame";
        return false;
    }
    const auto mode = SessionClass::Instance.GameMode;
    if (mode == GameMode::LAN || mode == GameMode::Internet)
    {
        // 原生网络帧即使没有可比较的录制 CRC，也会计算 CRC 并推进场景 RNG。
        Game::ComputeFrameCRC();
        const auto checksum = EventClass::CurrentFrameCRC;
        EventClass::LatestFramesCRC[frame.mFrameNumber & 255] = checksum;
        if (frame.mHasChecksum && checksum != frame.mChecksum)
        {
            error = "Replay frame checksum mismatch";
            return false;
        }
    }
    else if (mode == GameMode::Campaign || mode == GameMode::Skirmish)
    {
        // 原生离线解析器不计算网络帧 CRC；调用它会额外推进场景 RNG。
        if (frame.mHasChecksum)
        {
            error = "Offline replay frame unexpectedly contains a network checksum";
            return false;
        }
    }
    else
    {
        error = "Unsupported replay game mode";
        return false;
    }
    DiscardLocalCommands();
    // 播放接管后该帧的 DoList 只由录像帧填充；旧网络及本机条目不能重复执行。
    EventClass::DoList.Init();
    if (frame.mEvents.size() > 16384 ||
        frame.mEvents.size() > static_cast<std::size_t>(EventClass::MAX_EVENTS * 128 - EventClass::DoList.Count))
    {
        error = "Replay event queue overflow";
        return false;
    }
    for (const auto& bytes : frame.mEvents)
    {
        if ((bytes[1] & 1) != 0)
        {
            error = "Replay event was already executed";
            return false;
        }
        // Options 在目标版本的 0x4C7939 仅请求本地暂停菜单，不属于可重演的世界命令。
        if (bytes[0] == static_cast<std::uint8_t>(EventType::Options))
            continue;
        EventClass event(static_cast<int>(bytes[2]), static_cast<EventType>(bytes[0]));
        std::memcpy(&event, bytes.data(), bytes.size());
        if (event.Frame != frame.mFrameNumber || !EventClass::DoList.Add(event))
        {
            error = "Invalid replay event";
            return false;
        }
    }
    if (!native::ExecuteDueEvents())
    {
        error = "Native event execution failed";
        return false;
    }
    native::ProcessPostFrameEvents();
    error.clear();
    return true;
}

void GameApi::DiscardLocalCommands()
{
    ProcessViewerCommands();
    EventClass::OutList.Init();
}

void GameApi::EndReplay()
{
    native::RequestReplayExit();
}

void GameApi::BeginFrame()
{
    mActiveFrame = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    mIsFrameActive = IsWorldReady();
    if (mIsFrameActive && !mPlayerInputEffects.BeginFrame(mActiveFrame,
            mPlayerInputEffectsError))
        mIsFrameActive = false;
    mHasNativeChecksum = false;
}

void GameApi::ProcessViewerCommands()
{
    const int count = EventClass::OutList.Count;
    if (count < 0 || count > EventClass::MAX_EVENTS)
        return;
    bool requestedOptions = false;
    for (int index = 0; index < count; ++index)
    {
        const auto& event = EventClass::OutList[index];
        if (event.Type == EventType::Options)
            requestedOptions = true;
        else if (event.Type == EventType::GameSpeed)
            TrySetViewerPlaybackSpeed(event.GameSpeed.GameSpeed);
    }
    if (requestedOptions)
        native::RequestViewerOptionsMenu();
}

bool GameApi::TrySetViewerPlaybackSpeed(int setting) noexcept
{
    return CanControlViewerPlaybackSpeed() && mViewerPlaybackSpeed.TrySet(setting);
}

bool GameApi::TryGetViewerPlaybackSpeed(int& outSetting) const noexcept
{
    return CanControlViewerPlaybackSpeed() && mViewerPlaybackSpeed.TryGet(outSetting);
}

bool GameApi::CanControlViewerPlaybackSpeed() const noexcept
{
    return SessionClass::Instance.GameMode == GameMode::Skirmish;
}

void GameApi::BeforeNativeEventExecution()
{
    // ProcessNetworkFrame 在调用 handleAny 前写入当前帧 CRC；离线分支无此保证。
    const auto mode = SessionClass::Instance.GameMode;
    mHasNativeChecksum = mIsFrameActive &&
        (mode == GameMode::LAN || mode == GameMode::Internet) &&
        static_cast<std::uint32_t>(Unsorted::CurrentFrame) == mActiveFrame &&
        EventClass::LatestFramesCRC[mActiveFrame & 255] == EventClass::CurrentFrameCRC;
}

void GameApi::InvokeNativeFrameParser()
{
    native::InvokeFrameParser();
}

bool GameApi::InvokeNativePreworldWait()
{
    return native::WaitForPeers();
}

void GameApi::DrawViewerOnTop() { native::DrawViewerOnTop(); }
void GameApi::ProcessGameOver() { native::ProcessGameOver(); }
bool GameApi::IsReplayExitRequested() { return native::IsReplayExitRequested(); }
void GameApi::UpdatePausedCamera() { native::UpdatePausedCamera(); }
void GameApi::InvalidateViewerShroud() { native::InvalidateViewerShroud(); }
void GameApi::ClearViewerShroud() noexcept { native::ClearViewerShroud(); }

void GameApi::RestoreRandomSeedForInitialization(std::uint32_t seed)
{
    Game::Seed = std::bit_cast<std::int32_t>(seed);
}
}
