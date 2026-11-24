#pragma once

#include "../ui/ReplayHud.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace rareplay::render
{
/**
 * WndProc 线程只复制 Win32 输入消息；ImGui 后端在 OpenGL 渲染线程消费消息。
 * 输入策略是渲染线程发布的不可变上一帧快照。
 */
class OverlayInputQueue final
{
public:
    static constexpr std::size_t CAPACITY = 256;

    void PublishPolicy(bool playbackActive, bool overlayVisible, bool wantsKeyboard,
                       const std::vector<ui::UiRect>& inputRegions) noexcept;

    /**
     * 入队并根据上一帧 UI 区域决定是否让游戏收到该消息；返回 true 表示消息被吞掉。
     * 回放激活时，按住 Ctrl 的 WM_MOUSEWHEEL 若不在 HUD 输入区域内（F10 隐藏时没有区域），既不交给游戏也不进 ImGui 队列，
     * 而是按 WHEEL_DELTA 累积为战局缩放档数（向前滚放大），由 TakeBattlefieldZoomSteps 取出。
     * 指针在 HUD 区域内、没按 Ctrl 或回放未激活时，滚轮保持原有路由。只能在窗口线程调用。
     */
    bool RouteMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    /**
     * 取出并清零 Ctrl+滚轮累积出的整档数（正数放大）。窗口线程在每次 RouteMessage 之后调用，
     * 以当前快照的会话 ID 提交观看命令；不足一档的余量留在队列里，供高精度滚轮继续累积。
     */
    int TakeBattlefieldZoomSteps() noexcept;
    void RequestFocusReset() noexcept;

    std::vector<MSG> Drain() noexcept;
    bool ConsumeFocusReset() noexcept;
    void Clear() noexcept;

private:
    struct CapturePolicy
    {
        bool mPlaybackActive = false;
        bool mOverlayVisible = false;
        bool mWantsKeyboard = false;
        std::vector<ui::UiRect> mInputRegions;
    };

    static bool IsMouseMessage(UINT message) noexcept;
    static bool IsKeyboardMessage(UINT message) noexcept;
    static bool IsF10Message(UINT message, WPARAM wParam) noexcept;
    static bool IsInsideRegion(const CapturePolicy& policy, const POINT& point) noexcept;
    static bool IsBattlefieldZoomWheel(const CapturePolicy& policy, HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam) noexcept;
    void AccumulateBattlefieldZoomWheel(int wheelDelta) noexcept;
    static bool IsButtonDown(UINT message) noexcept;
    static bool IsButtonUp(UINT message) noexcept;
    static std::uint8_t ButtonMask(UINT message, WPARAM wParam) noexcept;

    void Enqueue(const MSG& message) noexcept;

    std::atomic<std::shared_ptr<const CapturePolicy>> mPolicy;
    std::mutex mQueueMutex;
    std::deque<MSG> mMessages;
    bool mFocusResetRequested = false;
    std::atomic<bool> mClearCapturedButtonsRequested{false};
    std::uint8_t mCapturedButtons = 0;
    std::uint8_t mGameButtons = 0;
    bool mOwnsWindowCapture = false;
    // Ctrl+滚轮不足一档的余量（|值| < WHEEL_DELTA），只在窗口线程读写；回放未激活、失去焦点或 Clear 之后归零。
    int mZoomWheelRemainder = 0;
    // Clear 可能在其他线程调用：只置此标志，由窗口线程在下一条消息开始时清零余量。
    std::atomic<bool> mZoomWheelResetRequested{false};
    // 已累积、尚未取出的整档数；Clear 在其他线程直接清零，所以用原子量。
    std::atomic<int> mPendingZoomSteps{0};
};
}
