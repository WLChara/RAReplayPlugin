#include "ReplayHudDemoData.h"

#include "../src/ui/ReplayHud.h"
#include "../src/ui/ReplayHudLayout.h"
#include "../src/ui/ReplayHudScale.h"
#include "../src/render/ReplayOverlayRenderer.h"
#include "../src/render/OverlayFonts.h"
#include "../src/viewer/ReplayViewExchange.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#include <Windows.h>
#include <gl/GL.h>

#include <algorithm>
#include <array>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
struct PreviewOptions
{
    std::filesystem::path mCapturePath;
    int mWidth = 1600;
    int mHeight = 900;
    // 0 表示游戏画布与窗口同尺寸；否则模拟 cnc-ddraw 把该画布拉伸到窗口。
    int mCanvasWidth = 0;
    int mCanvasHeight = 0;
    rareplay::ui::ReplayPanel mPanel = rareplay::ui::ReplayPanel::Overview;
    rareplay::viewer::HudScaleSetting mHudScale = rareplay::viewer::HudScaleSetting::Percent100;
    std::optional<rareplay::viewer::UnitCategory> mUnitCategory;
    // 经济曲线大窗口：是否打开、只看某位玩家（房屋下标）与时间范围。
    bool mEconomyWindow = false;
    std::optional<int> mEconomyOnlyHouse;
    rareplay::ui::EconomyTimeRange mEconomyRange = rareplay::ui::EconomyTimeRange::All;
    bool mMissingData = false;
    bool mNominalTime = false;
    // 雷达事件提示栏：是否附带示例事件、折叠、历史模式；以及是否发布示例超武落点图标。
    bool mRadar = false;
    bool mRadarCollapsed = false;
    bool mRadarHistory = false;
    bool mMarks = false;
    bool mCapture = false;
    bool mHostSmoke = false;
    rareplay::viewer::PlaybackStatus mPlaybackStatus = rareplay::viewer::PlaybackStatus::Playing;
    bool mRestartError = false;
    // 示例“快进中”状态（进度、取消与禁用的控件）。
    bool mSeeking = false;
};

bool ParseInteger(const wchar_t* text, int& outValue)
{
    if (text == nullptr || *text == L'\0')
    {
        return false;
    }
    wchar_t* end = nullptr;
    const long value = wcstol(text, &end, 10);
    if (end == text || *end != L'\0' || value < 320 || value > 4096)
    {
        return false;
    }
    outValue = static_cast<int>(value);
    return true;
}

