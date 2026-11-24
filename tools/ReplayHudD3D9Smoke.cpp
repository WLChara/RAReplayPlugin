#include "ReplayHudDemoData.h"
#include "../src/hooks/graphics/ReplayOverlayHook.h"
#include "../src/render/D3D9OverlayRenderer.h"
#include "../src/render/ReplayOverlayRenderer.h"
#include "../src/viewer/ReplayViewExchange.h"
#include "imgui.h"

#include <Windows.h>
#include <d3d9.h>
#include <gl/GL.h>
#include <float.h>
#include <xmmintrin.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
constexpr D3DCOLOR BACKGROUND = D3DCOLOR_XRGB(8, 12, 10);
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void Check(HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        std::ostringstream error;
        error << message << " HRESULT=0x" << std::hex << static_cast<std::uint32_t>(result);
        throw std::runtime_error(error.str());
    }
}

void SaveAndVerify(IDirect3DDevice9* device, const std::filesystem::path& path)
{
    IDirect3DSurface9* backBuffer = nullptr;
    Check(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer), "GetBackBuffer");
    D3DSURFACE_DESC description{};
    backBuffer->GetDesc(&description);
    IDirect3DSurface9* staging = nullptr;
    Check(device->CreateOffscreenPlainSurface(description.Width, description.Height,
        description.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr), "Readback surface");
    Check(device->GetRenderTargetData(backBuffer, staging), "Readback pixels");
    backBuffer->Release();
    D3DLOCKED_RECT locked{};
    Check(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY), "Readback lock");
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(description.Width) * description.Height);
    std::size_t changed = 0;
    for (UINT y = 0; y < description.Height; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(y) * locked.Pitch);
        for (UINT x = 0; x < description.Width; ++x)
        {
            pixels[static_cast<std::size_t>(y) * description.Width + x] = row[x];
            if ((row[x] & 0xFFFFFF) != (BACKGROUND & 0xFFFFFF))
                ++changed;
        }
    }
    staging->UnlockRect();
    staging->Release();
    Require(changed > 10000, "HUD must actually change backbuffer pixels");
    Require((pixels[20 * description.Width + description.Width - 25] & 0xFFFFFF) ==
        (BACKGROUND & 0xFFFFFF), "Native minimap reserve must remain untouched");

    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(description.Width);
    info.biHeight = -static_cast<LONG>(description.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biSizeImage = static_cast<DWORD>(pixels.size() * sizeof(std::uint32_t));
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()), info.biSizeImage);
    Require(output.good(), "Save DX9 smoke image");
}

// 发布带指定缩放档位的示例快照；宿主在下一帧发现档位变化即重建字体图集与设备对象。
void PublishDemoWithScale(rareplay::viewer::HudScaleSetting scale)
{
    auto snapshot = rareplay::tools::CreateReplayHudDemoSnapshot({});
    snapshot->mHudScaleSetting = scale;
    rareplay::viewer::GetReplayViewExchange().Publish(std::move(snapshot));
}

void DrawPhase(IDirect3DDevice9* device, const std::filesystem::path& capture,
               const std::function<void(int)>& beforeFrame)
{
    for (int frame = 0; frame < 4; ++frame)
    {
        beforeFrame(frame);
        Check(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE), "Prepare render state");
        D3DVIEWPORT9 viewport{};
        Check(device->GetViewport(&viewport), "Read viewport");
        ImGuiContext* previousImGui = ImGui::GetCurrentContext();
        Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, BACKGROUND, 1.0f, 0), "Clear test target");
        Check(device->BeginScene(), "BeginScene");
        Check(device->EndScene(), "Original EndScene after actual Hook callback");
        DWORD zState = 0;
        device->GetRenderState(D3DRS_ZENABLE, &zState);
        D3DVIEWPORT9 restored{};
        device->GetViewport(&restored);
        Require(zState == D3DZB_TRUE, "HUD must restore native D3D render state");
        Require(viewport.X == restored.X && viewport.Y == restored.Y &&
            viewport.Width == restored.Width && viewport.Height == restored.Height,
            "HUD must restore native viewport");
        Require(ImGui::GetCurrentContext() == previousImGui, "HUD must restore caller ImGui context");
        if (frame == 3)
            SaveAndVerify(device, capture);
        Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    }
}

