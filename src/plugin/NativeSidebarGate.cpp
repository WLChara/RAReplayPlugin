#include "NativeSidebarGate.h"

namespace rareplay::plugin
{
bool ShouldSkipNativeSidebarChrome(const NativeSidebarGateInput& input) noexcept
{
    if (!input.mIsPlaybackActive || !input.mIsHudVisible || !input.mPanelPresentedAt ||
        !input.mIsSidebarOnRight || input.mIsArmageddonMode)
        return false;
    // 渲染线程的时刻可能略晚于游戏线程读取的 now，负的间隔按刚呈现处理。
    return input.mNow - *input.mPanelPresentedAt <= PANEL_PRESENCE_TIMEOUT;
}

NativeSidebarAction NativeSidebarGate::Update(const NativeSidebarGateInput& input) noexcept
{
    const bool shouldSkip = ShouldSkipNativeSidebarChrome(input);
    const bool wasSkipping = mIsSkipping;
    mIsSkipping = shouldSkip;
    if (shouldSkip)
        return NativeSidebarAction::SkipChrome;
    return wasSkipping ? NativeSidebarAction::RedrawNative : NativeSidebarAction::DrawNative;
}
}
