#include "D3D9OverlayRenderer.h"

#include "OverlayFonts.h"
#include "OverlayCursor.h"
#include "OverlayInputProcessing.h"
#include "OverlayPanelPresence.h"
#include "../ui/ReplayHudScale.h"
#include "../viewer/ReplayViewExchange.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <cmath>
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

struct DrawTarget final
{
    HWND mWindow = nullptr;
    UINT mBackBufferWidth = 0;
    UINT mBackBufferHeight = 0;
    LONG mClientWidth = 0;
    LONG mClientHeight = 0;
    D3DVIEWPORT9 mGameViewport{};
    IDirect3DSwapChain9* mSwapChain = nullptr;
    IDirect3DSurface9* mBackBuffer = nullptr;
    IDirect3DSurface9* mCurrentTarget = nullptr;

    ~DrawTarget()
    {
        if (mCurrentTarget != nullptr) mCurrentTarget->Release();
        if (mBackBuffer != nullptr) mBackBuffer->Release();
        if (mSwapChain != nullptr) mSwapChain->Release();
    }
};

bool QueryDrawTarget(IDirect3DDevice9* device, DrawTarget& target) noexcept
{
    if (device == nullptr)
    {
        return false;
    }
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(device->GetCreationParameters(&creation)) ||
        FAILED(device->GetSwapChain(0, &target.mSwapChain)) || target.mSwapChain == nullptr)
    {
        return false;
    }
    D3DPRESENT_PARAMETERS present{};
    if (FAILED(target.mSwapChain->GetPresentParameters(&present)) ||
        FAILED(target.mSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &target.mBackBuffer)) ||
        target.mBackBuffer == nullptr ||
        FAILED(device->GetRenderTarget(0, &target.mCurrentTarget)) ||
        target.mCurrentTarget == nullptr || target.mCurrentTarget != target.mBackBuffer)
    {
        return false;
    }
    target.mWindow = present.hDeviceWindow != nullptr ? present.hDeviceWindow : creation.hFocusWindow;
    if (target.mWindow == nullptr || !IsWindow(target.mWindow))
    {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(target.mWindow, &processId);
    RECT client{};
    wchar_t className[128]{};
    if (processId != GetCurrentProcessId() ||
        GetClassNameW(target.mWindow, className, static_cast<int>(std::size(className))) <= 0 ||
        !GetClientRect(target.mWindow, &client) || client.right <= client.left ||
        client.bottom <= client.top)
    {
        return false;
    }
    target.mClientWidth = client.right - client.left;
    target.mClientHeight = client.bottom - client.top;

    D3DSURFACE_DESC surface{};
    if (FAILED(target.mBackBuffer->GetDesc(&surface)) ||
        surface.Width == 0 || surface.Height == 0 ||
        FAILED(device->GetViewport(&target.mGameViewport)))
    {
        return false;
    }
    target.mBackBufferWidth = surface.Width;
    target.mBackBufferHeight = surface.Height;
    return true;
}
}

bool D3D9OverlayRenderer::Initialize(IDirect3DDevice9* device, HWND window) noexcept
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
                return mIcons.Resolve(mDevice, image);
            });
        }

        ScopedImGuiContext imguiContext(mImGuiContext);
        if (!ImGui_ImplWin32_Init(window))
        {
            return false;
        }
        if (!ImGui_ImplDX9_Init(device))
        {
            ImGui_ImplWin32_Shutdown();
            return false;
        }
        // DX9 官方后端持有 device 的 COM 引用；mDevice 只记录身份。
        mDevice = device;
        mWindow = window;
        mBackendInitialized = true;
        mDeviceLost = false;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void D3D9OverlayRenderer::ShutdownOnRenderThread() noexcept
{
    if (!mBackendInitialized || mRenderThreadId != GetCurrentThreadId())
    {
        return;
    }
    ScopedImGuiContext imguiContext(mImGuiContext);
    mIcons.Clear();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    mBackendInitialized = false;
    mDevice = nullptr;
    mWindow = nullptr;
    mSessionId = 0;
    mHud.Reset();
    mInputQueue.Clear();
}

void D3D9OverlayRenderer::SetFailure(FailureReason reason) noexcept
{
    mFailureReason.store(reason, std::memory_order_release);
    mInputQueue.Clear();
}

