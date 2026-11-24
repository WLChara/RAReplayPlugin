#include "ReplayHudParts.h"

#include "ReplayHudEconomyChart.h"
#include "../viewer/ReplayViewExchange.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rareplay::ui::detail
{
namespace
{
constexpr ImU32 GENERIC_FILL = IM_COL32(22, 30, 27, 255);
constexpr ImU32 GENERIC_GLYPH = IM_COL32(224, 213, 178, 255);
constexpr ImU32 LABEL_COLOR = IM_COL32(236, 232, 214, 255);
constexpr ImU32 LABEL_SHADOW = IM_COL32(0, 0, 0, 255);
constexpr int ELLIPSE_SEGMENTS = 28;

// 圆角矩形框放不下时（图标很小）圆角取一个较小值。
float GetFrameRounding(float scale)
{
    return std::max(2.0f, std::round(3.0f * scale));
}

// 椭圆折线；ImGui 1.89 没有 AddEllipse。
std::vector<ImVec2> MakeEllipse(const ImVec2& center, float radiusX, float radiusY)
{
    std::vector<ImVec2> points;
    points.reserve(ELLIPSE_SEGMENTS);
    for (int index = 0; index < ELLIPSE_SEGMENTS; ++index)
    {
        const float angle = 6.2831853f * static_cast<float>(index) / static_cast<float>(ELLIPSE_SEGMENTS);
        points.emplace_back(center.x + std::cos(angle) * radiusX, center.y + std::sin(angle) * radiusY);
    }
    return points;
}

// 图标下方的玩家颜色底标：椭圆底座与细圆环，一半露在图标下沿之外，区分是谁释放的超武。
void DrawOwnerBase(ImDrawList* drawList, const ImVec2& iconMin, const ImVec2& iconMax, ImU32 ownerColor,
                   float opacity, float scale)
{
    const float width = iconMax.x - iconMin.x;
    const ImVec2 center((iconMin.x + iconMax.x) * 0.5f, iconMax.y - std::round(2.0f * scale));
    const float radiusX = width * 0.62f;
    const float radiusY = radiusX * 0.42f;
    const std::vector<ImVec2> ring = MakeEllipse(center, radiusX, radiusY);
    drawList->AddConvexPolyFilled(ring.data(), static_cast<int>(ring.size()), WithOpacity(ownerColor, opacity * 0.45f));
    drawList->AddPolyline(ring.data(), static_cast<int>(ring.size()), WithOpacity(ownerColor, std::min(1.0f, opacity * 1.5f)),
                          ImDrawFlags_Closed, std::max(1.5f, std::round(2.0f * scale)));
}

// 没有图标：与 cameo 同比例的深色圆角框、玩家颜色边框与通用标记，下方写超武名称。
void DrawGenericMark(ImDrawList* drawList, const MarkPlacement& placement, ImU32 ownerColor, float scale)
{
    const ImVec2 min(placement.mIconRect.mLeft, placement.mIconRect.mTop);
    const ImVec2 max(placement.mIconRect.mRight, placement.mIconRect.mBottom);
    const float rounding = GetFrameRounding(scale);
    const float opacity = placement.mOpacity;
    drawList->AddRectFilled(min, max, WithOpacity(GENERIC_FILL, std::min(1.0f, opacity * 1.6f)), rounding);
    drawList->AddRect(min, max, WithOpacity(ownerColor, std::min(1.0f, opacity * 1.5f)), rounding, ImDrawFlags_None,
                      std::max(1.0f, std::round(2.0f * scale)));
    DrawSuperWeaponGlyph(drawList, ImVec2(min.x + (max.x - min.x) * 0.2f, min.y + (max.y - min.y) * 0.15f),
                         ImVec2(max.x - (max.x - min.x) * 0.2f, max.y - (max.y - min.y) * 0.15f),
                         WithOpacity(GENERIC_GLYPH, std::min(1.0f, opacity * 1.5f)), std::max(1.5f, std::round(2.0f * scale)));
    // 名称居中写在图标框下方；加一层黑色阴影，在任何地形上都能读出。
    const std::string& label = placement.mMark->mInfo->mLabel;
    if (label.empty())
        return;
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    const float textX = std::round((min.x + max.x - textSize.x) * 0.5f);
    const float textY = max.y + std::round(4.0f * scale);
    const float labelOpacity = std::min(1.0f, opacity * 1.6f);
    drawList->AddText(ImVec2(textX + 1.0f, textY + 1.0f), WithOpacity(LABEL_SHADOW, labelOpacity), label.c_str());
    drawList->AddText(ImVec2(textX, textY), WithOpacity(LABEL_COLOR, labelOpacity), label.c_str());
}
}

void DrawSuperWeaponGlyph(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 color, float thickness)
{
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float radius = std::max(2.0f, std::min(max.x - min.x, max.y - min.y) * 0.34f);
    drawList->AddCircle(center, radius, color, 20, thickness);
    // 四根伸出圆环的准星线与一个中心点。
    const float inner = radius * 0.45f;
    const float outer = radius * 1.45f;
    drawList->AddLine(ImVec2(center.x - outer, center.y), ImVec2(center.x - inner, center.y), color, thickness);
    drawList->AddLine(ImVec2(center.x + inner, center.y), ImVec2(center.x + outer, center.y), color, thickness);
    drawList->AddLine(ImVec2(center.x, center.y - outer), ImVec2(center.x, center.y - inner), color, thickness);
    drawList->AddLine(ImVec2(center.x, center.y + inner), ImVec2(center.x, center.y + outer), color, thickness);
    drawList->AddCircleFilled(center, std::max(1.0f, thickness * 0.8f), color, 8);
}

std::size_t DrawSuperWeaponMarks(const viewer::ReplayViewSnapshot& snapshot, viewer::ReplayViewExchange& exchange,
                                 const GameImagePlacement& image, const ImVec2& origin, const ImVec2& extent,
                                 float scale, const ReplayHud::IconResolver& iconResolver)
{
    const auto frame = exchange.ReadMarks();
    if (!frame || frame->mSessionId != snapshot.mSessionId || frame->mMarks.empty())
        return 0;
    const auto mapper = MakeNativePointMapper(frame->mCanvasWidth, frame->mCanvasHeight, extent.x, extent.y, image);
    if (!mapper)
        return 0;
    const MarkLayout layout = LayoutSuperWeaponMarks(*frame, *mapper, scale);
    // 背景层在所有 HUD 窗口之下；再裁剪到战术视图，使图标不会画进侧栏面板之外的画面边缘或底部命令栏区。
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(origin.x + layout.mClip.mLeft, origin.y + layout.mClip.mTop),
                           ImVec2(origin.x + layout.mClip.mRight, origin.y + layout.mClip.mBottom), true);
    std::size_t drawn = 0;
    for (const MarkPlacement& placement : layout.mPlacements)
    {
        if (!placement.mIsVisible || !placement.mMark->mInfo)
            continue;
        const viewer::SuperWeaponMarkInfo& info = *placement.mMark->mInfo;
        const ImVec2 min(origin.x + placement.mIconRect.mLeft, origin.y + placement.mIconRect.mTop);
        const ImVec2 max(origin.x + placement.mIconRect.mRight, origin.y + placement.mIconRect.mBottom);
        const ImU32 ownerColor = GetChartColor(info.mOwnerColorRgb);
        DrawOwnerBase(drawList, min, max, ownerColor, placement.mOpacity, scale);
        const ImTextureID texture = info.mIcon && iconResolver ? iconResolver(info.mIcon) : nullptr;
        if (texture)
        {
            // 不透明度 60% 作用于整张图标，淡出时一起降到 0。
            drawList->AddImage(texture, min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                               WithOpacity(IM_COL32_WHITE, placement.mOpacity));
        }
        else
        {
            MarkPlacement shifted = placement;
            shifted.mIconRect = {min.x, min.y, max.x, max.y};
            DrawGenericMark(drawList, shifted, ownerColor, scale);
        }
        ++drawn;
    }
    drawList->PopClipRect();
    return drawn;
}
}
