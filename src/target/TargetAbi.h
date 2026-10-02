#pragma once

#include <cstdint>

namespace rareplay::target
{
// 已核实的 gamemd.exe x86 固定版本；仅底层适配器与 Hook 使用。
constexpr std::uintptr_t START_SESSION = 0x52D9A0;
constexpr std::uintptr_t RANDOM_SEED_DEFAULT_READ = 0x52FDF9;
constexpr std::uintptr_t RANDOM_SEED_GUARDED_READ = 0x52FE5C;
constexpr std::uintptr_t MAIN_LOOP = 0x55D360;
constexpr std::uintptr_t VIEWER_LOOP_CONTINUE = 0x55D8F7;
constexpr std::uintptr_t VIEWER_HELD_LOOP = 0x55D830;
constexpr std::uintptr_t VIEWER_EXIT_LOOP = 0x55DEC8;
constexpr std::uintptr_t GAME_OVER_CONTINUE = 0x48CEA1;
constexpr std::uintptr_t NATIVE_GAME_OVER = 0x48C8B0;
constexpr std::uintptr_t NATIVE_DRAW_ON_TOP = 0x4F4480;
// 原生 KeyboardProcess 将此槽当作四方向位，不能按 SDK 的 FileName 指针解释。
constexpr std::uintptr_t VIEWER_SCROLL_KEYS = 0xABCE14;
constexpr std::uintptr_t BEFORE_SIMULATION = 0x55DC99;
constexpr std::uintptr_t EXECUTE_EVENTS = 0x64C380;
constexpr std::uintptr_t FRAME_PARSER_CALL = 0x55DE40;
constexpr std::uintptr_t FRAME_PARSER_CONTINUE = 0x55DE45;
constexpr std::uintptr_t FRAME_COMPLETE = 0x55DE7E;
constexpr std::uintptr_t PREWORLD_WAIT_CALL = 0x684B3E;
constexpr std::uintptr_t PREWORLD_WAIT_CONTINUE = 0x684B43;
constexpr std::uintptr_t SESSION_END = 0x48CEAF;
constexpr std::uintptr_t SIDEBAR_COMMAND_ENTRY = 0x6AAD00;
constexpr std::uintptr_t SIDEBAR_COMMAND_BLOCK_RETURN = 0x6AAD20;
constexpr std::uintptr_t VIEWER_SPEED_GETTER = 0x4E209E;
constexpr std::uintptr_t VIEWER_SPEED_GETTER_CONTINUE = 0x4E20A4;
constexpr std::uintptr_t VIEWER_SPEED_SETTER = 0x4E1E1B;
constexpr std::uintptr_t VIEWER_SPEED_SETTER_CONTINUE = 0x4E1EC0;
constexpr std::uintptr_t VIEWER_FRAME_PACING = 0x55D79E;
constexpr std::uintptr_t VIEWER_FRAME_PACING_CONTINUE = 0x55D7A4;
constexpr std::uintptr_t VIEWER_MOUSE_INPUT_BEGIN = 0x4F4320;
constexpr std::uintptr_t VIEWER_MOUSE_INPUT_END = 0x4F43DC;
constexpr std::uintptr_t VIEWER_KEYBOARD_INPUT_BEGIN = 0x55DEE0;
constexpr std::uintptr_t VIEWER_CONTROLLED_BY_PLAYER = 0x50B6F0;
constexpr std::uintptr_t VIEWER_CONTROLLED_FALSE_RETURN = 0x50B70D;
constexpr std::uintptr_t ANIMATION_CONSTRUCTOR = 0x421EA0;
constexpr std::uintptr_t NATIVE_FRAME_PARSER = 0x647260;
constexpr std::uintptr_t NATIVE_PREWORLD_WAIT = 0x684370;
constexpr std::uintptr_t POST_FRAME_EVENTS = 0x64D990;
constexpr std::uintptr_t EXECUTOR_CONTEXT = 0xA8B548;
constexpr std::uintptr_t EXIT_REQUEST = 0xA83D48;
constexpr std::uintptr_t OPTIONS_REQUEST = 0xA8EDA0;
constexpr std::uintptr_t WORLD_READY = 0xA8ED5C;
}
