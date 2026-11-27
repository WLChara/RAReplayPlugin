#include "ViewerInputScope.h"

#include "../game_api/native/NativeGame.h"
#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"

#include <Syringe.h>

// 函数级输入范围覆盖主循环与限速等待中的鼠标轮询。
// 快进中按界面节拍降频：跳过的这次写出“无按键”后直接跳到函数末尾的 retn 0Ch（入口处 ESP 不变，正好清掉三个参数）；
// 跳过路径不经过 0x4F43DC 的 Leave，所以不能进入输入范围。
DEFINE_HOOK(0x4F4320, ReplayMouseInputBeginHook, 5)
{
    if (rareplay::plugin::ShouldSkipViewerInputUpdate())
    {
        rareplay::game_api::native::WriteNoViewerInputKey(
            R->Stack<void*>(rareplay::target::VIEWER_MOUSE_INPUT_KEY_CODE_STACK));
        return static_cast<DWORD>(rareplay::target::VIEWER_MOUSE_INPUT_SKIP_RETURN);
    }
    rareplay::hooks::GetViewerInputScope().Enter();
    return 0;
}

DEFINE_HOOK(0x4F43DC, ReplayMouseInputEndHook, 6)
{
    rareplay::hooks::GetViewerInputScope().Leave();
    return 0;
}

DEFINE_HOOK(0x55DEE0, ReplayKeyboardInputBeginHook, 5)
{
    rareplay::hooks::GetViewerInputScope().Enter();
    return 0;
}

// 所有正常出口都在尚未弹出函数栈帧的位置执行同一段深度收尾。
DEFINE_HOOK(0x55E072, ReplayKeyboardInputEndHook, 6)
{
    rareplay::hooks::GetViewerInputScope().Leave();
    return 0;
}

DEFINE_HOOK_AGAIN(0x55E083, ReplayKeyboardInputEndHook, 6)
DEFINE_HOOK_AGAIN(0x55E099, ReplayKeyboardInputEndHook, 6)
DEFINE_HOOK_AGAIN(0x55E155, ReplayKeyboardInputEndHook, 6)
DEFINE_HOOK_AGAIN(0x55E0B5, ReplayKeyboardInputEndHook, 7)
DEFINE_HOOK_AGAIN(0x55E0C4, ReplayKeyboardInputEndHook, 7)
DEFINE_HOOK_AGAIN(0x55E0E5, ReplayKeyboardInputEndHook, 7)
DEFINE_HOOK_AGAIN(0x55E0F4, ReplayKeyboardInputEndHook, 7)
DEFINE_HOOK_AGAIN(0x55E114, ReplayKeyboardInputEndHook, 7)
DEFINE_HOOK_AGAIN(0x55E122, ReplayKeyboardInputEndHook, 7)
DEFINE_HOOK_AGAIN(0x55E142, ReplayKeyboardInputEndHook, 7)

DEFINE_HOOK(0x50B6F0, ReplayViewerControlledByPlayerHook, 5)
{
    const auto& scope = rareplay::hooks::GetViewerInputScope();
    if (!scope.IsActive())
        return 0;
    if (!rareplay::hooks::ShouldSuppressViewerControl(scope,
            rareplay::plugin::ShouldBlockPlayerCommands(), R->Stack<DWORD>(0)))
        return 0;
    return static_cast<DWORD>(rareplay::target::VIEWER_CONTROLLED_FALSE_RETURN);
}
