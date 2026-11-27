#include "GameApi.h"

#include "capture/DispatchedEvents.h"
#include "native/NativeGame.h"
#include "native/NativeScenarioRestart.h"
#include "native/NativeShroud.h"
#include "native/NativeControlGroups.h"
#include "../replay/FrameCodec.h"
#include "../target/TargetAbi.h"

// 此版 YRpp 的 ProdFailType 仍使用 Win32 的小写 byte 基类型。
using byte = unsigned char;

#include <EventClass.h>
#include <Fundamentals.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <Unsorted.h>

#include <cstring>

namespace rareplay::game_api
{
namespace
{
static_assert(sizeof(EventClass) == 111);
static_assert(offsetof(EventClass, Frame) == 3);
// 分派采集的 SDK 无关规则与目标 ABI 常量必须与 YRpp 声明一致。
static_assert(sizeof(EventClass) == target::EVENT_BYTES);
static_assert(sizeof(EventClass) == std::tuple_size_v<replay::RecordedEvent>);
static_assert(offsetof(EventClass, Type) == EVENT_TYPE_OFFSET);
static_assert(offsetof(EventClass, Frame) == EVENT_FRAME_OFFSET);
static_assert(static_cast<std::uint8_t>(EventType::FrameInfo) == EVENT_TYPE_FRAME_INFO);
static_assert(EventClass::MAX_EVENTS * 128 == target::DO_LIST_CAPACITY);
// 诊断读取 SessionClass::MPGameMode（0xA8B23C）；构造函数 0x5D5B60 在 0x5D5BBA 把 MPModeIndex 写到 this+0x28。
static_assert(offsetof(SessionClass, MPGameMode) == 4);
static_assert(offsetof(MPGameModeClass, MPModeIndex) == 0x28);

bool IsExecuted(const EventClass& event)
{
    // SDK 将执行标志声明为 bool，原生只使用 bit0；按字节读取避免非 0/1 值的 bool 语义。
    return (reinterpret_cast<const std::uint8_t*>(&event)[1] & 1) != 0;
}
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
    // 新录像一律写最新帧流版本 v5：布局与 v4 相同（联机帧流不带速度，与 v3 布局逐字节相同）；
    // v5 另表示单机帧流录制时已中和飞机影子绘制的模拟副作用（R1），中和由 SetHeight 的 Hook 在插件接受请求后执行。
    outInfo.mStreamVersion = replay::FrameCodec::LATEST_STREAM_VERSION;
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
    // 单机 handleAny 还会执行全部过期事件，入口的 Frame==N 只是其子集；改由分派点逐条采集。
    if (UsesDispatchEventCapture())
    {
        error.clear();
        return true;
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

bool GameApi::ReadGameSpeed(std::int32_t& outSpeed, std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady())
    {
        error = "No active game frame for game speed";
        return false;
    }
    outSpeed = native::ReadGameSpeed();
    error.clear();
    return true;
}

bool GameApi::ApplyGameSpeed(std::uint32_t speed, std::string& error)
{
    if (!mIsFrameActive || !IsWorldReady())
    {
        error = "No active game frame for game speed playback";
        return false;
    }
    if (speed > replay::FrameCodec::MAX_GAME_SPEED)
    {
        error = "Recorded game speed is outside 0..6";
        return false;
    }
    native::WriteGameSpeed(static_cast<int>(speed));
    error.clear();
    return true;
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
    if (!mPlayerInputEffects.Append(effect, error))
        return false;
    // MoveFlash 只在单机采集；记录点击发生在本帧原生解析器之前还是之后。
    mDispatchCapture.RecordMoveFlash(ReadDispatchCaptureFrame());
    return true;
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
    ConsumeLocalOutList();
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
    mDispatchCapture.BeginFrame(mActiveFrame);
}

void GameApi::ConsumeLocalOutList()
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
    // 单机 handleAny 只由原生帧解析器在 0x6475AE 调用；入口即“进入本帧分派”的标记。
    if (mDispatchCapture.OpenAtEventExecution(ReadDispatchCaptureFrame()))
        RecordEntryDiagnostics();
}

void GameApi::RecordEntryDiagnostics()
{
    const int count = EventClass::DoList.Count;
    if (count < 0 || count > EventClass::MAX_EVENTS * 128)
        return;
    std::uint32_t staleCount = 0;
    std::uint32_t currentCount = 0;
    std::uint32_t futureCount = 0;
    for (int index = 0; index < count; ++index)
    {
        const EventClass& event = EventClass::DoList[index];
        if (IsExecuted(event))
            continue;
        const auto relation = ClassifyEventFrame(event.Frame, mActiveFrame);
        if (relation == EventFrameRelation::Stale)
            ++staleCount;
        else if (relation == EventFrameRelation::Current)
            ++currentCount;
        else
            ++futureCount;
    }
    const int outListCount = EventClass::OutList.Count;
    mDispatchCapture.RecordEntry(staleCount, currentCount, futureCount,
        outListCount > 0 ? static_cast<std::uint32_t>(outListCount) : 0);
}

void GameApi::AfterNativeFrameParser() noexcept
{
    mDispatchCapture.CloseAfterFrameParser(static_cast<std::uint32_t>(Unsorted::CurrentFrame));
}

bool GameApi::UsesDispatchEventCapture() const noexcept
{
    const auto mode = SessionClass::Instance.GameMode;
    return mode == GameMode::Campaign || mode == GameMode::Skirmish;
}

bool GameApi::CopyDispatchedEvent(std::uint32_t ringIndex, replay::RecordedEvent& outEvent,
    bool& outIsCaptured, std::string& error)
{
    outIsCaptured = false;
    error.clear();
    const auto admission = mDispatchCapture.AdmitDispatch(ReadDispatchCaptureFrame());
    if (admission == DispatchAdmission::Unmarked)
        return true;
    if (ringIndex >= target::DO_LIST_CAPACITY || admission == DispatchAdmission::OutsideActiveFrame)
    {
        error = "Native event dispatch is outside the active recording frame";
        return false;
    }
    const EventClass* events = EventClass::DoList.GetArray();
    if (reinterpret_cast<std::uintptr_t>(events) != target::DO_LIST_EVENTS)
    {
        error = "Native DoList layout differs from the dispatch ABI";
        return false;
    }
    replay::RecordedEvent dispatched{};
    std::memcpy(dispatched.data(), &events[ringIndex], dispatched.size());
    outIsCaptured = mDispatchCapture.Capture(dispatched, mActiveFrame, outEvent);
    return true;
}

EventCaptureContext GameApi::ReadEventCaptureContext() const
{
    EventCaptureContext context;
    context.mSessionGameMode = static_cast<std::uint32_t>(SessionClass::Instance.GameMode);
    const MPGameModeClass* mode = SessionClass::Instance.MPGameMode;
    context.mMpGameModeAddress = reinterpret_cast<std::uintptr_t>(mode);
    if (mode)
        context.mMpModeIndex = mode->MPModeIndex;
    context.mUsesDispatchCapture = UsesDispatchEventCapture();
    return context;
}

std::optional<EventCaptureFrameStats> GameApi::CompleteEventCaptureFrame() noexcept
{
    return mDispatchCapture.CompleteFrame(UsesDispatchEventCapture());
}

const EventCaptureTotals& GameApi::GetEventCaptureTotals() const noexcept
{
    return mDispatchCapture.GetTotals();
}

DispatchCaptureFrame GameApi::ReadDispatchCaptureFrame() const noexcept
{
    DispatchCaptureFrame frame;
    frame.mActiveFrame = mActiveFrame;
    frame.mIsFrameActive = mIsFrameActive;
    frame.mCurrentFrame = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    frame.mUsesDispatchCapture = UsesDispatchEventCapture();
    return frame;
}

std::uint32_t GameApi::ReadSessionGameMode() const noexcept { return native::ReadSessionGameMode(); }
bool GameApi::IsReplayExitRequested() const { return native::IsReplayExitRequested(); }
void GameApi::UpdatePausedCamera() { native::UpdatePausedCamera(); }
void GameApi::InvalidateViewerShroud() { native::InvalidateViewerShroud(); }
void GameApi::ClearViewerShroud() noexcept { native::ClearViewerShroud(); }
void GameApi::ClearFrameBudgets() noexcept { native::ClearFrameBudgets(); }
void GameApi::RequestFullRedraw() noexcept { native::RequestFullRedraw(); }
void GameApi::MuteAudioForSeek() noexcept { native::MuteAudioForSeek(); }
void GameApi::RestoreAudioAfterSeek() noexcept { native::RestoreAudioAfterSeek(); }
native::SeekBacklogCounts GameApi::ReadSeekBacklogCounts() const noexcept { return native::ReadSeekBacklogCounts(); }
native::SeekBacklogCleanupResult GameApi::CleanSeekBacklog(const native::SeekBacklogCleanup& cleanup) noexcept
{
    return native::CleanSeekBacklog(cleanup);
}
void GameApi::RegisterViewerShroudFullRedraw() noexcept { native::RegisterViewerShroudFullRedraw(); }

std::optional<native::ScenarioSessionSnapshot> GameApi::ReadScenarioSessionSnapshot() const noexcept
{
    return native::ReadScenarioSessionSnapshot();
}

bool GameApi::CheckScenarioRestartReady(const native::ScenarioSessionSnapshot& snapshot,
    std::string& outReason) const noexcept
{
    return native::CheckScenarioRestartReady(snapshot, outReason);
}

bool GameApi::RestartScenarioInProcess(const native::ScenarioSessionSnapshot& snapshot,
    native::ScenarioRestartReport& outReport) noexcept
{
    // 重开后的第 0 帧由下一次 BeginFrame 重新建立本帧上下文；上一场的帧号与未取走的输入效果不得带过去。
    mPlayerInputEffects.Reset();
    mPlayerInputEffectsError.clear();
    mActiveFrame = 0;
    mIsFrameActive = false;
    mHasNativeChecksum = false;
    return native::RestartScenarioInProcess(snapshot, outReport);
}
}
