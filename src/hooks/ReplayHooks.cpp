#include "../game_api/native/NativeGame.h"
#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"
#include "ViewerInputScope.h"

#include <Syringe.h>

#include <cstdint>
#include <optional>

// Syringe 从 .syhks00 读取声明；CALL rel32 站点由回调显式调用原目标并跳到续点。
DEFINE_HOOK(0x52D9A0, ReplayStartupHook, 6)
{
    rareplay::plugin::OnStartup();
    return 0;
}

// 两个分支均已完成选种；在原指令读取种子并初始化 RNG 前恢复录像值。
DEFINE_HOOK(0x52FDF9, ReplayRandomSeedHook, 6)
{
    std::uint32_t seed = 0;
    if (rareplay::plugin::TryGetPlaybackSeed(seed))
        rareplay::game_api::native::RestoreRandomSeedForInitialization(seed);
    return 0;
}

DEFINE_HOOK_AGAIN(0x52FE5C, ReplayRandomSeedHook, 5)

DEFINE_HOOK(0x55D360, ReplayFrameBeginHook, 5)
{
    rareplay::plugin::OnFrameBegin();
    return 0;
}

// Native Input 之后、Logic.Update 之前重演本帧输入效果。
DEFINE_HOOK(0x55DC99, ReplayBeforeSimulationHook, 5)
{
    rareplay::plugin::OnBeforeSimulation();
    return 0;
}

// 已安装 Syringe 至少改写 5 字节；覆盖 SUB ESP,40h + PUSH EBX + 完整 MOV EBX,[A80238]。
DEFINE_HOOK(0x64C380, ReplayBeforeEventsHook, 10)
{
    rareplay::plugin::OnBeforeNativeEvents();
    return 0;
}

// 正常与单机过期执行路径都在此汇合，每条将分派的事件必经一次；ECX = 37 × DoList 环索引。
// 覆盖完整 7 字节 mov dl,[ecx+ecx*2+8B4204h]（无相对位移），返回 0 由 Syringe 执行原指令。
DEFINE_HOOK(0x64C63D, ReplayEventDispatchHook, 7)
{
    std::uint32_t ringIndex = 0;
    const bool isValid = rareplay::target::TryDecodeEventDispatchIndex(
        static_cast<std::uint32_t>(R->ECX()), ringIndex);
    rareplay::plugin::OnNativeEventDispatched(
        isValid ? std::optional<std::uint32_t>(ringIndex) : std::nullopt);
    return 0;
}

DEFINE_HOOK(0x55DE40, ReplayParserHook, 5)
{
    if (rareplay::plugin::OnPlaybackParser())
        return static_cast<DWORD>(rareplay::target::FRAME_PARSER_CONTINUE);
    rareplay::game_api::native::InvokeFrameParser();
    // 单机 handleAny 只在该解析器内调用；返回即本帧分派结束。
    rareplay::plugin::OnNativeFrameParserReturned();
    return static_cast<DWORD>(rareplay::target::FRAME_PARSER_CONTINUE);
}

DEFINE_HOOK(0x55DE7E, ReplayFrameCompleteHook, 9)
{
    rareplay::plugin::OnFrameComplete();
    return 0;
}

// 每次开局载入（含进程内重开的载入）经过一次：先让插件在首次载入时记下会话快照（之后的载入只比对），
// 再决定是否绕过等待其他玩家。
DEFINE_HOOK(0x684B3E, ReplayPreworldWaitHook, 5)
{
    rareplay::plugin::OnScenarioPreworldWait();
    const bool ready = rareplay::plugin::ShouldBypassPeerProgressWait() ||
        rareplay::game_api::native::WaitForPeers();
    R->EAX(static_cast<DWORD>(ready));
    return static_cast<DWORD>(rareplay::target::PREWORLD_WAIT_CONTINUE);
}

// 载入提速 L1：覆盖 DoBlit 中完整的 push 32h 与 call ds:[Sleep]（8 字节，无相对位移）。回放载入期间跳到下一条指令，
// 两条指令一并跳过，栈平衡；其余情况返回 0，由 Syringe 重放原指令照常等待。
DEFINE_HOOK(0x4F4B8E, ReplayLoadBlitSleepHook, 8)
{
    if (rareplay::plugin::ShouldSkipLoadBlitSleep())
        return static_cast<DWORD>(rareplay::target::LOAD_BLIT_SLEEP_CONTINUE);
    return 0;
}

DEFINE_HOOK(0x48CEAF, ReplaySessionEndHook, 5)
{
    rareplay::hooks::GetViewerInputScope().Reset();
    rareplay::plugin::OnSessionEnd();
    return 0;
}
