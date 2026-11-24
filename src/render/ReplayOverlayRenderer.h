#pragma once

#include "IconTextureCache.h"
#include "OverlayInputQueue.h"
#include "WglOverlayContext.h"
#include "../ui/ReplayHud.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

struct ImGuiContext;

namespace rareplay::viewer
{
class ReplayViewExchange;
}

namespace rareplay::render
{
/**
 * 仅由 SwapBuffers 呈现线程持有 ImGui 与专用 WGL 上下文。
 * WndProc 线程只能使用 GetInputQueue() 返回的线程安全队列。
 */
class ReplayOverlayRenderer final
{
public:
    bool Render(HDC drawDc, viewer::ReplayViewExchange& exchange) noexcept;
    OverlayInputQueue& GetInputQueue() noexcept { return mInputQueue; }
    void ResetInput() noexcept;
    bool IsDisabled() const noexcept { return mDisabled.load(std::memory_order_acquire); }
    /** 诊断用：字体图集当前对应的 HUD 缩放百分比，尚未装载时为 0。任意线程可读。 */
    int GetAppliedHudScalePercent() const noexcept { return mAppliedScalePercent.load(std::memory_order_acquire); }

private:
    bool InitializeImGui(HWND window) noexcept;
    bool TearDownOnCurrentContext() noexcept;
    void ProcessInput(HWND window, bool playbackActive) noexcept;
    void Disable() noexcept;

    std::mutex mRenderMutex;
    OverlayInputQueue mInputQueue;
    WglOverlayContext mWglContext;
    IconTextureCache mIcons;
    ui::ReplayHud mHud;
    ImGuiContext* mImGuiContext = nullptr;
    HWND mWindow = nullptr;
    HGLRC mSourceContext = nullptr;
    int mPixelFormat = 0;
    DWORD mRenderThreadId = 0;
    std::uint64_t mSessionId = 0;
    bool mBackendInitialized = false;
    bool mVisible = true;
    bool mHasShownEnded = false;
    // 渲染线程独占；图集在后端重建或换上下文时保留，只在档位变化时重建。
    int mFontScalePercent = 0;
    std::atomic<int> mAppliedScalePercent{0};
    std::atomic<bool> mDisabled{false};
};

/** 故意保持到进程结束，避免 DLL 卸载时在未知 GL 线程上析构纹理。 */
ReplayOverlayRenderer& GetReplayOverlayRenderer() noexcept;
}
