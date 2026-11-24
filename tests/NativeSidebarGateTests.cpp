#include "../src/plugin/NativeSidebarGate.h"

#include <iostream>
#include <stdexcept>

namespace
{
using rareplay::plugin::NativeSidebarAction;
using rareplay::plugin::NativeSidebarGate;
using rareplay::plugin::NativeSidebarGateInput;
using rareplay::plugin::PANEL_PRESENCE_TIMEOUT;
using rareplay::plugin::ShouldSkipNativeSidebarChrome;
using namespace std::chrono_literals;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

NativeSidebarGateInput MakeSkippableInput()
{
    NativeSidebarGateInput input;
    input.mNow = std::chrono::steady_clock::time_point(10s);
    input.mIsPlaybackActive = true;
    input.mIsHudVisible = true;
    input.mPanelPresentedAt = input.mNow - 16ms;
    input.mIsSidebarOnRight = true;
    input.mIsArmageddonMode = false;
    return input;
}

void CheckPureConditions()
{
    Require(ShouldSkipNativeSidebarChrome(MakeSkippableInput()), "All conditions met allows skipping");

    auto input = MakeSkippableInput();
    input.mIsPlaybackActive = false;
    Require(!ShouldSkipNativeSidebarChrome(input), "Recording or inactive playback draws native sidebar");

    input = MakeSkippableInput();
    input.mIsHudVisible = false;
    Require(!ShouldSkipNativeSidebarChrome(input), "F10-hidden HUD draws native sidebar");

    input = MakeSkippableInput();
    input.mPanelPresentedAt.reset();
    Require(!ShouldSkipNativeSidebarChrome(input), "Never presented panel draws native sidebar");

    input = MakeSkippableInput();
    input.mPanelPresentedAt = input.mNow - PANEL_PRESENCE_TIMEOUT;
    Require(ShouldSkipNativeSidebarChrome(input), "Heartbeat exactly at the timeout is still fresh");
    input.mPanelPresentedAt = input.mNow - PANEL_PRESENCE_TIMEOUT - 1ms;
    Require(!ShouldSkipNativeSidebarChrome(input), "Stale heartbeat draws native sidebar");
    input.mPanelPresentedAt = input.mNow + 3ms;
    Require(ShouldSkipNativeSidebarChrome(input), "Render clock slightly ahead counts as fresh");

    input = MakeSkippableInput();
    input.mIsSidebarOnRight = false;
    Require(!ShouldSkipNativeSidebarChrome(input), "Left sidebar layout draws native sidebar");

    input = MakeSkippableInput();
    input.mIsArmageddonMode = true;
    Require(!ShouldSkipNativeSidebarChrome(input), "Armageddon mode draws native sidebar");
}

void CheckSingleRedrawOnRestore()
{
    NativeSidebarGate gate;
    auto input = MakeSkippableInput();
    input.mIsHudVisible = false;
    Require(gate.Update(input) == NativeSidebarAction::DrawNative, "Native at start needs no redraw");
    Require(gate.Update(input) == NativeSidebarAction::DrawNative, "Staying native never redraws");

    input = MakeSkippableInput();
    Require(gate.Update(input) == NativeSidebarAction::SkipChrome && gate.IsSkipping(), "Panel presence starts skip");
    input.mNow += 16ms;
    input.mPanelPresentedAt = input.mNow;
    Require(gate.Update(input) == NativeSidebarAction::SkipChrome, "Fresh heartbeat keeps skipping");

    // F10 隐藏：只在切回原生的第一帧请求一次重画。
    input.mIsHudVisible = false;
    Require(gate.Update(input) == NativeSidebarAction::RedrawNative && !gate.IsSkipping(),
            "First native frame after skipping requests a full redraw");
    Require(gate.Update(input) == NativeSidebarAction::DrawNative, "Redraw is requested only once");
    Require(gate.Update(input) == NativeSidebarAction::DrawNative, "Later native frames do not repeat redraw");

    // 心跳超时同样只重画一次。
    input = MakeSkippableInput();
    Require(gate.Update(input) == NativeSidebarAction::SkipChrome, "Panel shown again resumes skip");
    input.mNow += PANEL_PRESENCE_TIMEOUT + 1ms;
    Require(gate.Update(input) == NativeSidebarAction::RedrawNative, "Heartbeat timeout restores native once");
    Require(gate.Update(input) == NativeSidebarAction::DrawNative, "Timeout redraw is not repeated");

    // 回放结束（会话停止）也交回原生并重画一次。
    input = MakeSkippableInput();
    gate.Update(input);
    input.mIsPlaybackActive = false;
    Require(gate.Update(input) == NativeSidebarAction::RedrawNative, "Leaving playback restores native once");
    Require(gate.Update(input) == NativeSidebarAction::DrawNative, "Inactive playback stays native");
}
}

int main()
{
    try
    {
        CheckPureConditions();
        CheckSingleRedrawOnRestore();
        std::cout << "Native sidebar gate tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
