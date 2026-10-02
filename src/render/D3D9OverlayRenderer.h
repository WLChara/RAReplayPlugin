#pragma once

#include "D3D9IconTextureCache.h"
#include "OverlayInputQueue.h"
#include "../ui/ReplayHud.h"

#include <Windows.h>
#include <d3d9.h>

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
/** 在游戏有效 EndScene 内绘制 HUD；不执行 BeginScene、EndScene、Clear 或 Present。 */
class D3D9OverlayRenderer final
{
public:
    bool Render(IDirect3DDevice9* device, viewer::ReplayViewExchange& exchange) noexcept;
    void BeforeReset(IDirect3DDevice9* device) noexcept;
    void AfterReset(IDirect3DDevice9* device, HRESULT result) noexcept;
    OverlayInputQueue& GetInputQueue() noexcept { return mInputQueue; }
    void ResetInput() noexcept;
    const char* GetFailureReason() const noexcept;

private:
    enum class FailureReason { None, WrongThread, InvalidDevice, Initialization, DeviceChanged, DeviceLost };

    bool Initialize(IDirect3DDevice9* device, HWND window) noexcept;
    void ShutdownOnRenderThread() noexcept;
    void SetFailure(FailureReason reason) noexcept;

    std::mutex mRenderMutex;
    OverlayInputQueue mInputQueue;
    D3D9IconTextureCache mIcons;
    ui::ReplayHud mHud;
    ImGuiContext* mImGuiContext = nullptr;
    IDirect3DDevice9* mDevice = nullptr;
    HWND mWindow = nullptr;
    DWORD mRenderThreadId = 0;
    std::uint64_t mSessionId = 0;
    std::atomic<FailureReason> mFailureReason{FailureReason::None};
    bool mBackendInitialized = false;
    bool mVisible = true;
    bool mHasShownEnded = false;
    bool mDeviceLost = false;
};

/** 进程期稳定实例，避免未知线程的 DLL 卸载阶段触发 COM/ImGui 析构。 */
D3D9OverlayRenderer& GetD3D9OverlayRenderer() noexcept;
}
