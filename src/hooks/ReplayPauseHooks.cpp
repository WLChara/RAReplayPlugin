#include "../game_api/native/NativeGame.h"
#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"
#include "ViewerInputScope.h"

#include <Syringe.h>

// 完整 CALL DrawOnTop：ECX 是 MouseClass，无栈参数。正常分支显式原调用。
// 输入已处理；暂停复用原生消息、绘制和节流分支，不进入任何模拟阶段。
// 快进中不画的帧（ContinueWithoutDraw）只跳过这次 DrawOnTop，续点与 Continue 相同；需要完整重绘时运行时已在返回前请求。
DEFINE_HOOK(0x55D8F2, ReplayViewerLoopGateHook, 5)
{
    const auto action = rareplay::plugin::OnViewerLoopGate();
    if (action == rareplay::plugin::ViewerLoopAction::Exit)
        return static_cast<DWORD>(rareplay::target::VIEWER_EXIT_LOOP);
    if (action == rareplay::plugin::ViewerLoopAction::Hold)
        return static_cast<DWORD>(rareplay::target::VIEWER_HELD_LOOP);
    if (action != rareplay::plugin::ViewerLoopAction::ContinueWithoutDraw)
        rareplay::game_api::native::DrawViewerOnTop();
    return static_cast<DWORD>(rareplay::target::VIEWER_LOOP_CONTINUE);
}

// MainLoop 完整返回后，帧计数已原生递增。先执行待执行的进程内重开，或预取确认 EOF，再决定胜负处理。
// CALL ProcessGameOverState 无参数，返回值被宿主忽略；HNAC 仍消费退出请求。
// 进程内重开在这里完成（主循环两次迭代之间），之后照常续到 GAME_OVER_CONTINUE，不调用原生胜负处理；
// 观看输入范围属于 hooks，由这里随新场景复位。
DEFINE_HOOK(0x48CE9C, ReplayAfterFrameLoopHook, 5)
{
    using rareplay::plugin::AfterFrameLoopAction;
    const auto action = rareplay::plugin::OnAfterFrameLoop();
    if (action == AfterFrameLoopAction::ScenarioRestarted)
        rareplay::hooks::GetViewerInputScope().Reset();
    else if (action == AfterFrameLoopAction::NativeGameOver)
        rareplay::game_api::native::ProcessGameOver();
    return static_cast<DWORD>(rareplay::target::GAME_OVER_CONTINUE);
}
