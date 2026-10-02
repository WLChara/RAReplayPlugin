#include "ReplayHudDemoData.h"

#include "../src/ui/ReplayHud.h"
#include "../src/ui/ReplayHudLayout.h"
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
    rareplay::tools::ReplayHudDemoPanel mPanel = rareplay::tools::ReplayHudDemoPanel::Production;
    bool mMissingData = false;
    bool mCapture = false;
    bool mHostSmoke = false;
    rareplay::viewer::PlaybackStatus mPlaybackStatus = rareplay::viewer::PlaybackStatus::Playing;
    bool mRestartError = false;
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
        else if (argument == L"--panel")
        {
            if (++index >= argc)
            {
                return false;
            }
            const std::wstring panel = argv[index];
            if (panel == L"production")
            {
                options.mPanel = rareplay::tools::ReplayHudDemoPanel::Production;
            }
            else if (panel == L"units")
            {
                options.mPanel = rareplay::tools::ReplayHudDemoPanel::Units;
            }
            else if (panel == L"groups")
            {
                options.mPanel = rareplay::tools::ReplayHudDemoPanel::Groups;
            }
            else
            {
                return false;
            }
        }
        else if (argument == L"--missing-data")
        {
            options.mMissingData = true;
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
        else
        {
            return false;
        }
    }
    return (!options.mCapture || !options.mCapturePath.empty()) && (!options.mHostSmoke || options.mCapture);
}

void PrintUsage()
{
    std::wcerr << L"用法：ReplayHudPreview [--capture <BMP路径>] [--width 1600] [--height 900] "
                  L"[--panel production|units|groups] [--missing-data] [--host-smoke（需截图模式）] "
                  L"[--playback-state playing|paused|ended] [--restart-error]\n";
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

        const rareplay::tools::ReplayHudDemoOptions demoOptions{mOptions.mPanel, mOptions.mMissingData};
        mSnapshot = rareplay::tools::CreateReplayHudDemoSnapshot(demoOptions);
        mSnapshot->mPlaybackControls.mStatus = mOptions.mPlaybackStatus;
        if (mOptions.mRestartError)
            mSnapshot->mPlaybackControls.mRestartError = "无法重新播放：启动器通知写入失败，当前回放已保留";
        mExchange.Publish(mSnapshot);
        mHud.SetIconResolver([this](const std::shared_ptr<const rareplay::viewer::IconImage>& image) {
            return ResolveIcon(image);
        });
        mHud.SelectPanel(ToUiPanel(mOptions.mPanel));

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
    static rareplay::ui::ReplayPanel ToUiPanel(rareplay::tools::ReplayHudDemoPanel panel)
    {
        switch (panel)
        {
        case rareplay::tools::ReplayHudDemoPanel::Units:
            return rareplay::ui::ReplayPanel::Units;
        case rareplay::tools::ReplayHudDemoPanel::Groups:
            return rareplay::ui::ReplayPanel::Groups;
        default:
            return rareplay::ui::ReplayPanel::Production;
        }
    }

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
        const DWORD style = WS_OVERLAPPEDWINDOW;
        AdjustWindowRect(&windowRect, style, FALSE);
        mWindow = CreateWindowExW(0, windowClass.lpszClassName, L"回放 HUD 布局预览",
                                  style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                                  nullptr, nullptr, windowClass.hInstance, this);
        if (mWindow == nullptr)
        {
            std::wcerr << L"创建预览窗口失败：" << GetLastError() << L"\n";
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

        rareplay::render::LoadOverlayFont(io);

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
        return SaveBackBuffer(mOptions.mCapturePath);
    }

    void RenderFrame()
    {
        if (mImGuiContext == nullptr || !mWin32BackendReady || !mOpenGlBackendReady)
        {
            return;
        }

        ImGui::SetCurrentContext(mImGuiContext);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawNativeRadarPlaceholder();
        if (!mOptions.mHostSmoke)
            mHud.Draw(*mSnapshot, mExchange);
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
        for (const auto& command : mExchange.Drain())
        {
            if (command.mSessionId != mSnapshot->mSessionId || command.mValue < 0 || command.mValue > 6)
                continue;
            auto nextSnapshot = std::make_shared<rareplay::viewer::ReplayViewSnapshot>(*mSnapshot);
            nextSnapshot->mPlaybackSpeedSetting = command.mValue;
            mSnapshot = std::move(nextSnapshot);
            mExchange.Publish(mSnapshot);
        }
    }

    void DrawNativeRadarPlaceholder()
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const auto bottom = rareplay::ui::GetSidebarPanelTop(mSnapshot->mNativeUiLayout,
            size.x, size.y, {});
        if (!bottom || *bottom < 75.0f)
            return;
        const ImVec2 topLeft(size.x - 190.0f, 14.0f);
        const ImVec2 bottomRight(size.x - 14.0f, *bottom - 8.0f);
        ImDrawList* background = ImGui::GetBackgroundDrawList();
        background->AddRectFilled(topLeft, bottomRight, IM_COL32(19, 29, 25, 255), 5.0f);
        background->AddRect(topLeft, bottomRight, IM_COL32(87, 114, 96, 255), 5.0f);
        for (float x = topLeft.x + 12.0f; x < bottomRight.x; x += 24.0f)
            background->AddLine({x, topLeft.y + 36.0f}, {x, bottomRight.y - 12.0f}, IM_COL32(39, 56, 46, 255));
        for (float y = topLeft.y + 36.0f; y < bottomRight.y - 12.0f; y += 24.0f)
            background->AddLine({topLeft.x + 12.0f, y}, {bottomRight.x - 12.0f, y}, IM_COL32(39, 56, 46, 255));
        background->AddText({topLeft.x + 12.0f, topLeft.y + 10.0f}, IM_COL32(179, 195, 169, 255), "原生小地图保留区");
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
