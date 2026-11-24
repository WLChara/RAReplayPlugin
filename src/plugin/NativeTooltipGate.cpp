#include "NativeTooltipGate.h"

namespace rareplay::plugin
{
std::optional<bool> NativeTooltipGate::Update(bool shouldSuppress, bool currentIsActive) noexcept
{
    if (shouldSuppress)
    {
        if (!mIsSuppressing)
        {
            mIsSuppressing = true;
            mSavedIsActive = currentIsActive;
        }
        else if (currentIsActive)
        {
            // 插件一直保持关闭，此时的启用只能来自原生的再次设置，恢复时沿用这个新值。
            mSavedIsActive = true;
        }
        return currentIsActive ? std::optional<bool>(false) : std::nullopt;
    }
    if (!mIsSuppressing)
        return std::nullopt;
    mIsSuppressing = false;
    return currentIsActive != mSavedIsActive ? std::optional<bool>(mSavedIsActive) : std::nullopt;
}
}
