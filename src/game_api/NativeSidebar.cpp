#include "NativeSidebar.h"

using byte = unsigned char;

#include <GameOptionsClass.h>
#include <SidebarClass.h>
#include <Surface.h>
#include <Unsorted.h>

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace rareplay::game_api
{
namespace
{
// 以下偏移由 IDA 对 SidebarClass::Draw 0x6A6C30、GetTacticalMapAvailability 0x656DE0 与
// GameOptionsClass 0xA8EB60 的读写核对；YRpp 声明漂移时必须在编译期失败。
static_assert(offsetof(GameOptionsClass, SidebarMode) == 0x1C);
static_assert(offsetof(PowerClass, PowerNeedRedraw) == 0x150C);
static_assert(offsetof(RadarClass, IsAvailableNow) == 0x14D8);
static_assert(offsetof(SidebarClass, Tabs) == 0x1544);
static_assert(sizeof(StripClass) == 0xF94);
static_assert(offsetof(SidebarClass, ActiveTabIndex) == 0x539C);
static_assert(offsetof(SidebarClass, IsSidebarActive) == 0x53A5);
static_assert(offsetof(SidebarClass, SidebarNeedsRedraw) == 0x53A6);
static_assert(offsetof(SidebarClass, SidebarBackgroundNeedsRedraw) == 0x53A7);
}

NativeSidebarState ReadNativeSidebarState() noexcept
{
    NativeSidebarState state;
    state.mIsOnRight = GameOptionsClass::Instance.SidebarMode;
    state.mIsArmageddonMode = Unsorted::ArmageddonMode != 0;
    return state;
}

std::optional<viewer::NativeUiLayout> ReadNativeUiLayout() noexcept
{
    const auto state = ReadNativeSidebarState();
    if (!state.mIsOnRight || state.mIsArmageddonMode)
        return std::nullopt;
    const auto& window = DSurface::WindowBounds;
    const auto& sidebar = DSurface::SidebarBounds;
    if (window.Width <= 0 || window.Height <= 0 || sidebar.Width <= 0 || sidebar.Height <= 0)
        return std::nullopt;
    // IDA 核对表明 SidebarBounds 已是窗口坐标；仍按 WindowBounds 原点归一，便于换算到客户区。
    const std::int64_t left = static_cast<std::int64_t>(sidebar.X) - window.X;
    const std::int64_t top = static_cast<std::int64_t>(sidebar.Y) - window.Y;
    if (left < 0 || top <= 0 || left + sidebar.Width > window.Width || top + sidebar.Height > window.Height)
        return std::nullopt;
    viewer::NativeUiLayout layout;
    layout.mCanvasWidth = window.Width;
    layout.mCanvasHeight = window.Height;
    layout.mSidebarPanel = {static_cast<int>(left), static_cast<int>(top), sidebar.Width, sidebar.Height};

    // 原生底部命令栏固定占 ViewBounds 下方、宽同 ViewBounds 的 32 行，与展开或收起无关（IDA 只读核实）。
    // 高度不是 32 时视为布局未就绪，不报告矩形，底栏只按自身内容排版。
    constexpr int COMMAND_BAR_HEIGHT = 32;
    const auto& view = DSurface::ViewBounds;
    const std::int64_t barX = static_cast<std::int64_t>(view.X) - window.X;
    const std::int64_t barY = static_cast<std::int64_t>(view.Y) + view.Height - window.Y;
    const std::int64_t barHeight = static_cast<std::int64_t>(window.Y) + window.Height -
                                   (static_cast<std::int64_t>(view.Y) + view.Height);
    if (barHeight == COMMAND_BAR_HEIGHT && view.Width > 0 && barX >= 0 && barY >= 0 &&
        barX + view.Width <= window.Width && barY + barHeight <= window.Height)
        layout.mCommandBar = viewer::NativeRect{static_cast<int>(barX), static_cast<int>(barY), view.Width,
                                                static_cast<int>(barHeight)};
    return layout;
}

bool IsNativeRadarAvailable() noexcept
{
    // 与 RadarClass::GetTacticalMapAvailability 相同的只读字段；RecheckRadarAvailability 0x508DF0 只为 LocalPlayer 维护。
    return RadarClass::Instance.IsAvailableNow;
}

void RequestNativeSidebarRedraw() noexcept
{
    auto& sidebar = SidebarClass::Instance;
    sidebar.SidebarNeedsRedraw = true;
    sidebar.SidebarBackgroundNeedsRedraw = true;
    sidebar.PowerNeedRedraw = true;
    const int tabIndex = sidebar.ActiveTabIndex;
    if (tabIndex >= 0 && tabIndex < static_cast<int>(std::size(sidebar.Tabs)))
    {
        sidebar.Tabs[tabIndex].AllowedToDraw = true;
        sidebar.Tabs[tabIndex].NeedsRedraw = true;
    }
}
}