bool ParseArguments(int argc, wchar_t** argv, PreviewOptions& options)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        if (argument == L"--capture")
        {
            if (++index >= argc || argv[index][0] == L'\0')
            {
                return false;
            }
            options.mCapture = true;
            options.mCapturePath = argv[index];
        }
        else if (argument == L"--width")
        {
            if (++index >= argc || !ParseInteger(argv[index], options.mWidth))
            {
                return false;
            }
        }
        else if (argument == L"--height")
        {
            if (++index >= argc || !ParseInteger(argv[index], options.mHeight))
            {
                return false;
            }
        }
        else if (argument == L"--canvas-width")
        {
            if (++index >= argc || !ParseInteger(argv[index], options.mCanvasWidth))
            {
                return false;
            }
        }
        else if (argument == L"--canvas-height")
        {
            if (++index >= argc || !ParseInteger(argv[index], options.mCanvasHeight))
            {
                return false;
            }
        }
        else if (argument == L"--panel")
        {
            if (++index >= argc)
            {
                return false;
            }
            const std::wstring panel = argv[index];
            if (panel == L"overview")
                options.mPanel = rareplay::ui::ReplayPanel::Overview;
            else if (panel == L"production")
                options.mPanel = rareplay::ui::ReplayPanel::Production;
            else if (panel == L"units")
                options.mPanel = rareplay::ui::ReplayPanel::Units;
            else if (panel == L"groups")
                options.mPanel = rareplay::ui::ReplayPanel::Groups;
            else if (panel == L"economy")
                options.mPanel = rareplay::ui::ReplayPanel::Economy;
            else
                return false;
        }
        else if (argument == L"--scale")
        {
            if (++index >= argc)
                return false;
            const std::wstring scale = argv[index];
            const int value = scale == L"auto" ? 0 : static_cast<int>(wcstol(scale.c_str(), nullptr, 10));
            const auto setting = rareplay::viewer::ToHudScaleSetting(value);
            if (!setting || (value == 0 && scale != L"auto"))
                return false;
            options.mHudScale = *setting;
        }
        else if (argument == L"--unit-category")
        {
            if (++index >= argc)
                return false;
            const std::wstring category = argv[index];
            if (category == L"all")
                options.mUnitCategory.reset();
            else if (category == L"infantry")
                options.mUnitCategory = rareplay::viewer::UnitCategory::Infantry;
            else if (category == L"vehicle")
                options.mUnitCategory = rareplay::viewer::UnitCategory::Vehicle;
            else if (category == L"aircraft")
                options.mUnitCategory = rareplay::viewer::UnitCategory::Aircraft;
            else if (category == L"building")
                options.mUnitCategory = rareplay::viewer::UnitCategory::Building;
            else if (category == L"other")
                options.mUnitCategory = rareplay::viewer::UnitCategory::Other;
            else
                return false;
        }
        else if (argument == L"--economy-window")
        {
            options.mEconomyWindow = true;
        }
        else if (argument == L"--economy-only")
        {
            if (++index >= argc)
                return false;
            wchar_t* end = nullptr;
            const long house = wcstol(argv[index], &end, 10);
            if (end == argv[index] || *end != L'\0' || house < 0 || house > 64)
                return false;
            options.mEconomyOnlyHouse = static_cast<int>(house);
        }
        else if (argument == L"--economy-range")
        {
            if (++index >= argc)
                return false;
            const std::wstring range = argv[index];
            if (range == L"all")
                options.mEconomyRange = rareplay::ui::EconomyTimeRange::All;
            else if (range == L"10")
                options.mEconomyRange = rareplay::ui::EconomyTimeRange::LastTenMinutes;
            else if (range == L"5")
                options.mEconomyRange = rareplay::ui::EconomyTimeRange::LastFiveMinutes;
            else
                return false;
        }
        else if (argument == L"--radar")
        {
            if (++index >= argc)
                return false;
            const std::wstring mode = argv[index];
            options.mRadar = true;
            options.mRadarCollapsed = mode == L"collapsed";
            options.mRadarHistory = mode == L"history";
            if (mode != L"expanded" && mode != L"collapsed" && mode != L"history")
                return false;
        }
        else if (argument == L"--marks")
        {
            options.mMarks = true;
        }
        else if (argument == L"--missing-data")
        {
            options.mMissingData = true;
        }
        else if (argument == L"--time-nominal")
        {
            options.mNominalTime = true;
        }
        else if (argument == L"--host-smoke")
        {
            options.mHostSmoke = true;
        }
        else if (argument == L"--playback-state")
        {
            if (++index >= argc)
                return false;
            const std::wstring state = argv[index];
            if (state == L"playing")
                options.mPlaybackStatus = rareplay::viewer::PlaybackStatus::Playing;
            else if (state == L"paused")
                options.mPlaybackStatus = rareplay::viewer::PlaybackStatus::Paused;
            else if (state == L"ended")
                options.mPlaybackStatus = rareplay::viewer::PlaybackStatus::Ended;
            else
                return false;
        }
        else if (argument == L"--restart-error")
        {
            options.mRestartError = true;
        }
        else if (argument == L"--seeking")
        {
            options.mSeeking = true;
        }
        else
        {
            return false;
        }
    }
    if ((options.mCanvasWidth == 0) != (options.mCanvasHeight == 0))
        return false;
    return (!options.mCapture || !options.mCapturePath.empty()) && (!options.mHostSmoke || options.mCapture);
}

void PrintUsage()
{
    std::wcerr << L"用法：ReplayHudPreview [--capture <BMP路径>] [--width 1600] [--height 900] "
                  L"[--canvas-width 1024 --canvas-height 768（模拟拉伸）] "
                  L"[--panel overview|production|units|groups|economy] [--scale auto|100|125|150|175|200] "
                  L"[--unit-category all|infantry|vehicle|aircraft|building|other] "
                  L"[--economy-window（打开资金曲线大窗口）] [--economy-only 房屋下标（只看该玩家）] "
                  L"[--economy-range all|10|5（大窗口时间范围，分钟）] "
                  L"[--radar expanded|collapsed|history（雷达事件提示栏及示例事件）] "
                  L"[--marks（发布示例超武落点图标，并画出示意地图）] "
                  L"[--missing-data] [--time-nominal（示例游戏时间带“名义”标记）] "
                  L"[--host-smoke（需截图模式）] [--playback-state playing|paused|ended] [--restart-error] "
                  L"[--seeking（示例快进中状态）]\n";
}

