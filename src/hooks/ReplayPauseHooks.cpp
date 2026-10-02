#include "../game_api/GameApi.h"
#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"

#include <Syringe.h>

// 完整 CALL DrawOnTop：ECX 是 MouseClass，无栈参数。正常分支显式原调用。
// 输入已处理；暂停复用原生消息、绘制和节流分支，不进入任何模拟阶段。
DEFINE_HOOK(0x55D8F2, ReplayViewerLoopGateHook, 5)
{
    const auto action = rareplay::plugin::OnViewerLoopGate();
    if (action == rareplay::plugin::ViewerLoopAction::Exit)
        return static_cast<DWORD>(rareplay::target::VIEWER_EXIT_LOOP);
    if (action == rareplay::plugin::ViewerLoopAction::Hold)
        return static_cast<DWORD>(rareplay::target::VIEWER_HELD_LOOP);
    rareplay::game_api::GameApi::DrawViewerOnTop();
    return static_cast<DWORD>(rareplay::target::VIEWER_LOOP_CONTINUE);
}

// MainLoop 完整返回后，帧计数已原生递增。先预取确认 EOF，再决定胜负处理。
// CALL ProcessGameOverState 无参数，返回值被宿主忽略；HNAC 仍消费退出请求。
DEFINE_HOOK(0x48CE9C, ReplayAfterFrameLoopHook, 5)
{
    if (!rareplay::plugin::OnAfterFrameLoop())
        rareplay::game_api::GameApi::ProcessGameOver();
    return static_cast<DWORD>(rareplay::target::GAME_OVER_CONTINUE);
}
