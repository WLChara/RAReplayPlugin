#include "OverlayPanelPresence.h"

#include "../viewer/ReplayViewExchange.h"

#include <chrono>

namespace rareplay::render
{
void ReportPanelPresence(viewer::ReplayViewExchange& exchange, const viewer::ReplayViewSnapshot& snapshot,
                         const ui::ReplayHud& hud) noexcept
{
    if (snapshot.mActive)
        exchange.ReportHudPresented(snapshot.mSessionId, std::chrono::steady_clock::now(),
                                    hud.IsSidebarPanelCovering());
    else
        exchange.WithdrawHudPresence();
}
}
