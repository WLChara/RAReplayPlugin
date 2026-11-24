#include "ZoomAnchor.h"

#include <cstdint>
#include <limits>

namespace rareplay::zoom
{
namespace
{
struct AxisView
{
    std::int64_t mTacticalPos = 0;
    std::int64_t mCenter = 0;
};

AxisView ComputeAxis(int physicalSize, int oldLogicalSize, int newLogicalSize, int anchor, int oldTacticalPos) noexcept
{
    const int effectiveAnchor = (anchor >= 0 && anchor < physicalSize) ? anchor : physicalSize / 2;
    const std::int64_t oldLogical = MapPhysicalToLogical(effectiveAnchor, physicalSize, oldLogicalSize);
    const std::int64_t newLogical = MapPhysicalToLogical(effectiveAnchor, physicalSize, newLogicalSize);
    AxisView axis;
    axis.mTacticalPos = static_cast<std::int64_t>(oldTacticalPos) + oldLogical - newLogical;
    axis.mCenter = axis.mTacticalPos + newLogicalSize / 2;
    return axis;
}

bool FitsInt(std::int64_t value) noexcept
{
    return value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
}
}

std::optional<AnchoredView> ComputeAnchoredView(const ZoomTransform& oldTransform, const ZoomTransform& newTransform,
    ZoomPoint physicalAnchor, ZoomPoint oldTacticalPos) noexcept
{
    if (!oldTransform.IsValid() || !newTransform.IsValid() ||
        oldTransform.mPhysicalWidth != newTransform.mPhysicalWidth ||
        oldTransform.mPhysicalHeight != newTransform.mPhysicalHeight)
        return std::nullopt;
    const AxisView x = ComputeAxis(newTransform.mPhysicalWidth, oldTransform.mLogicalWidth, newTransform.mLogicalWidth,
        physicalAnchor.mX, oldTacticalPos.mX);
    const AxisView y = ComputeAxis(newTransform.mPhysicalHeight, oldTransform.mLogicalHeight, newTransform.mLogicalHeight,
        physicalAnchor.mY, oldTacticalPos.mY);
    if (!FitsInt(x.mTacticalPos) || !FitsInt(x.mCenter) || !FitsInt(y.mTacticalPos) || !FitsInt(y.mCenter))
        return std::nullopt;
    return AnchoredView{
        {static_cast<int>(x.mTacticalPos), static_cast<int>(y.mTacticalPos)},
        {static_cast<int>(x.mCenter), static_cast<int>(y.mCenter)}};
}
}
