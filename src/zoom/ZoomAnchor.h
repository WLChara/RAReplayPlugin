#pragma once

#include "ZoomMapping.h"

#include <optional>

namespace rareplay::zoom
{
/** 换档后的镜头：左上角 TacticalPos 与镜头中心，单位都是等距像素。 */
struct AnchoredView
{
    ZoomPoint mTacticalPos;
    ZoomPoint mCenter;

    friend constexpr bool operator==(const AnchoredView&, const AnchoredView&) = default;
};

/**
 * 以物理锚点为中心换档：锚点下的逻辑点 m(a) + TacticalPos 在换档前后不变。
 * 各轴：a 在 [0, P) 内取 a，否则取 P/2（向下取整）；
 * TacticalPos2 = T1 + m1(a) − m2(a)，C2 = TacticalPos2 + L2/2，即 C2 = T1 + m1(a) − m2(a) + L2/2。
 * 非 1× 档的 L 都是偶数，C2 − L2/2 精确还原 TacticalPos2；1× 且 P 为奇数时 L2/2 向下取整，
 * 调用方从中心反推左上角时须使用同样的向下取整。
 *
 * @param oldTransform 换档前的映射（L1），必须合法。
 * @param newTransform 换档后的映射（L2），必须合法且物理尺寸与 oldTransform 相同。
 * @param physicalAnchor 战术区相对的物理锚点（物理像素），通常是鼠标的游戏画布坐标。
 * @param oldTacticalPos 换档前的 TacticalPos T1（等距像素，镜头左上角）。
 * @return 前置条件不满足或结果超出 int 时返回 std::nullopt。
 */
std::optional<AnchoredView> ComputeAnchoredView(const ZoomTransform& oldTransform, const ZoomTransform& newTransform,
    ZoomPoint physicalAnchor, ZoomPoint oldTacticalPos) noexcept;
}
