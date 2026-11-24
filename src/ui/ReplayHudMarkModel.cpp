#include "ReplayHudMarkModel.h"

#include "../viewer/RadarDisplayRules.h"

#include <algorithm>
#include <cmath>

namespace rareplay::ui
{
namespace
{
constexpr float OVERLAP_RATIO = 0.7f;
constexpr int MAX_STAGGER_STEPS = 4;
constexpr float STAGGER_STEP_X = 22.0f;
constexpr float STAGGER_STEP_Y = -18.0f;

struct IconSize
{
    float mWidth = 0.0f;
    float mHeight = 0.0f;
};

IconSize GetIconSize(const viewer::SuperWeaponMarkInfo& info, float scale)
{
    float width = MARK_FALLBACK_WIDTH;
    float height = MARK_FALLBACK_HEIGHT;
    if (info.mIcon && info.mIcon->mWidth > 0 && info.mIcon->mHeight > 0)
    {
        width = static_cast<float>(info.mIcon->mWidth);
        height = static_cast<float>(info.mIcon->mHeight);
        const float extent = std::max(width, height);
        if (extent > MARK_MAX_EXTENT)
        {
            width *= MARK_MAX_EXTENT / extent;
            height *= MARK_MAX_EXTENT / extent;
        }
    }
    return {std::max(8.0f, std::round(width * scale)), std::max(8.0f, std::round(height * scale))};
}

bool Intersects(const UiRect& left, const UiRect& right) noexcept
{
    return left.mLeft < right.mRight && right.mLeft < left.mRight && left.mTop < right.mBottom &&
           right.mTop < left.mBottom;
}
}

MarkLayout LayoutSuperWeaponMarks(const viewer::SuperWeaponMarkFrame& frame, const NativePointMapper& mapper,
    float scale)
{
    const float factor = std::isfinite(scale) ? std::max(1.0f, scale) : 1.0f;
    MarkLayout layout;
    layout.mClip = mapper.MapRect(frame.mView);
    layout.mPlacements.reserve(frame.mMarks.size());
    std::vector<UiPoint> anchors;
    anchors.reserve(frame.mMarks.size());
    for (const viewer::SuperWeaponMark& mark : frame.mMarks)
    {
        MarkPlacement placement;
        placement.mMark = &mark;
        if (!mark.mInfo)
        {
            anchors.push_back({});
            layout.mPlacements.push_back(placement);
            continue;
        }
        const IconSize size = GetIconSize(*mark.mInfo, factor);
        const UiPoint anchor = mapper.MapPoint(static_cast<float>(mark.mPosition.mX),
                                               static_cast<float>(mark.mPosition.mY));
        // 与更早的落点（按未错开的位置）挨得太近时，每多一个就多错开一步。
        int overlapCount = 0;
        for (std::size_t earlier = 0; earlier < anchors.size(); ++earlier)
        {
            if (!layout.mPlacements[earlier].mMark->mInfo)
                continue;
            if (std::abs(anchors[earlier].mX - anchor.mX) < size.mWidth * OVERLAP_RATIO &&
                std::abs(anchors[earlier].mY - anchor.mY) < size.mHeight * OVERLAP_RATIO)
                ++overlapCount;
        }
        const int steps = std::min(overlapCount, MAX_STAGGER_STEPS);
        const float centerX = anchor.mX + static_cast<float>(steps) * std::round(STAGGER_STEP_X * factor);
        const float centerY = anchor.mY + static_cast<float>(steps) * std::round(STAGGER_STEP_Y * factor);
        placement.mAnchor = anchor;
        placement.mIconRect = {std::round(centerX - size.mWidth * 0.5f), std::round(centerY - size.mHeight * 0.5f),
                               std::round(centerX - size.mWidth * 0.5f) + size.mWidth,
                               std::round(centerY - size.mHeight * 0.5f) + size.mHeight};
        placement.mOpacity = viewer::GetSuperWeaponMarkOpacity(frame.mGameTimeMs - mark.mInfo->mReleasedAtMs);
        placement.mIsVisible = placement.mOpacity > 0.0f && Intersects(placement.mIconRect, layout.mClip);
        anchors.push_back(anchor);
        layout.mPlacements.push_back(placement);
    }
    return layout;
}
}
