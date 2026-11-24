#include "../src/render/OverlayInputQueue.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

LPARAM Position(int x, int y) { return MAKELPARAM(x, y); }

// 滚轮消息的 wParam：低字为按键状态，高字为有符号滚动量（WHEEL_DELTA 的倍数或高精度滚轮的零碎值）。
WPARAM Wheel(int delta, WORD keys = MK_CONTROL)
{
    return MAKEWPARAM(keys, static_cast<WORD>(static_cast<short>(delta)));
}

// Ctrl+滚轮：区域外吞掉并累积档数；区域内、无 Ctrl、回放未激活时保持原有路由；F10 隐藏时仍然缩放。
// 测试窗口为空：ScreenToClient 失败时不改坐标，滚轮坐标即按客户区坐标判断。
void CheckBattlefieldZoomWheel()
{
    using rareplay::render::OverlayInputQueue;
    const std::vector<rareplay::ui::UiRect> regions{{100, 100, 250, 250}};
    OverlayInputQueue input;
    input.PublishPolicy(true, true, false, regions);

    Require(input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA), Position(30, 30)),
            "Ctrl+wheel over the map is swallowed");
    Require(input.Drain().empty(), "Swallowed Ctrl+wheel does not reach ImGui");
    Require(input.TakeBattlefieldZoomSteps() == 1, "One notch forward zooms in one level");
    Require(input.TakeBattlefieldZoomSteps() == 0, "Taking the steps clears them");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(-2 * WHEEL_DELTA), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == -2, "Two notches backward zoom out two levels");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA), Position(30, 30));
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 2, "Steps accumulate until taken");

    // 高精度滚轮：不足一格不产生档位，余量保留。
    Require(input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(50), Position(30, 30)),
            "A partial notch is still swallowed");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(50), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 0, "100 of 120 is not a level yet");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(50), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 1, "150 makes one level and keeps 30");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(90), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 1, "The kept 30 plus 90 makes another level");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(60), Position(30, 30));
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(-60), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 0, "Reversing drops the old remainder instead of cancelling it");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(-60), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == -1, "Two backward halves make one level out");
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(5 * WHEEL_DELTA + 7), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 5, "A large delta makes several levels at once");

    // 区域内：照常交给 ImGui 滚动面板，不缩放。
    input.Drain();
    Require(input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA), Position(120, 120)),
            "Ctrl+wheel over the HUD is consumed by the HUD");
    auto queued = input.Drain();
    Require(queued.size() == 1 && queued[0].message == WM_MOUSEWHEEL, "Ctrl+wheel over the HUD reaches ImGui");
    Require(input.TakeBattlefieldZoomSteps() == 0, "Ctrl+wheel over the HUD does not zoom");

    // 没按 Ctrl：地图上的滚轮照常交给游戏，HUD 上的照常交给 ImGui。
    Require(!input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA, 0), Position(30, 30)),
            "Plain wheel over the map reaches the game");
    Require(input.Drain().empty() && input.TakeBattlefieldZoomSteps() == 0, "Plain wheel over the map does not zoom");
    Require(input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA, MK_SHIFT), Position(120, 120)),
            "Plain wheel over the HUD is consumed");
    Require(input.Drain().size() == 1 && input.TakeBattlefieldZoomSteps() == 0, "Plain wheel over the HUD scrolls it");
    Require(!input.RouteMessage(nullptr, WM_MOUSEHWHEEL, Wheel(WHEEL_DELTA), Position(30, 30)),
            "Ctrl+horizontal wheel keeps its native route");
    Require(input.TakeBattlefieldZoomSteps() == 0, "Horizontal wheel never zooms");

    // 其他鼠标消息与按键不受 Ctrl 影响。
    Require(!input.RouteMessage(nullptr, WM_MOUSEMOVE, MK_CONTROL, Position(30, 30)), "Ctrl+move over the map passes");
    Require(!input.RouteMessage(nullptr, WM_LBUTTONDOWN, MK_CONTROL | MK_LBUTTON, Position(30, 30)),
            "Ctrl+click on the map passes");
    input.RouteMessage(nullptr, WM_LBUTTONUP, MK_CONTROL, Position(30, 30));
    Require(input.RouteMessage(nullptr, WM_LBUTTONDOWN, MK_CONTROL | MK_LBUTTON, Position(120, 120)),
            "Ctrl+click on the HUD is still captured");
    Require(input.RouteMessage(nullptr, WM_LBUTTONUP, MK_CONTROL, Position(30, 30)),
            "The captured release stays paired");
    Require(!input.RouteMessage(nullptr, WM_KEYDOWN, VK_CONTROL, 0), "The Ctrl key itself reaches the game");
    Require(input.TakeBattlefieldZoomSteps() == 0, "Only wheel messages produce zoom steps");
    input.Drain();

    // F10 隐藏 HUD：没有输入区域，原来的 HUD 位置也缩放；没按 Ctrl 的滚轮交给游戏。
    input.PublishPolicy(true, false, false, {});
    Require(input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA), Position(120, 120)),
            "Ctrl+wheel still zooms while the HUD is hidden");
    Require(input.Drain().empty() && input.TakeBattlefieldZoomSteps() == 1, "Hidden HUD zoom step");
    Require(!input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA, 0), Position(120, 120)),
            "Plain wheel reaches the game while the HUD is hidden");

    // 回放未激活：Ctrl+滚轮交给游戏，余量清零。
    input.PublishPolicy(true, true, false, regions);
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(100), Position(30, 30));
    input.PublishPolicy(false, true, false, regions);
    Require(!input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA), Position(30, 30)),
            "Ctrl+wheel reaches the game when playback is inactive");
    Require(input.TakeBattlefieldZoomSteps() == 0, "Inactive playback produces no zoom steps");
    input.PublishPolicy(true, true, false, regions);
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(30), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 0, "The remainder was dropped while playback was inactive");

    // Clear：清掉未取出的档数与余量，即使策略随后马上重新发布。
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(WHEEL_DELTA + 100), Position(30, 30));
    input.Clear();
    Require(input.TakeBattlefieldZoomSteps() == 0, "Clear drops pending steps");
    input.PublishPolicy(true, true, false, regions);
    input.RouteMessage(nullptr, WM_MOUSEWHEEL, Wheel(30), Position(30, 30));
    Require(input.TakeBattlefieldZoomSteps() == 0, "Clear drops the remainder");
}
}

