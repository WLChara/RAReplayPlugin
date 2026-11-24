#include "ReplayHudZoomModel.h"

namespace rareplay::ui
{
ZoomControlModel BuildZoomControlModel(const viewer::BattlefieldZoomView& view, bool canInteract)
{
    ZoomControlModel model;
    if (!view.mIsAvailable)
    {
        if (view.mUnavailableReason.empty())
            return model;
        model.mMode = ZoomControlMode::Unavailable;
        model.mLabel = "缩放不可用：" + view.mUnavailableReason;
        model.mCompactLabel = "缩放不可用";
        return model;
    }
    model.mMode = ZoomControlMode::Controls;
    model.mCompactLabel = std::to_string(view.mPercent) + "%";
    if (view.mHasPendingChange)
        model.mCompactLabel += "…";
    model.mLabel = "缩放 " + model.mCompactLabel;
    model.mCanZoomIn = canInteract && view.mCanZoomIn;
    model.mCanZoomOut = canInteract && view.mCanZoomOut;
    model.mCanReset = canInteract && (view.mIsActive || view.mHasPendingChange);
    return model;
}
}