void Wait(std::future<void>& future)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Require(std::chrono::steady_clock::now() < deadline, "DX9 rendering worker timed out");
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    future.get();
}

int RunOpenGlHookSmoke()
{
    HWND window = CreateWindowExW(0, L"STATIC", L"RAReplayPlugin.OpenGlSmoke", WS_POPUP,
        0, 0, 1024, 768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "Create hidden GL hook test window");
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    const int index = ChoosePixelFormat(dc, &format);
    Require(index > 0 && SetPixelFormat(dc, index, &format), "Set GL pixel format");
    HGLRC context = wglCreateContext(dc);
    Require(context && wglMakeCurrent(dc, context), "Bind GL source context");
    Require(rareplay::hooks::EnsureReplayOverlayStarted(), "Arm both graphics APIs");
    PublishDemoWithScale(rareplay::viewer::HudScaleSetting::Percent100);
    auto& renderer = rareplay::render::GetReplayOverlayRenderer();
    for (int frame = 0; frame < 4; ++frame)
    {
        // 第二帧后切到 175%，第三帧后切回自动（客户区 768 高解析为 100%），验证两次重建都能继续绘制。
        if (frame == 2)
        {
            Require(renderer.GetAppliedHudScalePercent() == 100, "GL HUD starts at 100% font atlas");
            PublishDemoWithScale(rareplay::viewer::HudScaleSetting::Percent175);
        }
        else if (frame == 3)
        {
            Require(renderer.GetAppliedHudScalePercent() == 175, "GL HUD rebuilt the font atlas at 175%");
            PublishDemoWithScale(rareplay::viewer::HudScaleSetting::Auto);
        }
        glViewport(0, 0, 1024, 768);
        glClearColor(0.03f, 0.04f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        Require(SwapBuffers(dc), "Actual hooked GL swap");
        Require(wglGetCurrentContext() == context && wglGetCurrentDC() == dc,
            "GL Hook must restore host context");
        Require(ImGui::GetCurrentContext() == nullptr, "GL Hook must restore ImGui context");
    }
    const auto info = rareplay::hooks::GetReplayOverlayDiagnostics();
    Require(info.mObservedBackend == rareplay::hooks::ReplayGraphicsBackend::OpenGL &&
        info.mOpenGlContextFrames == 4 && info.mRenderedFrames == 4 && info.mD3D9EndSceneCalls == 0,
        "Only actual GL callbacks select GL even with d3d9.dll loaded");
    Require(renderer.GetAppliedHudScalePercent() == 100, "GL HUD restored the automatic 100% atlas");
    rareplay::viewer::GetReplayViewExchange().Clear();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    std::cout << "OpenGL actual=OpenGL swaps=" << info.mOpenGlSwapCalls
        << " contextFrames=" << info.mOpenGlContextFrames << " drawn=" << info.mRenderedFrames
        << "; loaded DX9 module did not choose backend; HUD scale 100->175->auto(100) passed\n";
    return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc > 1 && wcscmp(argv[1], L"--opengl-hooks") == 0)
            return RunOpenGlHookSmoke();
        const std::filesystem::path output = argc > 1 ? argv[1] : L"dx9-smoke.bmp";
        HWND window = CreateWindowExW(0, L"STATIC", L"RAReplayPlugin.D3D9Smoke", WS_POPUP,
            0, 0, 1024, 768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "Create hidden smoke window");
        IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
        Require(api != nullptr, "Direct3DCreate9");
        D3DPRESENT_PARAMETERS parameters{};
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = window;
        parameters.BackBufferWidth = 1600;
        parameters.BackBufferHeight = 900;
        parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9* device = nullptr;
        Check(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
            &parameters, &device), "Create hidden hardware device");

        unsigned int controlBefore = 0, controlAfter = 0;
        _controlfp_s(&controlBefore, 0, 0);
        const unsigned int mxcsr = _mm_getcsr();
        Require(rareplay::hooks::EnsureReplayOverlayStarted(), "Arm graphics hooks");
        _controlfp_s(&controlAfter, 0, 0);
        Require(controlBefore == controlAfter && mxcsr == _mm_getcsr(), "Hook probe must preserve game FPU controls");
        PublishDemoWithScale(rareplay::viewer::HudScaleSetting::Percent100);

        std::promise<void> phaseOne, continueAfterReset, phaseTwo;
        auto first = phaseOne.get_future();
        auto resume = continueAfterReset.get_future();
        auto second = phaseTwo.get_future();
        std::thread renderThread([&]
        {
            bool firstFinished = false;
            try
            {
                auto& renderer = rareplay::render::GetD3D9OverlayRenderer();
                // 第一阶段：100% 绘制两帧后切到 200%，最后一帧截图要求切档后仍正常绘制。
                DrawPhase(device, output, [&renderer](int frame)
                {
                    if (frame == 2)
                    {
                        Require(renderer.GetAppliedHudScalePercent() == 100, "DX9 HUD starts at 100% font atlas");
                        PublishDemoWithScale(rareplay::viewer::HudScaleSetting::Percent200);
                    }
                });
                Require(renderer.GetAppliedHudScalePercent() == 200, "DX9 HUD rebuilt the font atlas at 200%");
                phaseOne.set_value();
                firstFinished = true;
                resume.get();
                // 设备重置后切回自动：客户区 1024×768 应解析为 100%，并在重建的设备上恢复绘制。
                DrawPhase(device, output.parent_path() / L"dx9-after-reset.bmp", [](int frame)
                {
                    if (frame == 0)
                        PublishDemoWithScale(rareplay::viewer::HudScaleSetting::Auto);
                });
                Require(renderer.GetAppliedHudScalePercent() == 100, "DX9 HUD restored the automatic 100% atlas");
                phaseTwo.set_value();
            }
            catch (...)
            {
                if (!firstFinished) phaseOne.set_exception(std::current_exception());
                else phaseTwo.set_exception(std::current_exception());
            }
        });
        try
        {
            Wait(first);
            parameters.BackBufferWidth = 1280;
            parameters.BackBufferHeight = 960;
            // Reset 在创建设备的主线程，呈现在另一线程；复现 cnc-ddraw 生命周期边界。
            Check(device->Reset(&parameters), "Reset must not retain HUD default-pool resources");
            continueAfterReset.set_value();
            Wait(second);
            renderThread.join();
        }
        catch (...)
        {
            try { continueAfterReset.set_exception(std::current_exception()); } catch (...) {}
            renderThread.join();
            throw;
        }
        const auto diagnostics = rareplay::hooks::GetReplayOverlayDiagnostics();
        Require(diagnostics.mObservedBackend == rareplay::hooks::ReplayGraphicsBackend::Direct3D9 &&
            diagnostics.mRenderedFrames >= 8 && diagnostics.mD3D9PresentCalls >= 8,
            "Actual DX9 calls and HUD draws must be observed");
        rareplay::viewer::GetReplayViewExchange().Clear();
        std::cout << "DX9 actual=Direct3D9 EndScene=" << diagnostics.mD3D9EndSceneCalls
            << " Present=" << diagnostics.mD3D9PresentCalls << " drawn=" << diagnostics.mRenderedFrames
            << " failed=" << diagnostics.mRenderFailures
            << "; cross-thread Reset, scaled backbuffer and HUD scale 100->200->auto(100) passed\n";
        device->Release();
        api->Release();
        DestroyWindow(window);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
