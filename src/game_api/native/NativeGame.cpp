#include "NativeGame.h"

#include "../../target/TargetAbi.h"

using byte = unsigned char;

#include <objidl.h>
#include <Unsorted.h>
#include <MouseClass.h>
#include <InputManagerClass.h>
#include <algorithm>

#include <cstdint>

namespace rareplay::game_api::native
{
namespace
{
using HandleAnyFunction = int(__fastcall*)(int, int, void*, int*, int*);
using PostFrameEventsFunction = void(__cdecl*)();
using FrameParserFunction = void(__cdecl*)();
using PreworldWaitFunction = bool(__cdecl*)();
}

bool IsWorldReady()
{
    return *reinterpret_cast<const std::uint8_t*>(target::WORLD_READY) == 1;
}

bool ExecuteDueEvents()
{
    // ProcessNetworkFrame 在目标版本使用 Session+0x310 作 ECX、EDX=0。
    const int context = *reinterpret_cast<const int*>(target::EXECUTOR_CONTEXT);
    return reinterpret_cast<HandleAnyFunction>(target::EXECUTE_EVENTS)(
        context, 0, nullptr, nullptr, nullptr) != 0;
}

void ProcessPostFrameEvents()
{
    reinterpret_cast<PostFrameEventsFunction>(target::POST_FRAME_EVENTS)();
}

void InvokeFrameParser()
{
    reinterpret_cast<FrameParserFunction>(target::NATIVE_FRAME_PARSER)();
}

bool WaitForPeers()
{
    return reinterpret_cast<PreworldWaitFunction>(target::NATIVE_PREWORLD_WAIT)();
}

void RequestReplayExit()
{
    *reinterpret_cast<volatile std::uint8_t*>(target::EXIT_REQUEST) = 1;
    Game::IsActive = false;
}

void RequestViewerOptionsMenu()
{
    *reinterpret_cast<volatile int*>(target::OPTIONS_REQUEST) = 1;
}

void DrawViewerOnTop()
{
    reinterpret_cast<void(__thiscall*)(MouseClass*)>(target::NATIVE_DRAW_ON_TOP)(
        &MouseClass::Instance);
}

void ProcessGameOver()
{
    reinterpret_cast<void(__cdecl*)()>(target::NATIVE_GAME_OVER)();
}

bool IsReplayExitRequested()
{
    return *reinterpret_cast<const volatile std::uint8_t*>(target::EXIT_REQUEST) != 0;
}

void UpdatePausedCamera()
{
    // KeyboardProcess 已维护四方向位；这里只复用 flags=1 的显示滚动，不运行 Logic。
    const auto keys = *reinterpret_cast<const std::uint32_t*>(target::VIEWER_SCROLL_KEYS);
    // 原生 0x82A030 的基础滚动量为 21；ForceSelect 优先于 ForceFire。
    constexpr int BASE_SCROLL_AMOUNT = 21;
    int amount = BASE_SCROLL_AMOUNT;
    if (InputManagerClass::Instance->IsForceSelectKeyPressed())
        amount = Game::F2I(amount * 2.5);
    else if (InputManagerClass::Instance->IsForceFireKeyPressed())
        amount = std::max(MouseClass::Instance.MapRect.Width, MouseClass::Instance.MapRect.Height) << 8;
    constexpr std::uint32_t KEY_BITS[] = {0x100, 0x1000, 0x1, 0x10};
    constexpr DWORD DIRECTIONS[] = {6, 2, 0, 4};
    for (std::size_t index = 0; index < 4; ++index)
    {
        if ((keys & KEY_BITS[index]) != 0)
            MouseClass::Instance.ScrollMap(DIRECTIONS[index], reinterpret_cast<DWORD>(&amount), 1);
    }
}
}
