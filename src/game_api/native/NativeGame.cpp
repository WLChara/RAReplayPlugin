#include "NativeGame.h"

#include "../../target/TargetAbi.h"

using byte = unsigned char;

#include <objidl.h>
#include <Unsorted.h>
#include <MouseClass.h>
#include <InputManagerClass.h>
#include <GameOptionsClass.h>
#include <SessionClass.h>
#include <algorithm>
#include <bit>

#include <cstddef>
#include <cstdint>

namespace rareplay::game_api::native
{
namespace
{
// GameSpeed 是 GameOptionsClass（0xA8EB60）的首个成员，GetAnimSpeed（0x5FB2E0）读的就是它；YRpp 声明漂移时编译期失败。
static_assert(offsetof(GameOptionsClass, GameSpeed) == 0);
// 快进后恢复斜坡读的三个音量（0xA8EB98、0xA8EB9C、0xA8EBA0），与原生 setter 0x5FA510、0x5FA590、0x5FA4A0 读写的位置相同。
// YRpp 把 +0x40 命名为 MovieVolume，原生音乐 setter 与 SaveSettings 都把它当作 ScoreVolume。
static_assert(offsetof(GameOptionsClass, SoundVolume) == 0x38);
static_assert(offsetof(GameOptionsClass, VoiceVolume) == 0x3C);
static_assert(offsetof(GameOptionsClass, MovieVolume) == 0x40);

using HandleAnyFunction = int(__fastcall*)(int, int, void*, int*, int*);
using PostFrameEventsFunction = void(__cdecl*)();
using FrameParserFunction = void(__cdecl*)();
using PreworldWaitFunction = bool(__cdecl*)();
// ECX = 斜坡，EDX = 目标值（16.16 定点的整数部分，函数内左移 16 位）；retn，无栈参数。
using AudioRampSetTargetFunction = void(__fastcall*)(void* ramp, int value);
// CL = 是否清空队列；retn，无栈参数。
using VoxStopCurrentFunction = void(__fastcall*)(bool clearQueue);

constexpr std::uintptr_t VOLUME_RAMPS[] = {
    target::SOUND_VOLUME_RAMP, target::VOICE_VOLUME_RAMP, target::SCORE_VOLUME_RAMP};

// 斜坡指针全局在音频初始化前为空；为空时跳过，与原生 setter 的空指针检查相同。
void SetVolumeRamp(std::uintptr_t rampGlobal, int value) noexcept
{
    void* const ramp = *reinterpret_cast<void* const*>(rampGlobal);
    if (ramp != nullptr)
        reinterpret_cast<AudioRampSetTargetFunction>(target::AUDIO_RAMP_SET_TARGET_IMMEDIATE)(ramp, value);
}

void StopCurrentVoice() noexcept
{
    reinterpret_cast<VoxStopCurrentFunction>(target::VOX_STOP_CURRENT)(true);
}

int ToRampVolume(float volume) noexcept
{
    return Game::F2I(static_cast<double>(volume) * target::AUDIO_RAMP_VOLUME_SCALE);
}
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

void RestoreRandomSeedForInitialization(std::uint32_t seed)
{
    Game::Seed = std::bit_cast<std::int32_t>(seed);
}

int ReadGameSpeed()
{
    return GameOptionsClass::Instance.GameSpeed;
}

void WriteGameSpeed(int speed)
{
    GameOptionsClass::Instance.GameSpeed = speed;
}

std::uint32_t ReadSessionGameMode() noexcept
{
    return static_cast<std::uint32_t>(SessionClass::Instance.GameMode);
}

void ClearFrameBudgets() noexcept
{
    *reinterpret_cast<volatile int*>(target::FRAME_BUDGET) = 0;
    *reinterpret_cast<volatile int*>(target::FRAME_WAIT_BUDGET) = 0;
}

void RequestFullRedraw() noexcept
{
    // 与 YRpp 的 MapClass::RedrawSidebar 同一地址与约定（thiscall、一个栈参数、retn 4）；参数 2 同时请求战术区与侧栏、命令条完整重绘。
    reinterpret_cast<void(__thiscall*)(MouseClass*, int)>(target::GSCREEN_MARK_NEEDS_REDRAW)(
        &MouseClass::Instance, target::FULL_REDRAW_WITH_SIDEBAR);
}

void MuteAudioForSeek() noexcept
{
    StopCurrentVoice();
    for (const std::uintptr_t ramp : VOLUME_RAMPS)
        SetVolumeRamp(ramp, 0);
}

void RestoreAudioAfterSeek() noexcept
{
    StopCurrentVoice();
    const GameOptionsClass& options = GameOptionsClass::Instance;
    SetVolumeRamp(target::SOUND_VOLUME_RAMP, ToRampVolume(options.SoundVolume));
    SetVolumeRamp(target::VOICE_VOLUME_RAMP, ToRampVolume(options.VoiceVolume));
    SetVolumeRamp(target::SCORE_VOLUME_RAMP, ToRampVolume(options.MovieVolume));
}

void WriteNoViewerInputKey(void* outKeyCode) noexcept
{
    if (outKeyCode != nullptr)
        *static_cast<volatile std::uint32_t*>(outKeyCode) = 0;
}
}