int main()
{
    try
    {
        rareplay::render::OverlayInputQueue input;
        const std::vector<rareplay::ui::UiRect> regions{{100, 100, 250, 250}, {10, 270, 90, 330}};
        input.PublishPolicy(true, true, false, regions);

        Require(!input.RouteMessage(nullptr, WM_LBUTTONDOWN, 0, Position(30, 30)), "Map down passes through");
        Require(input.Drain().empty(), "Map button must not reach ImGui and alter native mouse capture");
        input.RouteMessage(nullptr, WM_MOUSEMOVE, MK_LBUTTON, Position(120, 120));
        Require(!input.RouteMessage(nullptr, WM_LBUTTONUP, 0, Position(120, 120)), "Map drag ends on UI without capture theft");
        for (const auto& message : input.Drain())
            Require(message.message != WM_LBUTTONDOWN && message.message != WM_LBUTTONUP, "Map drag has no ImGui button pair");

        Require(input.RouteMessage(nullptr, WM_LBUTTONDOWN, 0, Position(120, 120)), "UI down consumed");
        Require(input.RouteMessage(nullptr, WM_LBUTTONUP, 0, Position(30, 30)), "UI up remains paired outside UI");
        const auto uiClick = input.Drain();
        Require(uiClick.size() == 2 && uiClick[0].message == WM_LBUTTONDOWN && uiClick[1].message == WM_LBUTTONUP,
            "Only a complete UI button pair is delivered");

        HWND testWindow = CreateWindowExW(0, L"STATIC", L"ReplayInputTest", WS_POPUP,
            0, 0, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(testWindow != nullptr, "Hidden input fixture window created");
        SetCapture(testWindow);
        Require(input.RouteMessage(testWindow, WM_LBUTTONDOWN, 0, Position(120, 120)), "Existing game capture on same window allows UI click");
        input.RouteMessage(testWindow, WM_LBUTTONUP, 0, Position(120, 120));
        Require(GetCapture() == testWindow, "UI does not release pre-existing game capture");
        ReleaseCapture();
        Require(input.RouteMessage(testWindow, WM_LBUTTONDOWN, 0, Position(120, 120)), "UI can own capture when game has none");
        input.RouteMessage(testWindow, WM_LBUTTONUP, 0, Position(30, 30));
        Require(GetCapture() == nullptr, "UI releases only its owned capture");

        // 其他窗口留下未释放的捕获：HUD 点击接管捕获并照常消费，抬起时释放自己接管的捕获。
        HWND staleCaptureWindow = CreateWindowExW(0, L"STATIC", L"ReplayInputStaleCapture", WS_POPUP,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(staleCaptureWindow != nullptr, "Hidden stale capture window created");
        SetCapture(staleCaptureWindow);
        Require(GetCapture() == staleCaptureWindow, "Another window holds a stale capture");
        Require(input.RouteMessage(testWindow, WM_LBUTTONDOWN, 0, Position(120, 120)),
            "UI click takes over a stale capture left by another window");
        Require(GetCapture() == testWindow, "UI moves the stale capture to its own window");
        input.RouteMessage(testWindow, WM_LBUTTONUP, 0, Position(30, 30));
        Require(GetCapture() == nullptr, "UI releases the capture it took over");
        DestroyWindow(staleCaptureWindow);
        DestroyWindow(testWindow);
        input.Drain();

        Require(input.RouteMessage(nullptr, WM_LBUTTONDOWN, 0, Position(20, 290)), "Popup region also consumes clicks");
        input.RouteMessage(nullptr, WM_LBUTTONUP, 0, Position(20, 290));
        input.Drain();
        input.PublishPolicy(true, true, true, regions);
        Require(!input.RouteMessage(nullptr, WM_SYSKEYDOWN, VK_F4, 0), "Alt F4 must not be swallowed by UI keyboard focus");
        Require(!input.RouteMessage(nullptr, WM_CLOSE, 0, 0), "Window close always passes through");
        Require(!input.RouteMessage(nullptr, WM_SIZE, 0, 0), "Resize always passes through");
        input.PublishPolicy(false, true, true, regions);
        Require(!input.RouteMessage(nullptr, WM_LBUTTONDOWN, 0, Position(120, 120)), "Inactive replay does not consume input");

        input.Clear();
        input.PublishPolicy(true, true, false, regions);
        for (int index = 0; index < 1000; ++index)
            input.RouteMessage(nullptr, WM_MOUSEMOVE, 0, Position(120, 120));
        Require(input.Drain().size() <= rareplay::render::OverlayInputQueue::CAPACITY, "Queue stays bounded");
        CheckBattlefieldZoomWheel();
        std::cout << "Overlay input routing tests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
