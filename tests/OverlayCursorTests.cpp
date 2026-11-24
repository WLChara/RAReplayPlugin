#include "../src/render/OverlayCursor.h"
#include "../src/ui/ReplayHud.h"
#include "../src/viewer/ReplayViewExchange.h"

#include "imgui.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool value, const char* message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

bool Contains(const std::vector<rareplay::ui::UiRect>& regions, float x, float y)
{
    for (const auto& region : regions)
    {
        if (x >= region.mLeft && x < region.mRight &&
            y >= region.mTop && y < region.mBottom)
        {
            return true;
        }
    }
    return false;
}

void CheckCursorPolicy()
{
    using rareplay::render::ShouldDrawOverlayCursor;
    const std::vector<rareplay::ui::UiRect> regions = {
        {10.0f, 10.0f, 110.0f, 80.0f},
        {200.0f, 80.0f, 250.0f, 200.0f}};
    Require(ShouldDrawOverlayCursor(true, true, true, true, 15.0f, 20.0f, regions),
            "Visible panel draws software cursor");
    Require(ShouldDrawOverlayCursor(true, true, true, true, 225.0f, 90.0f, regions),
            "Popup outside panel also draws software cursor");
    Require(!ShouldDrawOverlayCursor(true, true, true, true, 110.0f, 20.0f, regions),
            "Right boundary is outside the visible region");
    Require(!ShouldDrawOverlayCursor(true, true, true, true, 150.0f, 90.0f, regions),
            "Map gap keeps native cursor");
    Require(!ShouldDrawOverlayCursor(false, true, true, true, 15.0f, 20.0f, regions),
            "Inactive replay clears software cursor");
    Require(!ShouldDrawOverlayCursor(true, false, true, true, 15.0f, 20.0f, regions),
            "Hidden overlay clears software cursor");
    Require(!ShouldDrawOverlayCursor(true, true, false, true, 15.0f, 20.0f, regions),
            "Focus loss clears software cursor");
    Require(!ShouldDrawOverlayCursor(true, true, true, false, 15.0f, 20.0f, regions),
            "Missing mouse position cannot reuse a stale cursor");
    Require(!ShouldDrawOverlayCursor(true, true, true, true,
                                     std::numeric_limits<float>::quiet_NaN(), 20.0f, regions),
            "Invalid mouse coordinate is rejected");
    Require(!ShouldDrawOverlayCursor(true, true, true, true, 15.0f, 20.0f, {}),
            "No visible HUD leaves native cursor in place");
}

void CheckReadOnlyFpsCard()
{
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1024.0f, 768.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();

    rareplay::viewer::ReplayViewSnapshot snapshot;
    snapshot.mSessionId = 1;
    snapshot.mActive = true;
    snapshot.mNativeUiLayout = rareplay::viewer::NativeUiLayout{1024, 768, {856, 158, 168, 610}};
    rareplay::viewer::ReplayViewExchange exchange;
    rareplay::ui::ReplayHud hud;
    ImGui::NewFrame();
    hud.Draw(snapshot, exchange);
    ImGui::Render();

    const auto& visible = hud.GetVisibleRegions();
    const auto& input = hud.GetInputRegions();
    Require(Contains(visible, 20.0f, 20.0f), "FPS card is visibly covered by HUD");
    Require(!Contains(input, 20.0f, 20.0f), "Read-only FPS card does not capture input");
    Require(rareplay::render::ShouldDrawOverlayCursor(true, true, true, true,
                                                       20.0f, 20.0f, visible),
            "FPS card still receives a foreground software cursor");
    Require(!Contains(visible, 400.0f, 400.0f), "Map center keeps native cursor");
    hud.Reset();
    Require(hud.GetVisibleRegions().empty() && hud.GetInputRegions().empty(),
            "Reset clears both visible and interactive regions");
    ImGui::DestroyContext(context);
}
}

int main()
{
    try
    {
        CheckCursorPolicy();
        CheckReadOnlyFpsCard();
        std::cout << "Overlay cursor tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