class PreviewApp final
{
public:
    explicit PreviewApp(PreviewOptions options)
        : mOptions(std::move(options))
    {
    }

    int Run()
    {
        if (!CreatePreviewWindow() || !CreateGlContext() || !InitializeImGui())
        {
            Destroy();
            return 1;
        }

        rareplay::tools::ReplayHudDemoOptions demoOptions;
        demoOptions.mMissingData = mOptions.mMissingData;
        demoOptions.mNominalTime = mOptions.mNominalTime;
        demoOptions.mRadarEvents = mOptions.mRadar;
        demoOptions.mSeeking = mOptions.mSeeking;
        demoOptions.mCanvasWidth = mOptions.mCanvasWidth != 0 ? mOptions.mCanvasWidth : mOptions.mWidth;
        demoOptions.mCanvasHeight = mOptions.mCanvasHeight != 0 ? mOptions.mCanvasHeight : mOptions.mHeight;
        mSnapshot = rareplay::tools::CreateReplayHudDemoSnapshot(demoOptions);
        mSnapshot->mPlaybackControls.mStatus = mOptions.mPlaybackStatus;
        // 与游戏线程的口径一致：回放结束后不能再跳转，快进也随之结束。
        if (mOptions.mPlaybackStatus == rareplay::viewer::PlaybackStatus::Ended)
        {
            mSnapshot->mPlaybackControls.mCanSeek = false;
            mSnapshot->mPlaybackControls.mSeek = {};
        }
        mSnapshot->mHudScaleSetting = mOptions.mHudScale;
        if (mOptions.mRestartError)
            mSnapshot->mPlaybackControls.mRestartError = "无法重新播放：启动器通知写入失败，当前回放已保留";
        mExchange.Publish(mSnapshot);
        mHud.SetIconResolver([this](const std::shared_ptr<const rareplay::viewer::IconImage>& image) {
            return ResolveIcon(image);
        });
        mHud.SelectPanel(mOptions.mPanel);
        mHud.SetRadarBarCollapsed(mOptions.mRadarCollapsed);
        mHud.SetRadarBarHistory(mOptions.mRadarHistory);
        if (mOptions.mMarks && mSnapshot->mGameTime)
            mExchange.PublishMarks(rareplay::tools::CreateReplayHudDemoMarks(demoOptions, mSnapshot->mGameTime->mMilliseconds));
        mHud.SelectUnitCategory(mOptions.mUnitCategory);
        mHud.SetEconomyWindowOpen(mOptions.mEconomyWindow);
        mHud.SelectEconomyRange(mOptions.mEconomyRange);
        if (mOptions.mEconomyOnlyHouse)
            mHud.ShowOnlyEconomyPlayer(*mOptions.mEconomyOnlyHouse);

        if (!mOptions.mCapture)
        {
            ShowWindow(mWindow, SW_SHOW);
            UpdateWindow(mWindow);
        }

        int result = 0;
        if (mOptions.mCapture)
        {
            result = CapturePreview() ? 0 : 2;
        }
        else
        {
            result = RunWindowLoop();
        }
        Destroy();
        return result;
    }

private:
    bool CreatePreviewWindow()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &PreviewApp::WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"RAReplayPlugin.ReplayHudPreview";

        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::wcerr << L"注册预览窗口失败：" << GetLastError() << L"\n";
            return false;
        }

        RECT windowRect{0, 0, mOptions.mWidth, mOptions.mHeight};
        // 截图窗口不显示；用无边框弹出窗口，避免系统把大于屏幕工作区的标题栏窗口缩小。
        const DWORD style = mOptions.mCapture ? WS_POPUP : WS_OVERLAPPEDWINDOW;
        AdjustWindowRect(&windowRect, style, FALSE);
        mWindow = CreateWindowExW(0, windowClass.lpszClassName, L"回放 HUD 布局预览",
                                  style, mOptions.mCapture ? 0 : CW_USEDEFAULT, mOptions.mCapture ? 0 : CW_USEDEFAULT,
                                  windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                                  nullptr, nullptr, windowClass.hInstance, this);
        if (mWindow == nullptr)
        {
            std::wcerr << L"创建预览窗口失败：" << GetLastError() << L"\n";
            return false;
        }

        RECT client{};
        if (mOptions.mCapture && (!GetClientRect(mWindow, &client) ||
            client.right - client.left != mOptions.mWidth || client.bottom - client.top != mOptions.mHeight))
        {
            std::wcerr << L"预览客户区不是请求的截图尺寸：" << client.right - client.left << L"×"
                       << client.bottom - client.top << L"\n";
            return false;
        }

        mDeviceContext = GetDC(mWindow);
        return mDeviceContext != nullptr;
    }

    bool CreateGlContext()
    {
        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        descriptor.cAlphaBits = 8;
        descriptor.cDepthBits = 0;
        descriptor.cStencilBits = 0;
        descriptor.iLayerType = PFD_MAIN_PLANE;

        const int pixelFormat = ChoosePixelFormat(mDeviceContext, &descriptor);
        if (pixelFormat == 0 || SetPixelFormat(mDeviceContext, pixelFormat, &descriptor) == FALSE)
        {
            std::wcerr << L"设置预览 OpenGL 像素格式失败：" << GetLastError() << L"\n";
            return false;
        }

        mGlContext = wglCreateContext(mDeviceContext);
        if (mGlContext == nullptr || wglMakeCurrent(mDeviceContext, mGlContext) == FALSE)
        {
            std::wcerr << L"创建预览 OpenGL 上下文失败：" << GetLastError() << L"\n";
            return false;
        }
        return true;
    }

    bool InitializeImGui()
    {
        mImGuiContext = ImGui::CreateContext();
        if (mImGuiContext == nullptr)
        {
            std::wcerr << L"创建预览 ImGui context 失败。\n";
            return false;
        }
        ImGui::SetCurrentContext(mImGuiContext);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        // 字体在 RenderFrame 中按 HUD 缩放装载，与游戏内宿主同一规则。

        if (!ImGui_ImplWin32_Init(mWindow))
        {
            std::wcerr << L"初始化预览 Win32 backend 失败。\n";
            return false;
        }
        mWin32BackendReady = true;
        if (!ImGui_ImplOpenGL3_Init("#version 130"))
        {
            std::wcerr << L"初始化预览 OpenGL backend 失败。\n";
            return false;
        }
        mOpenGlBackendReady = true;
        return true;
    }

    int RunWindowLoop()
    {
        MSG message{};
        while (!mCloseRequested)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            RenderFrame();
            SwapBuffers(mDeviceContext);
            // 点击消息可能同批到达；持续推进 ImGui 的输入队列，而不是等待下一次鼠标移动。
            MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        return 0;
    }

    bool CapturePreview()
    {
        constexpr int RENDER_FRAMES = 4;
        for (int frame = 0; frame < RENDER_FRAMES; ++frame)
        {
            RenderFrame();
        }
        if (!mOptions.mHostSmoke)
        {
            // 供审查核对：面板是否被视为完整覆盖原生侧栏，以及输入区域。
            std::wcout << L"hudScalePercent=" << mFontScalePercent << L"\n";
            std::wcout << L"sidebarPanelCovering=" << (mHud.IsSidebarPanelCovering() ? 1 : 0) << L"\n";
            for (const auto& region : mHud.GetInputRegions())
                std::wcout << L"inputRegion=" << region.mLeft << L"," << region.mTop << L","
                           << region.mRight << L"," << region.mBottom << L"\n";
            if (const auto& zoom = mHud.GetZoomControlRects())
                std::wcout << L"zoomControls=" << zoom->mGroup.mLeft << L"," << zoom->mGroup.mTop << L","
                           << zoom->mGroup.mRight << L"," << zoom->mGroup.mBottom << L"\n";
        }
        return SaveBackBuffer(mOptions.mCapturePath);
    }

    void RenderFrame()
    {
        if (mImGuiContext == nullptr || !mWin32BackendReady || !mOpenGlBackendReady)
        {
            return;
        }

        ImGui::SetCurrentContext(mImGuiContext);
        EnsureFontScale();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawNativeRadarPlaceholder();
        if (mOptions.mMarks || mOptions.mRadar)
            DrawNativeMapPlaceholder();
        if (!mOptions.mHostSmoke)
        {
            mHud.SetGameImagePlacement(GetPreviewImagePlacement());
            mHud.Draw(*mSnapshot, mExchange);
        }
        ImGui::Render();

        glViewport(0, 0, mOptions.mWidth, mOptions.mHeight);
        glClearColor(0.035f, 0.045f, 0.040f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (mOptions.mHostSmoke)
        {
            const HDC previousDc = wglGetCurrentDC();
            const HGLRC previousGl = wglGetCurrentContext();
            ImGuiContext* previousImGui = ImGui::GetCurrentContext();
            if (!rareplay::render::GetReplayOverlayRenderer().Render(mDeviceContext, mExchange))
                throw std::runtime_error("Overlay renderer failed in isolated host smoke");
            if (wglGetCurrentDC() != previousDc || wglGetCurrentContext() != previousGl ||
                ImGui::GetCurrentContext() != previousImGui)
                throw std::runtime_error("Overlay failed to restore host graphics context");
        }
        // 预览只模拟游戏线程对速度、界面缩放与跳转命令的处理（跳转只切换“快进中”的显示，帧号不前进）；
        // 其他命令没有可演示的效果。
        for (const auto& command : mExchange.Drain())
        {
            if (command.mSessionId != mSnapshot->mSessionId)
                continue;
            auto nextSnapshot = std::make_shared<rareplay::viewer::ReplayViewSnapshot>(*mSnapshot);
            const auto scale = rareplay::viewer::ToHudScaleSetting(command.mValue);
            auto& seek = nextSnapshot->mPlaybackControls.mSeek;
            if (command.mKind == rareplay::viewer::ViewerCommandKind::SetPlaybackSpeed &&
                command.mValue >= 0 && command.mValue <= 6)
                nextSnapshot->mPlaybackSpeedSetting = command.mValue;
            else if (command.mKind == rareplay::viewer::ViewerCommandKind::SetHudScale && scale)
                nextSnapshot->mHudScaleSetting = *scale;
            else if (command.mKind == rareplay::viewer::ViewerCommandKind::SeekToFrame && command.mValue >= 0 &&
                     static_cast<std::uint32_t>(command.mValue) > (seek.mIsSeeking ? seek.mTargetFrame : nextSnapshot->mFrameNumber))
                seek = {true, seek.mIsSeeking ? seek.mStartFrame : nextSnapshot->mFrameNumber,
                        static_cast<std::uint32_t>(command.mValue)};
            else if (command.mKind == rareplay::viewer::ViewerCommandKind::CancelSeek && seek.mIsSeeking)
                seek = {};
            else
                continue;
            mSnapshot = std::move(nextSnapshot);
            mExchange.Publish(mSnapshot);
        }
    }

    // 与游戏内宿主相同：按客户区高度解析档位，变化时销毁 GL 设备对象并重建字体图集。
    void EnsureFontScale()
    {
        RECT client{};
        GetClientRect(mWindow, &client);
        const int percent = rareplay::ui::ResolveHudScalePercent(mSnapshot->mHudScaleSetting,
            static_cast<float>(client.bottom - client.top));
        if (percent != mFontScalePercent)
        {
            ImGui_ImplOpenGL3_DestroyDeviceObjects();
            rareplay::render::LoadOverlayFont(ImGui::GetIO(), static_cast<float>(percent) / 100.0f);
            mFontScalePercent = percent;
        }
        mHud.SetScalePercent(percent);
    }

    // 预览模拟 cnc-ddraw OpenGL：游戏画布拉伸到整个客户区，最终视口即图像区域。
    rareplay::ui::GameImagePlacement GetPreviewImagePlacement() const
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        return {{0.0f, 0.0f, size.x, size.y}, true};
    }

    // 原生资金条与雷达块（y < SidebarBounds.Y）的位置示意；游戏内不会绘制此方格。
    void DrawNativeRadarPlaceholder()
    {
        if (!mSnapshot->mNativeUiLayout)
            return;
        const auto& layout = *mSnapshot->mNativeUiLayout;
        const rareplay::viewer::NativeRect reserve{layout.mSidebarPanel.mX, 0, layout.mSidebarPanel.mWidth,
                                                   layout.mSidebarPanel.mY};
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const auto placement = rareplay::ui::MapNativeRect(layout, reserve, size.x, size.y, GetPreviewImagePlacement());
        if (!placement)
            return;
        const ImVec2 topLeft(placement->mRect.mLeft, placement->mRect.mTop);
        const ImVec2 bottomRight(placement->mRect.mRight, placement->mRect.mBottom);
        ImDrawList* background = ImGui::GetBackgroundDrawList();
        background->AddRectFilled(topLeft, bottomRight, IM_COL32(19, 29, 25, 255));
        background->AddRect(topLeft, bottomRight, IM_COL32(87, 114, 96, 255));
        for (float x = topLeft.x + 12.0f; x < bottomRight.x; x += 24.0f)
            background->AddLine({x, topLeft.y + 30.0f}, {x, bottomRight.y - 6.0f}, IM_COL32(39, 56, 46, 255));
        for (float y = topLeft.y + 30.0f; y < bottomRight.y - 6.0f; y += 24.0f)
            background->AddLine({topLeft.x + 6.0f, y}, {bottomRight.x - 6.0f, y}, IM_COL32(39, 56, 46, 255));
        const ImVec4 clip(topLeft.x, topLeft.y, bottomRight.x, bottomRight.y);
        background->AddText(nullptr, 0.0f, {topLeft.x + 6.0f, topLeft.y + 6.0f}, IM_COL32(179, 195, 169, 255),
                            "原生资金条与小地图", nullptr, 0.0f, &clip);
    }

    // 战术视图的示意地图：深绿色地面加等距网格，只用来让落点图标与提示栏有背景；游戏内不会绘制。
    void DrawNativeMapPlaceholder()
    {
        if (!mSnapshot->mNativeUiLayout)
            return;
        const auto& layout = *mSnapshot->mNativeUiLayout;
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const auto mapper = rareplay::ui::MakeNativePointMapper(layout.mCanvasWidth, layout.mCanvasHeight, size.x,
                                                                size.y, GetPreviewImagePlacement());
        if (!mapper)
            return;
        const rareplay::ui::UiRect view = mapper->MapRect({0, 0, layout.mCanvasWidth - 168, layout.mCanvasHeight - 32});
        ImDrawList* background = ImGui::GetBackgroundDrawList();
        background->PushClipRect(ImVec2(view.mLeft, view.mTop), ImVec2(view.mRight, view.mBottom), true);
        background->AddRectFilled(ImVec2(view.mLeft, view.mTop), ImVec2(view.mRight, view.mBottom), IM_COL32(58, 82, 56, 255));
        const float cellWidth = 60.0f * (view.mRight - view.mLeft) / static_cast<float>(layout.mCanvasWidth - 168);
        const float cellHeight = cellWidth * 0.5f;
        const ImU32 lineColor = IM_COL32(72, 98, 68, 255);
        // 等距菱形网格：相邻两行错开半格。
        int row = 0;
        for (float y = view.mTop - cellHeight; y < view.mBottom + cellHeight; y += cellHeight * 0.5f, ++row)
        {
            const float offset = row % 2 == 0 ? 0.0f : cellWidth * 0.5f;
            for (float x = view.mLeft - cellWidth + offset; x < view.mRight + cellWidth; x += cellWidth)
            {
                background->AddQuad(ImVec2(x, y), ImVec2(x + cellWidth * 0.5f, y + cellHeight * 0.5f),
                                    ImVec2(x, y + cellHeight), ImVec2(x - cellWidth * 0.5f, y + cellHeight * 0.5f), lineColor);
            }
        }
        background->PopClipRect();
    }

    ImTextureID ResolveIcon(const std::shared_ptr<const rareplay::viewer::IconImage>& image)
    {
        if (!image || image->mWidth <= 0 || image->mHeight <= 0 || image->mRgba.empty())
        {
            return nullptr;
        }
        const auto found = mTextures.find(image.get());
        if (found != mTextures.end())
        {
            return reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(found->second));
        }

        GLint previousTexture = 0;
        GLint previousAlignment = 4;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
        GLuint texture = 0;
        glGenTextures(1, &texture);
        if (texture == 0)
        {
            return nullptr;
        }

        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->mWidth, image->mHeight, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image->mRgba.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        mTextures.emplace(image.get(), texture);
        return reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(texture));
    }

    bool SaveBackBuffer(const std::filesystem::path& path)
    {
        const std::size_t width = static_cast<std::size_t>(mOptions.mWidth);
        const std::size_t height = static_cast<std::size_t>(mOptions.mHeight);
        if (width > std::numeric_limits<std::size_t>::max() / height / 4)
        {
            return false;
        }
        std::vector<std::uint8_t> rgba(width * height * 4);
        GLint previousAlignment = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, mOptions.mWidth, mOptions.mHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);
        glFinish();

        std::vector<std::uint8_t> bgra(rgba.size());
        for (std::size_t pixel = 0; pixel < width * height; ++pixel)
        {
            bgra[pixel * 4 + 0] = rgba[pixel * 4 + 2];
            bgra[pixel * 4 + 1] = rgba[pixel * 4 + 1];
            bgra[pixel * 4 + 2] = rgba[pixel * 4 + 0];
            bgra[pixel * 4 + 3] = rgba[pixel * 4 + 3];
        }

        BITMAPFILEHEADER fileHeader{};
        BITMAPINFOHEADER infoHeader{};
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = mOptions.mWidth;
        // OpenGL 读回的数据从底行开始，与正高度 BMP 的行顺序一致。
        infoHeader.biHeight = mOptions.mHeight;
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biCompression = BI_RGB;
        infoHeader.biSizeImage = static_cast<DWORD>(bgra.size());
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + infoHeader.biSizeImage;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            std::wcerr << L"无法创建截图文件：" << path.wstring() << L"\n";
            return false;
        }
        file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
        file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
        file.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
        return file.good();
    }

    void Destroy()
    {
        if (mDeviceContext != nullptr && mGlContext != nullptr)
        {
            wglMakeCurrent(mDeviceContext, mGlContext);
            if (mImGuiContext != nullptr)
            {
                ImGui::SetCurrentContext(mImGuiContext);
                mHud.Reset();
                if (mOpenGlBackendReady)
                {
                    ImGui_ImplOpenGL3_Shutdown();
                }
                if (mWin32BackendReady)
                {
                    ImGui_ImplWin32_Shutdown();
                }
                ImGui::DestroyContext(mImGuiContext);
                mImGuiContext = nullptr;
            }
            for (auto& [image, texture] : mTextures)
            {
                (void)image;
                glDeleteTextures(1, &texture);
            }
            mTextures.clear();
            wglMakeCurrent(nullptr, nullptr);
        }
        if (mGlContext != nullptr)
        {
            wglDeleteContext(mGlContext);
            mGlContext = nullptr;
        }
        if (mWindow != nullptr && mDeviceContext != nullptr)
        {
            ReleaseDC(mWindow, mDeviceContext);
            mDeviceContext = nullptr;
        }
        if (mWindow != nullptr)
        {
            DestroyWindow(mWindow);
            mWindow = nullptr;
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        PreviewApp* app = reinterpret_cast<PreviewApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            app = static_cast<PreviewApp*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }

        if (app != nullptr && app->mImGuiContext != nullptr)
        {
            ImGui::SetCurrentContext(app->mImGuiContext);
            ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        }

        if (message == WM_CLOSE)
        {
            if (app != nullptr)
            {
                app->mCloseRequested = true;
            }
            PostQuitMessage(0);
            return 0;
        }
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    PreviewOptions mOptions;
    HWND mWindow = nullptr;
    HDC mDeviceContext = nullptr;
    HGLRC mGlContext = nullptr;
    ImGuiContext* mImGuiContext = nullptr;
    bool mWin32BackendReady = false;
    bool mOpenGlBackendReady = false;
    bool mCloseRequested = false;
    int mFontScalePercent = 0;
    rareplay::ui::ReplayHud mHud;
    rareplay::viewer::ReplayViewExchange mExchange;
    std::shared_ptr<rareplay::viewer::ReplayViewSnapshot> mSnapshot;
    std::unordered_map<const rareplay::viewer::IconImage*, GLuint> mTextures;
};
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        PreviewOptions options;
        if (!ParseArguments(argc, argv, options))
        {
            PrintUsage();
            return 2;
        }
        PreviewApp app(std::move(options));
        return app.Run();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "预览工具出错：" << exception.what() << '\n';
        return 1;
    }
}
