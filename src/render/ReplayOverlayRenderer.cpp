#include "ReplayOverlayRenderer.h"

#include "OverlayFonts.h"
#include "OverlayCursor.h"
#include "OverlayInputProcessing.h"
#include "OverlayPanelPresence.h"
#include "../ui/ReplayHudScale.h"
#include "../viewer/ReplayViewExchange.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#include <cstdio>
#include <iterator>
#include <memory>
#include <utility>

namespace rareplay::render
{
namespace
{
class ScopedImGuiContext final
{
public:
    explicit ScopedImGuiContext(ImGuiContext* current) noexcept
        : mPrevious(ImGui::GetCurrentContext())
    {
        ImGui::SetCurrentContext(current);
    }
    ~ScopedImGuiContext() { ImGui::SetCurrentContext(mPrevious); }

private:
    ImGuiContext* mPrevious;
};

bool IsValidDrawTarget(HDC drawDc, HWND& outWindow, HGLRC& outContext, int& outPixelFormat) noexcept
{
    outWindow = nullptr;
    outContext = nullptr;
    outPixelFormat = 0;
    if (drawDc == nullptr || wglGetCurrentDC() != drawDc)
    {
        return false;
    }
    outContext = wglGetCurrentContext();
    outWindow = WindowFromDC(drawDc);
    if (outContext == nullptr || outWindow == nullptr || !IsWindow(outWindow))
    {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(outWindow, &processId);
    if (processId != GetCurrentProcessId())
    {
        return false;
    }
    wchar_t className[128]{};
    RECT client{};
    if (GetClassNameW(outWindow, className, static_cast<int>(std::size(className))) <= 0 ||
        !GetClientRect(outWindow, &client) || client.right <= client.left ||
        client.bottom <= client.top)
    {
        return false;
    }
    outPixelFormat = GetPixelFormat(drawDc);
    return outPixelFormat != 0;
}

}

bool ReplayOverlayRenderer::InitializeImGui(HWND window) noexcept
{
    try
    {
        if (mImGuiContext == nullptr)
        {
            ImGuiContext* previous = ImGui::GetCurrentContext();
            mImGuiContext = ImGui::CreateContext();
            ImGui::SetCurrentContext(mImGuiContext);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            // 字体在首次 Render 按 HUD 缩放装载；mFontScalePercent 为 0 表示尚未装载。
            ImGui::SetCurrentContext(previous);
            mHud.SetIconResolver([this](const std::shared_ptr<const viewer::IconImage>& image) {
                return mIcons.Resolve(image);
            });
        }

        ScopedImGuiContext imguiContext(mImGuiContext);
        if (!ImGui_ImplWin32_InitForOpenGL(window))
        {
            return false;
        }

        int major = 0;
        int minor = 0;
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if (version == nullptr || std::sscanf(version, "%d.%d", &major, &minor) != 2 || major < 2)
        {
            ImGui_ImplWin32_Shutdown();
            OutputDebugStringW(L"RAReplayPlugin: OpenGL 版本不足，HUD 已停用。\n");
            return false;
        }
        const char* shaderVersion = major >= 3 ? "#version 130" : "#version 120";
        if (!ImGui_ImplOpenGL3_Init(shaderVersion))
        {
            ImGui_ImplWin32_Shutdown();
            return false;
        }
        mBackendInitialized = true;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ReplayOverlayRenderer::TearDownOnCurrentContext() noexcept
{
    if (mWglContext.Get() == nullptr)
    {
        return true;
    }
    if (wglGetCurrentContext() != mWglContext.Get())
    {
        return false;
    }
    try
    {
        ScopedImGuiContext imguiContext(mImGuiContext);
        mIcons.Clear();
        if (mBackendInitialized)
        {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplWin32_Shutdown();
            mBackendInitialized = false;
        }
        mWglContext.Reset();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void ReplayOverlayRenderer::ProcessInput(HWND window, bool playbackActive) noexcept
{
    ProcessOverlayInput(mInputQueue, window, playbackActive, mVisible);
}

void ReplayOverlayRenderer::Disable() noexcept
{
    mDisabled.store(true, std::memory_order_release);
    mInputQueue.Clear();
    OutputDebugStringW(L"RAReplayPlugin: HUD 渲染上下文不可安全使用，覆盖层已停用。\n");
}

bool ReplayOverlayRenderer::Render(HDC drawDc, viewer::ReplayViewExchange& exchange) noexcept
{
    std::unique_lock lock(mRenderMutex, std::try_to_lock);
    if (!lock.owns_lock() || mDisabled.load(std::memory_order_acquire))
    {
        return false;
    }
    const auto snapshot = exchange.Read();
    if (!snapshot || !snapshot->mActive)
    {
        if (mImGuiContext != nullptr && mRenderThreadId == GetCurrentThreadId())
        {
            ScopedImGuiContext imguiContext(mImGuiContext);
            ClearOverlayCursor();
        }
        mInputQueue.Clear();
        return false;
    }
    HWND window = nullptr;
    HGLRC sourceContext = nullptr;
    int pixelFormat = 0;
    if (!IsValidDrawTarget(drawDc, window, sourceContext, pixelFormat))
    {
        return false;
    }
    GLint sourceViewport[4]{};
    // 必须在切到 HUD 专用 RC 前读取游戏当前视口，之后交给 UI 做客户区坐标比较。
    glGetIntegerv(GL_VIEWPORT, sourceViewport);
    const DWORD threadId = GetCurrentThreadId();
    if (mRenderThreadId != 0 && mRenderThreadId != threadId)
    {
        // 呈现线程被宿主重建后，不能继续使用旧线程的 UI 状态或保留其输入捕获。
        Disable();
        return false;
    }
    mRenderThreadId = threadId;

    if (mWglContext.Get() != nullptr &&
        (mWindow != window || mPixelFormat != pixelFormat))
    {
        Disable();
        return false;
    }
    if (mWglContext.Get() != nullptr && mSourceContext != sourceContext)
    {
        ScopedWglContext oldContext(drawDc, mWglContext.Get());
        if (!oldContext.IsBound() || !TearDownOnCurrentContext())
        {
            Disable();
            return false;
        }
    }

    if (!mWglContext.Ensure(window, drawDc, sourceContext))
    {
        return false;
    }
    ScopedWglContext overlayContext(drawDc, mWglContext.Get());
    if (!overlayContext.IsBound())
    {
        Disable();
        return false;
    }
    mWindow = window;
    mSourceContext = sourceContext;
    mPixelFormat = pixelFormat;

    if (!mBackendInitialized && !InitializeImGui(window))
    {
        Disable();
        return false;
    }

    try
    {
        ScopedImGuiContext imguiContext(mImGuiContext);
        if (mSessionId != snapshot->mSessionId)
        {
            mSessionId = snapshot->mSessionId;
            mIcons.Clear();
            mHud.Reset();
            mVisible = true;
            mHasShownEnded = false;
        }
        if (snapshot->mPlaybackControls.mStatus == viewer::PlaybackStatus::Ended && !mHasShownEnded)
        {
            mVisible = true;
            mHasShownEnded = true;
        }
        ProcessInput(window, true);
        if (!mVisible)
        {
            // F10 隐藏时立即撤回面板，游戏线程在下一次侧栏绘制时交回原生侧栏。
            exchange.WithdrawHudPresence();
            ClearOverlayCursor();
            mInputQueue.PublishPolicy(true, false, false, {});
            return true;
        }

        // 档位按客户区高度解析，与 Win32 后端随后计算的 DisplaySize 同源。
        RECT client{};
        GetClientRect(window, &client);
        const int scalePercent = ui::ResolveHudScalePercent(snapshot->mHudScaleSetting,
            static_cast<float>(client.bottom - client.top));
        if (scalePercent != mFontScalePercent)
        {
            // 专用 RC 已绑定：先删除旧着色器、缓冲与字体纹理，再按目标像素重建 CPU 图集；
            // 下面的 NewFrame 发现没有着色器，会按新图集重建全部设备对象。
            ImGui_ImplOpenGL3_DestroyDeviceObjects();
            LoadOverlayFont(ImGui::GetIO(), static_cast<float>(scalePercent) / 100.0f);
            mFontScalePercent = scalePercent;
            mAppliedScalePercent.store(scalePercent, std::memory_order_release);
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        RefreshOverlayMousePosition(window);
        mHud.SetScalePercent(scalePercent);
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        ui::GameImagePlacement gameImage;
        const double viewportRight = static_cast<double>(sourceViewport[0]) + sourceViewport[2];
        const double viewportTopInGl = static_cast<double>(sourceViewport[1]) + sourceViewport[3];
        if (sourceViewport[0] >= 0 && sourceViewport[1] >= 0 &&
            sourceViewport[2] > 0 && sourceViewport[3] > 0 &&
            viewportRight <= displaySize.x && viewportTopInGl <= displaySize.y)
        {
            gameImage.mRect = {
                static_cast<float>(sourceViewport[0]),
                displaySize.y - static_cast<float>(viewportTopInGl),
                static_cast<float>(viewportRight),
                displaySize.y - static_cast<float>(sourceViewport[1])};
            // cnc-ddraw OpenGL 以最终 glViewport 放置游戏图像：maintas=false 为整块客户区，
            // maintas/boxing 时为留边后的图像区域，因此此视口可作为图像位置的依据。
            gameImage.mIsImageRect = true;
        }
        mHud.SetGameImagePlacement(gameImage);
        ImGui::NewFrame();
        mHud.Draw(*snapshot, exchange);
        UpdateOverlayCursor(window, snapshot->mActive, mVisible, mHud.GetVisibleRegions());
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        mInputQueue.PublishPolicy(true, true, ImGui::GetIO().WantCaptureKeyboard,
                                  mHud.GetInputRegions());
        ReportPanelPresence(exchange, *snapshot, mHud);
        return true;
    }
    catch (...)
    {
        Disable();
        return false;
    }
}

void ReplayOverlayRenderer::ResetInput() noexcept
{
    mInputQueue.Clear();
}

ReplayOverlayRenderer& GetReplayOverlayRenderer() noexcept
{
    static ReplayOverlayRenderer* renderer = new ReplayOverlayRenderer();
    return *renderer;
}
}
