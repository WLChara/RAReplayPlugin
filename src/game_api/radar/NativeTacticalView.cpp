#include "NativeTacticalView.h"

#include "../zoom/BattlefieldZoomAdapter.h"

using byte = unsigned char;

#include <Windows.h>
#include <Unknwn.h>
#include <Surface.h>
#include <TacticalClass.h>

#include <cstddef>
#include <cstdint>

namespace rareplay::game_api
{
namespace
{
// TacticalClass 的镜头字段偏移由 IDA 对 0x6D2140 与 0x6D8B30 核对：CoordsToClient 读 [this+0xB0]、[this+0xB4]
// 作为 TacticalPos；YRpp 声明漂移时必须在编译期失败。
static_assert(offsetof(TacticalClass, TacticalPos) == 0xB0);
}

std::optional<TacticalViewGeometry> ReadTacticalViewGeometry() noexcept
{
    const auto& window = DSurface::WindowBounds;
    const auto& view = DSurface::ViewBounds;
    if (window.Width <= 0 || window.Height <= 0 || view.Width <= 0 || view.Height <= 0)
        return std::nullopt;
    const std::int64_t left = static_cast<std::int64_t>(view.X) - window.X;
    const std::int64_t top = static_cast<std::int64_t>(view.Y) - window.Y;
    if (left < 0 || top < 0 || left + view.Width > window.Width || top + view.Height > window.Height)
        return std::nullopt;
    TacticalViewGeometry geometry;
    geometry.mCanvasWidth = window.Width;
    geometry.mCanvasHeight = window.Height;
    geometry.mView = {static_cast<int>(left), static_cast<int>(top), view.Width, view.Height};
    return geometry;
}

std::optional<viewer::CanvasPoint> ProjectWorldToCanvas(const viewer::WorldPoint& world,
    const TacticalViewGeometry& geometry) noexcept
{
    const TacticalClass* tactical = TacticalClass::Instance;
    if (!tactical)
        return std::nullopt;
    const CoordStruct coords{world.mX, world.mY, world.mZ};
    Point2D client{0, 0};
    // 返回值只表示坐标是否落在视口外扩一圈的范围内，这里自己按图标大小裁剪，所以忽略它。
#pragma warning(suppress : 4996)
    (void)tactical->CoordsToClient(&coords, &client);
    // 战局缩放激活时 CoordsToClient 给出逻辑视口内的坐标（TacticalPos 与战术视口常驻 L），按屏幕上那一帧的映射
    // 换算为物理坐标；未激活时原样返回。插件与 HUD 拿到的始终是物理画布坐标。
    const rareplay::zoom::ZoomPoint physical = zoom::MapViewPointToPhysical({client.X, client.Y});
    return viewer::CanvasPoint{physical.mX + geometry.mView.mX, physical.mY + geometry.mView.mY};
}

bool CenterTacticalViewOn(const viewer::WorldPoint& world) noexcept
{
    TacticalClass* tactical = TacticalClass::Instance;
    if (!tactical)
        return false;
    CoordStruct coords{world.mX, world.mY, world.mZ};
    tactical->SetTacticalPosition(&coords);
    return true;
}
}
