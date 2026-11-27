#pragma once

#include <cstdint>
#include <optional>

#include "NativeSidebarGate.h"
#include "../replay/ReplayTypes.h"

namespace rareplay::plugin
{
void OnStartup() noexcept;
bool TryGetPlaybackSeed(std::uint32_t& outSeed) noexcept;
void OnFrameBegin() noexcept;
/**
 * VIEWER_LOOP_GATE（MainLoop 内 DrawOnTop 调用处）的处理结果。Continue 照常绘制并继续本帧；
 * ContinueWithoutDraw 只在快进中使用（除兜底重绘外的每一帧），不调用 DrawOnTop，其余与 Continue 相同；
 * Hold 进入原生保持分支；Exit 结束回放。
 */
enum class ViewerLoopAction { Continue, Hold, Exit, ContinueWithoutDraw };
ViewerLoopAction OnViewerLoopGate() noexcept;
bool OnAfterFrameLoop() noexcept;
bool ShouldUseHalfShroud() noexcept;
/**
 * GScreenClass::GetInputAndUpdate 入口每次调用一次，只在游戏线程。快进中、且距上次完整执行不到 SEEK_INPUT_TICK_MS 时
 * 返回 true：Hook 替它写出“无按键”后直接返回，不进入观看输入范围。其余情况返回 false（照常执行），快进中并记下这次完整执行的时刻。
 * 没有回放或不在快进中时总是 false，也不读时钟。不抛异常。
 */
bool ShouldSkipViewerInputUpdate() noexcept;
void OnBeforeSimulation() noexcept;
bool ShouldCapturePlayerInputEffect() noexcept;
void OnPlayerInputEffectCaptured(const replay::RecordedInputEffect& effect) noexcept;
void OnPlayerInputEffectCaptureFailed() noexcept;
void OnControlGroupUpdated() noexcept;
bool ShouldBlockPlayerCommands() noexcept;
/**
 * SetHeight 内飞机影子绘制的中和 Hook 在返回地址匹配后调用，只在游戏线程。按需完成启动（读取并消费启动请求）；
 * 没有已接受的请求时返回 false（原生行为）。规则见 RuntimeGates 的同名判定：只取决于请求类型与当前 GameMode，
 * 与会话是否已开始或失败无关。每次运行第一次返回 true 时记一行技术日志。不抛异常。
 */
bool ShouldNeutralizeAircraftShadowSideEffects() noexcept;
/**
 * SidebarClass::Draw 跳过点每次调用一次，只在游戏线程。返回 RedrawNative 时已置原生重画标志，
 * Hook 仍须令本次 Draw 按强制重画执行。异常或无回放时返回 DrawNative，不影响回放会话。
 */
NativeSidebarAction OnNativeSidebarDraw() noexcept;
/**
 * 超武与渗透的只读观察点，只在游戏线程调用，参数是 Hook 寄存器与栈上的原生对象地址（不转移所有权，可为空）。
 * 只在回放播放时工作；录制或无回放时立即返回。任何异常都只停用雷达事件，不影响回放会话。
 * Begin/End 成对出现在 HouseClass::Fire_SW 调用 ClickFire 的前后；Infiltrate 在间谍调用 BuildingClass::Infiltrate 之前。
 */
void OnSuperWeaponFireBegin(const void* house, const void* super, const void* prerequisiteSuper,
    const void* targetCell) noexcept;
void OnSuperWeaponFireEnd(const void* super) noexcept;
void OnBuildingInfiltrated(const void* spy, const void* building) noexcept;
bool TrySetPlaybackViewingSpeed(int setting) noexcept;
bool TryGetPlaybackViewingSpeed(int& outSetting) noexcept;
bool ShouldHandlePlaybackViewingSpeed() noexcept;
void OnBeforeNativeEvents() noexcept;
/** 原生 handleAny 每分派一条事件调用一次；ringIndex 为空表示分派点寄存器不符合目标 ABI。 */
void OnNativeEventDispatched(std::optional<std::uint32_t> ringIndex) noexcept;
/** 录制时原生帧解析器返回后调用。 */
void OnNativeFrameParserReturned() noexcept;
void OnFrameComplete() noexcept;
bool OnPlaybackParser() noexcept;
bool ShouldBypassPeerProgressWait() noexcept;
void OnSessionEnd() noexcept;
void OnHookFailure() noexcept;
}
