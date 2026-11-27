#pragma once

#include <cstdint>

// 原生函数调用与引擎全局状态读写。Hook 需要的原生调用直接调用这里的自由函数；plugin 经 GameApi 使用。
namespace rareplay::game_api::native
{
bool IsWorldReady();
bool ExecuteDueEvents();
void ProcessPostFrameEvents();
void InvokeFrameParser();
bool WaitForPeers();
void RequestReplayExit();
void RequestViewerOptionsMenu();
void DrawViewerOnTop();
void ProcessGameOver();
bool IsReplayExitRequested();
void UpdatePausedCamera();
/** GameOptionsClass::GameSpeed 原值；只读。 */
int ReadGameSpeed();
/** 写 GameOptionsClass::GameSpeed；调用方负责取值范围与时机（单机模拟前）。 */
void WriteGameSpeed(int speed);
/** 仅在已验证目标的 InitRandom 选种后、任一 RNG 初始化前的游戏线程调用；不要求世界就绪。 */
void RestoreRandomSeedForInitialization(std::uint32_t seed);
/** SessionClass::GameMode 原值（0 Campaign、1–4 联机、5 Skirmish 等）；只读，不要求世界就绪。 */
std::uint32_t ReadSessionGameMode() noexcept;

// ---- 回放向前跳转：都只在游戏线程调用，本身不保存状态，是否已静音由 plugin 持有。----
/**
 * 本帧不限速：把单机帧预算与联机等待预算写为 0。只在 FRAME_COMPLETE 之后、RegulateFrameRate 之前调用才对本帧有效；
 * 两者每帧开始时由原生重算，不必恢复。不改变模拟与帧号推进。
 */
void ClearFrameBudgets() noexcept;
/**
 * 请求下一次 DrawOnTop 完整重绘：GScreenClass::MarkNeedsRedraw(&MouseClass::Instance, 2)，战术区三个 Render 阶段按全重绘、
 * 侧栏与命令条按 TabClass::Draw(1)。会写 Tactical 的重绘标志，所以只在世界存在时调用。
 */
void RequestFullRedraw() noexcept;
/** 快进静音：VoxClass::StopCurrent(true) 清掉当前 EVA 与队列，再把三个主音量斜坡（非空的）立即设为 0；不写 GameOptions。 */
void MuteAudioForSeek() noexcept;
/**
 * 快进后恢复声音：再 StopCurrent(true) 丢掉快进期间积累的 EVA，三个斜坡（非空的）按 F2I(GameOptions 音量 × 16384) 立即恢复，
 * 取值与原生音量 setter 相同（音效 +0x38、语音 +0x3C、音乐 +0x40）。
 */
void RestoreAudioAfterSeek() noexcept;
/**
 * 快进中跳过一次 GScreenClass::GetInputAndUpdate 时，替它写出“无按键”：outKeyCode 是该函数第一个栈参数（调用方的局部
 * DWORD）。调用方随后把键码交给 KeyboardProcess，键码为 0 时它直接返回；两个鼠标输出不写，调用方也不读。
 */
void WriteNoViewerInputKey(void* outKeyCode) noexcept;
}