bool D3D9OverlayRenderer::Render(IDirect3DDevice9* device,
                                 viewer::ReplayViewExchange& exchange) noexcept
{
    std::unique_lock lock(mRenderMutex, std::try_to_lock);
    if (!lock.owns_lock() || mFailureReason.load(std::memory_order_acquire) == FailureReason::WrongThread)
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
    if (device == nullptr)
    {
        SetFailure(FailureReason::InvalidDevice);
        return false;
    }
    const DWORD threadId = GetCurrentThreadId();
    if (mRenderThreadId != 0 && mRenderThreadId != threadId)
    {
        SetFailure(FailureReason::WrongThread);
        return false;
    }
    mRenderThreadId = threadId;
    // 原生窗口线程负责 TestCooperativeLevel/Reset，呈现线程只消费 Reset 的结果。
    if (mDeviceLost && device == mDevice)
    {
        SetFailure(FailureReason::DeviceLost);
        return false;
    }

    DrawTarget target;
    if (!QueryDrawTarget(device, target))
    {
        SetFailure(FailureReason::InvalidDevice);
        return false;
    }
    if (mBackendInitialized && (mDevice != device || mWindow != target.mWindow))
    {
        ShutdownOnRenderThread();
    }
    if (!mBackendInitialized && !Initialize(device, target.mWindow))
    {
        SetFailure(FailureReason::Initialization);
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
        ProcessOverlayInput(mInputQueue, target.mWindow, true, mVisible);
        if (!mVisible)
        {
            // F10 隐藏时立即撤回面板，游戏线程在下一次侧栏绘制时交回原生侧栏。
            exchange.WithdrawHudPresence();
            ClearOverlayCursor();
            mInputQueue.PublishPolicy(true, false, false, {});
            return true;
        }

        // 档位按客户区高度解析，与 Win32 后端随后计算的 DisplaySize 同源。
        const int scalePercent = ui::ResolveHudScalePercent(snapshot->mHudScaleSetting,
            static_cast<float>(target.mClientHeight));
        if (scalePercent != mFontScalePercent)
        {
            // 在呈现线程、设备未丢失时释放旧字体纹理与缓冲，再按目标像素重建 CPU 图集；
            // 下面的 NewFrame 发现没有字体纹理，会按新图集重建设备对象。设备丢失期间不会走到这里。
            ImGui_ImplDX9_InvalidateDeviceObjects();
            LoadOverlayFont(ImGui::GetIO(), static_cast<float>(scalePercent) / 100.0f);
            mFontScalePercent = scalePercent;
            mAppliedScalePercent.store(scalePercent, std::memory_order_release);
        }
        ImGui_ImplDX9_NewFrame();
        if (ImGui::GetIO().Fonts->TexID == nullptr)
        {
            // 官方 NewFrame 对设备对象创建失败没有返回值，避免误报 HUD 已绘制。
            // 新图集保留在 CPU 侧，下一帧 NewFrame 会再次尝试创建纹理。
            SetFailure(FailureReason::Initialization);
            return false;
        }
        ImGui_ImplWin32_NewFrame();
        RefreshOverlayMousePosition(target.mWindow);
        mHud.SetScalePercent(scalePercent);
        ImGuiIO& io = ImGui::GetIO();
        const float scaleX = static_cast<float>(target.mBackBufferWidth) /
                             static_cast<float>(target.mClientWidth);
        const float scaleY = static_cast<float>(target.mBackBufferHeight) /
                             static_cast<float>(target.mClientHeight);
        if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0f || scaleY <= 0.0f ||
            io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
        {
            SetFailure(FailureReason::InvalidDevice);
            return false;
        }
        io.DisplayFramebufferScale = ImVec2(scaleX, scaleY);

        // cnc-ddraw D3D9 保持整块视口、用顶点位置做等比留边，所以视口只作保守参考，不能确认图像位置。
        ui::GameImagePlacement gameImage;
        const double viewportRight = static_cast<double>(target.mGameViewport.X) +
                                     target.mGameViewport.Width;
        const double viewportBottom = static_cast<double>(target.mGameViewport.Y) +
                                      target.mGameViewport.Height;
        if (target.mGameViewport.Width > 0 && target.mGameViewport.Height > 0 &&
            viewportRight <= target.mBackBufferWidth &&
            viewportBottom <= target.mBackBufferHeight)
        {
            gameImage.mRect = {
                static_cast<float>(target.mGameViewport.X) / scaleX,
                static_cast<float>(target.mGameViewport.Y) / scaleY,
                static_cast<float>(viewportRight) / scaleX,
                static_cast<float>(viewportBottom) / scaleY};
        }
        mHud.SetGameImagePlacement(gameImage);
        ImGui::NewFrame();
        mHud.Draw(*snapshot, exchange);
        UpdateOverlayCursor(target.mWindow, snapshot->mActive, mVisible, mHud.GetVisibleRegions());
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        mInputQueue.PublishPolicy(true, true, io.WantCaptureKeyboard, mHud.GetInputRegions());
        ReportPanelPresence(exchange, *snapshot, mHud);
        mFailureReason.store(FailureReason::None, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        SetFailure(FailureReason::Initialization);
        return false;
    }
}

void D3D9OverlayRenderer::BeforeReset(IDirect3DDevice9* device) noexcept
{
    std::lock_guard lock(mRenderMutex);
    if (!mBackendInitialized || device != mDevice)
    {
        return;
    }
    // Reset Hook 已与呈现 Hook 全局串行；此边界允许临时切换本 ImGui context，
    // 只释放 DEFAULT 资源，不在 Reset 线程执行 NewFrame 或绘制。
    ScopedImGuiContext imguiContext(mImGuiContext);
    mIcons.Clear();
    ImGui_ImplDX9_InvalidateDeviceObjects();
    mDeviceLost = true;
    mInputQueue.Clear();
}

void D3D9OverlayRenderer::AfterReset(IDirect3DDevice9* device, HRESULT result) noexcept
{
    std::lock_guard lock(mRenderMutex);
    if (device != mDevice)
    {
        return;
    }
    mDeviceLost = FAILED(result);
    mInputQueue.Clear();
}

void D3D9OverlayRenderer::ResetInput() noexcept
{
    mInputQueue.Clear();
}

const char* D3D9OverlayRenderer::GetFailureReason() const noexcept
{
    switch (mFailureReason.load(std::memory_order_acquire))
    {
    case FailureReason::None: return "none";
    case FailureReason::WrongThread: return "DX9 呈现线程变化";
    case FailureReason::InvalidDevice: return "DX9 设备或主交换链不可用";
    case FailureReason::Initialization: return "DX9 ImGui 后端初始化失败";
    case FailureReason::DeviceChanged: return "DX9 设备窗口变化";
    case FailureReason::DeviceLost: return "DX9 设备等待原生重置";
    }
    return "DX9 未知故障";
}

D3D9OverlayRenderer& GetD3D9OverlayRenderer() noexcept
{
    static D3D9OverlayRenderer* renderer = new D3D9OverlayRenderer();
    return *renderer;
}
}
