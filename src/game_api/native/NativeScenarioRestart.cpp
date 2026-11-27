#include "NativeScenarioRestart.h"

#include "NativeSeekBacklog.h"
#include "../../target/TargetAbi.h"

// 此版 YRpp 的若干声明仍使用 Win32 的小写 byte 基类型。
using byte = unsigned char;

#include <EventClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace rareplay::game_api::native
{
namespace
{
// 会话字段：Timing 处理分支 0x4C807D–0x4C80A3 写 DesiredFrameRate 0xA8B558、MaxAhead 0xA8B550、MaxMaxAhead 0xA8B568、
// FrameSendRate 0xA8B554（都是整字）；Session+0x300 由 AcceptDefeat 0x4FC205 整字写 1、新开局 0x52DA94 写 0。
static_assert(offsetof(SessionClass, GameMode) == 0);
static_assert(sizeof(SessionClass::GameMode) == sizeof(std::int32_t));
static_assert(offsetof(SessionClass, MultiplayerObserver) == 0x300);
static_assert(sizeof(SessionClass::MultiplayerObserver) == sizeof(std::int32_t));
static_assert(offsetof(SessionClass, MaxAhead) == 0x318);
static_assert(offsetof(SessionClass, FrameSendRate) == 0x31C);
static_assert(offsetof(SessionClass, DesiredFrameRate) == 0x320);
static_assert(offsetof(SessionClass, MaxMaxAhead) == 0x330);
// 场景：StartScenario 0x683B30 把文件名复制到 +0x125C；ClearScenario 0x685354 起按 41 字节步长清 +0x1CB0 起的 50 个全局变量值
// （0x802 = 50 × 41）并置 +0x34AA；局部变量的值紧随其后从 +0x24B2 起；开局前奏 0x48CE67 读 +0x35A2。
static_assert(offsetof(ScenarioClass, FileName) == 0x125C);
static_assert(sizeof(ScenarioClass::FileName) == 0x104);
static_assert(sizeof(Variable) == 41);
static_assert(offsetof(Variable, Value) == 40);
static_assert(offsetof(ScenarioClass, GlobalVariables) + offsetof(Variable, Value) == 0x1CB0);
static_assert(sizeof(ScenarioClass::GlobalVariables) / sizeof(Variable) == 50);
static_assert(offsetof(ScenarioClass, LocalVariables) + offsetof(Variable, Value) == 0x24B2);
static_assert(sizeof(ScenarioClass::LocalVariables) / sizeof(Variable) == 100);
static_assert(offsetof(ScenarioClass, VariablesChanged) == 0x34AA);
static_assert(offsetof(ScenarioClass, UserInputLocked) == 0x35A2);

constexpr std::uint32_t LAN_GAME_MODE = 3;
constexpr std::uint32_t INTERNET_GAME_MODE = 4;

// 地址与约定见 TargetAbi 的同名常量。SessionClass 与 IPXManagerClass 的成员函数都是 thiscall；返回值按 AL 或 EAX 读取。
using SessionMethodFunction = char(__thiscall*)(void* session);
using CreateConnectionsFunction = bool(__thiscall*)(void* session);
using ConnectionCountFunction = int(__thiscall*)(void* ipxManager);
using ConnectionIdAtFunction = int(__thiscall*)(void* ipxManager, int slot);
using DeleteConnectionFunction = bool(__thiscall*)(void* ipxManager, int id);
using StartScenarioFunction = bool(__fastcall*)(const char* fileName, bool briefing, int campaignIndex);
using NoArgumentFunction = void(__cdecl*)();

template <typename T>
volatile T& Global(std::uintptr_t address) noexcept
{
    return *reinterpret_cast<volatile T*>(address);
}

void* GetSession() noexcept
{
    return reinterpret_cast<void*>(target::SESSION_INSTANCE);
}

void* GetIpxManager() noexcept
{
    return reinterpret_cast<void*>(target::IPX_MANAGER_INSTANCE);
}

// SDK 把原生的 0/1 字节声明为 bool；按字节读写，避免非 0/1 值的 bool 语义。
std::uint8_t& AsByte(bool& flag) noexcept
{
    return *reinterpret_cast<std::uint8_t*>(&flag);
}

// 第 1 步：与原生 HNAC（0x55D080 加一、0x55D341 减一）一样包住整个重开；任何返回路径（含异常）都减一，
// 计数不为 0 时载入中的弹窗路径不会重入 MainLoop。
class SessionBusyGuard final
{
public:
    SessionBusyGuard() noexcept
    {
        auto& count = Global<std::int32_t>(target::SESSION_BUSY_COUNT);
        count = count + 1;
    }
    ~SessionBusyGuard()
    {
        auto& count = Global<std::int32_t>(target::SESSION_BUSY_COUNT);
        count = count - 1;
    }
    SessionBusyGuard(const SessionBusyGuard&) = delete;
    SessionBusyGuard& operator=(const SessionBusyGuard&) = delete;
};

// 第 8 步：与原生 WOL 再战（0x5C4BFC）相同每次删除第 0 槽；Delete_Connection 失败时计数不变，原生循环会死循环，
// 所以失败即停、最多 MAX_IPX_CONNECTION_DELETES 次。剩余数写进报告，不单独判失败（CreateConnections 的结果为准）。
void DeleteAllConnections(ScenarioRestartReport& report) noexcept
{
    const auto count = reinterpret_cast<ConnectionCountFunction>(target::IPX_NUM_CONNECTIONS);
    const auto idAt = reinterpret_cast<ConnectionIdAtFunction>(target::IPX_CONNECTION_ID_AT);
    const auto remove = reinterpret_cast<DeleteConnectionFunction>(target::IPX_DELETE_CONNECTION);
    for (int attempt = 0; attempt < target::MAX_IPX_CONNECTION_DELETES; ++attempt)
    {
        if (count(GetIpxManager()) <= 0)
            break;
        if (!remove(GetIpxManager(), idAt(GetIpxManager(), 0)))
            break;
        ++report.mDeletedConnections;
    }
    report.mRemainingConnections = count(GetIpxManager());
}

bool RunRestartSequence(const ScenarioSessionSnapshot& snapshot, ScenarioRestartReport& report)
{
    ScenarioClass* const scenario = ScenarioClass::Instance;
    if (scenario == nullptr)
    {
        report.mError = "step 0: no scenario object";
        return false;
    }
    // StartScenario 不限长复制传入的名字：先放进与 Scen->FileName 同大小、以 NUL 结尾的可写缓冲。
    char fileName[sizeof(ScenarioClass::FileName)]{};
    if (snapshot.mScenarioFileName.empty() || snapshot.mScenarioFileName.size() >= sizeof(fileName))
    {
        report.mError = "step 0: invalid scenario file name length";
        return false;
    }
    std::memcpy(fileName, snapshot.mScenarioFileName.data(), snapshot.mScenarioFileName.size());

    // 1. 忙碌计数加一，守卫析构时减一。
    const SessionBusyGuard busyGuard;

    // 2. 与原生 RestartCurrentScenario（0x68642A、0x686433）相同：场景未开始、游戏未激活。
    Global<std::uint8_t>(target::SCENARIO_STARTED) = 0;
    Global<std::uint8_t>(target::GAME_ACTIVE_FLAG) = 0;

    // 3. 退出对局状态：恢复首次进入时保存的游戏速度，清理影片、音频与 EVA，第 3 帧解锁的预约清零。
    reinterpret_cast<SessionMethodFunction>(target::FINALIZE_SESSION_CLEANUP)(GetSession());

    // 4. 清零与还原。两个队列只写 Count/Head/Tail，与原生新开局 0x52D9E4 起的写法及插件每帧的清空相同；
    // 0x4E7ED0/0x4E7EA0 是 CRT 初始化函数，另外逐项清 Type，计数为 0 时这些项不会被读取。
    EventClass::DoList.Init();
    EventClass::OutList.Init();
    Global<std::int32_t>(target::TIMING_TIME_STAMP) = 0;
    Global<std::int32_t>(target::FRAME_SYNC_TARGET_FRAME) = 0;
    SessionClass& session = SessionClass::Instance;
    session.MaxAhead = snapshot.mMaxAhead;
    session.FrameSendRate = snapshot.mFrameSendRate;
    session.MaxMaxAhead = snapshot.mMaxMaxAhead;
    session.DesiredFrameRate = snapshot.mDesiredFrameRate;
    session.MultiplayerObserver = snapshot.mMultiplayerObserver;
    Global<std::uint32_t>(target::SPECIAL_RULES_FLAGS_CACHE) = snapshot.mSpecialRulesFlags;
    // 变量的值直接写 0，不调用设置函数：设置函数会置变化标志，残留到第 0 帧多触发一次变量事件。局部变量的地图初值
    // 在载入中写入，所以必须在 StartScenario 之前清。
    for (Variable& variable : scenario->GlobalVariables)
        variable.Value = 0;
    for (Variable& variable : scenario->LocalVariables)
        variable.Value = 0;
    AsByte(scenario->VariablesChanged) = 0;

    // 5. 清场链不处理辐射光束，过期时会读写已删除对象的 +0x510：按快进积压清理的删除契约先全部删除。
    SeekBacklogCleanup cleanup;
    cleanup.mDeleteRadBeams = true;
    report.mDeletedRadBeams = CleanSeekBacklog(cleanup).mRadBeamsDeleted;

    // 6. 重新播种（插件的种子 Hook 写回录像种子），GameMode 置快照值（联机为 4，战队结盟等载入期处理只在 4 时执行）。
    reinterpret_cast<NoArgumentFunction>(target::INIT_RANDOM)();
    session.GameMode = static_cast<::GameMode>(snapshot.mGameMode);
    report.mScenarioInitBeforeStart = Global<std::int32_t>(target::SCENARIO_INIT);
    if (report.mScenarioInitBeforeStart != 0)
        Global<std::int32_t>(target::SCENARIO_INIT) = 0;

    // 7. 载入：不播影片，不指定战役。清场（ClearScenario）在其内部由读场景调用，插件不自行调用。
    const bool isStarted = reinterpret_cast<StartScenarioFunction>(target::START_SCENARIO)(
        fileName, false, target::START_SCENARIO_NO_CAMPAIGN);
    if (!isStarted)
    {
        report.mError = "step 7: StartScenario returned 0";
        return false;
    }

    // 8. 载入之后：ScenarioInit 与闪电风暴结束标志（清场在风暴进行中会置 1）置 0；联机改回 3 后重建连接。
    Global<std::int32_t>(target::SCENARIO_INIT) = 0;
    Global<std::uint8_t>(target::LIGHTNING_STORM_TIME_TO_END) = 0;
    if (snapshot.mGameMode == LAN_GAME_MODE || snapshot.mGameMode == INTERNET_GAME_MODE)
    {
        report.mHasRebuiltConnections = true;
        session.GameMode = static_cast<::GameMode>(LAN_GAME_MODE);
        DeleteAllConnections(report);
        if (!reinterpret_cast<CreateConnectionsFunction>(target::SESSION_CREATE_CONNECTIONS)(GetSession()))
        {
            report.mError = "step 8: CreateConnections returned 0 (remaining connections " +
                std::to_string(report.mRemainingConnections) + ")";
            return false;
        }
    }

    // 9. 按原生开局前奏（0x48CE62–0x48CE85）的顺序，然后进入对局状态（必要时锁输入，第 3 帧解锁）。
    ScenarioClass* const loaded = ScenarioClass::Instance;
    if (loaded == nullptr)
    {
        report.mError = "step 9: no scenario object after StartScenario";
        return false;
    }
    Global<std::int32_t>(target::SPECIAL_DIALOG_STATE) = 0;
    Global<std::uint8_t>(target::USER_INPUT_LOCKED_SNAPSHOT) = AsByte(loaded->UserInputLocked);
    Global<std::uint8_t>(target::PRELUDE_BYTE_822CFC) = 1;
    reinterpret_cast<SessionMethodFunction>(target::CHECK_AND_PROCESS_NETWORK_ABORT)(GetSession());

    // 10. 退出与重开请求字节清零，否则 GAME_OVER_CALL 之后的原生处理会结束会话或走原生重开；忙碌计数随守卫减一。
    Global<std::uint8_t>(target::EXIT_REQUEST) = 0;
    Global<std::uint8_t>(target::MAIN_LOOP_EXIT_GATE_A83D49) = 0;
    Global<std::uint8_t>(target::MAIN_LOOP_EXIT_GATE_A8ECD0) = 0;
    Global<std::uint8_t>(target::SCENARIO_RESTART_REQUESTED) = 0;
    return true;
}
}

std::optional<ScenarioSessionSnapshot> ReadScenarioSessionSnapshot() noexcept
{
    try
    {
        const ScenarioClass* const scenario = ScenarioClass::Instance;
        if (scenario == nullptr)
            return std::nullopt;
        auto fileName = CopyScenarioFileName(scenario->FileName, sizeof(scenario->FileName));
        if (!fileName)
            return std::nullopt;
        const SessionClass& session = SessionClass::Instance;
        ScenarioSessionSnapshot snapshot;
        snapshot.mMaxAhead = session.MaxAhead;
        snapshot.mFrameSendRate = session.FrameSendRate;
        snapshot.mMaxMaxAhead = session.MaxMaxAhead;
        snapshot.mDesiredFrameRate = session.DesiredFrameRate;
        snapshot.mSpecialRulesFlags = Global<std::uint32_t>(target::SPECIAL_RULES_FLAGS_CACHE);
        snapshot.mGameMode = static_cast<std::uint32_t>(session.GameMode);
        snapshot.mMultiplayerObserver = session.MultiplayerObserver;
        snapshot.mScenarioFileName = std::move(*fileName);
        return snapshot;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool CheckScenarioRestartReady(const ScenarioSessionSnapshot& snapshot, std::string& outReason) noexcept
{
    try
    {
        if (Global<std::uint8_t>(target::GAME_ACTIVE) == 0)
            outReason = "g_GameActive is 0";
        else if (ScenarioClass::Instance == nullptr)
            outReason = "no scenario object";
        else if (!CanRestartFromSnapshot(snapshot))
            outReason = "the session snapshot cannot restart this game mode";
        else if (snapshot.mScenarioFileName.size() >= sizeof(ScenarioClass::FileName))
            outReason = "the scenario file name is too long";
        else
        {
            outReason.clear();
            return true;
        }
    }
    catch (...)
    {
    }
    return false;
}

bool RestartScenarioInProcess(const ScenarioSessionSnapshot& snapshot, ScenarioRestartReport& outReport) noexcept
{
    try
    {
        outReport = ScenarioRestartReport();
        return RunRestartSequence(snapshot, outReport);
    }
    catch (...)
    {
        try
        {
            if (outReport.mError.empty())
                outReport.mError = "unexpected exception during the restart sequence";
        }
        catch (...)
        {
        }
        return false;
    }
}
}
