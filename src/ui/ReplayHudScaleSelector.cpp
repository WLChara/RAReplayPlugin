#include "ReplayHudParts.h"

#include "../viewer/ReplayViewExchange.h"

#include <cfloat>
#include <cstdio>

namespace rareplay::ui::detail
{
namespace
{
constexpr viewer::HudScaleSetting SCALE_OPTIONS[] = {
    viewer::HudScaleSetting::Auto, viewer::HudScaleSetting::Percent100, viewer::HudScaleSetting::Percent125,
    viewer::HudScaleSetting::Percent150, viewer::HudScaleSetting::Percent175, viewer::HudScaleSetting::Percent200};

void RecordWindowRegion(std::vector<UiRect>& regions)
{
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    regions.push_back({position.x, position.y, position.x + size.x, position.y + size.y});
}
}

const char* FormatHudScaleLabel(viewer::HudScaleSetting setting, int effectivePercent, char* buffer, std::size_t size)
{
    if (setting == viewer::HudScaleSetting::Auto)
        std::snprintf(buffer, size, "自动（%d%%）", effectivePercent);
    else
        std::snprintf(buffer, size, "%d%%", static_cast<int>(setting));
    return buffer;
}

void DrawHudScaleOptions(const viewer::ReplayViewSnapshot& snapshot, int effectivePercent,
                         viewer::ReplayViewExchange& exchange)
{
    for (const viewer::HudScaleSetting option : SCALE_OPTIONS)
    {
        char label[48];
        FormatHudScaleLabel(option, effectivePercent, label, sizeof(label));
        const bool isSelected = option == snapshot.mHudScaleSetting;
        ImGui::PushID(static_cast<int>(option));
        // 只提交命令；游戏线程保存设置并发布新快照后，宿主才按新档位重建字体。
        if (ImGui::Selectable(label, isSelected) && !isSelected && snapshot.mActive)
            exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::SetHudScale, static_cast<int>(option)});
        if (isSelected)
            ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
}

void DrawHudScaleCombo(const viewer::ReplayViewSnapshot& snapshot, int effectivePercent,
                       viewer::ReplayViewExchange& exchange, std::vector<UiRect>& inputRegions)
{
    ImGui::TextUnformatted("界面缩放");
    char preview[48];
    FormatHudScaleLabel(snapshot.mHudScaleSetting, effectivePercent, preview, sizeof(preview));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(!snapshot.mActive);
    if (ImGui::BeginCombo("##界面缩放", preview))
    {
        DrawHudScaleOptions(snapshot, effectivePercent, exchange);
        RecordWindowRegion(inputRegions);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    TextWrappedDisabled("保存在本游戏目录，下次回放沿用");
}
}
