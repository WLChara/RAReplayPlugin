#include "../src/ui/ReplayHudLayout.h"
#include "../src/ui/ReplayHudScale.h"

#include <cmath>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
using rareplay::ui::GameImagePlacement;
using rareplay::ui::GetSidebarPanelPlacement;
using rareplay::ui::MapNativeRect;
using rareplay::ui::UiRect;
using rareplay::viewer::NativeRect;
using rareplay::viewer::NativeUiLayout;

constexpr int SIDEBAR_WIDTH = 168;
constexpr int SIDEBAR_TOP = 158;

void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.01f;
}

bool SameRect(const UiRect& rect, float left, float top, float right, float bottom)
{
    return Near(rect.mLeft, left) && Near(rect.mTop, top) && Near(rect.mRight, right) && Near(rect.mBottom, bottom);
}

// 与 game_api 相同的规则：SidebarBounds = {画布宽 - 168, 158, 168, 画布高 - 158}。
NativeUiLayout MakeLayout(int canvasWidth, int canvasHeight)
{
    return {canvasWidth, canvasHeight,
            {canvasWidth - SIDEBAR_WIDTH, SIDEBAR_TOP, SIDEBAR_WIDTH, canvasHeight - SIDEBAR_TOP}};
}

void CheckNativeSize()
{
    for (const auto [width, height] : {std::pair{1600, 900}, std::pair{2560, 1440}, std::pair{1024, 768}})
    {
        const auto placement = GetSidebarPanelPlacement(MakeLayout(width, height),
            static_cast<float>(width), static_cast<float>(height), {});
        Require(placement && placement->mIsExact, "Native-size canvas maps exactly");
        Require(SameRect(placement->mRect, static_cast<float>(width - SIDEBAR_WIDTH), SIDEBAR_TOP,
                         static_cast<float>(width), static_cast<float>(height)),
                "Native-size panel equals SidebarBounds");
    }
}

void CheckUniformScale()
{
    // 1024×768 等比放大到 2048×1536：两种解释一致，面板正好 2 倍。
    const auto placement = GetSidebarPanelPlacement(MakeLayout(1024, 768), 2048.0f, 1536.0f, {});
    Require(placement && placement->mIsExact, "Uniform scale is unambiguous");
    Require(SameRect(placement->mRect, 1712.0f, 316.0f, 2048.0f, 1536.0f), "Uniform scale doubles SidebarBounds");
}

void CheckStretchWithImageRect()
{
    // cnc-ddraw OpenGL、maintas=false：最终视口为整个客户区，非等比拉伸。
    const auto placement = GetSidebarPanelPlacement(MakeLayout(1024, 768), 1600.0f, 900.0f,
        {{0.0f, 0.0f, 1600.0f, 900.0f}, true});
    Require(placement && placement->mIsExact, "Authoritative full viewport maps exactly");
    // 左 856 × 1600/1024 = 1337.5 向下取整；上 158 × 900/768 = 185.16 向下取整。
    Require(SameRect(placement->mRect, 1337.0f, 185.0f, 1600.0f, 900.0f), "Non-uniform stretch uses each axis scale");

    // 竖向更高的窗口：拉伸后的上沿仍是 158 × 1024/768 = 210.67，不能被等比留边解释推低。
    const auto tall = GetSidebarPanelPlacement(MakeLayout(1024, 768), 1024.0f, 1024.0f,
        {{0.0f, 0.0f, 1024.0f, 1024.0f}, true});
    Require(tall && tall->mIsExact && SameRect(tall->mRect, 856.0f, 210.0f, 1024.0f, 1024.0f),
            "Tall stretched window keeps exact stretched top");
}

void CheckLetterboxWithImageRect()
{
    // cnc-ddraw OpenGL、maintas=true：视口是左右留边后的图像区域。
    const auto placement = GetSidebarPanelPlacement(MakeLayout(1024, 768), 1600.0f, 900.0f,
        {{200.0f, 0.0f, 1400.0f, 900.0f}, true});
    Require(placement && placement->mIsExact, "Authoritative letterbox viewport maps exactly");
    // 左 200 + 856 × 1200/1024 = 1203.125；右 1400；上 185.16。
    Require(SameRect(placement->mRect, 1203.0f, 185.0f, 1400.0f, 900.0f), "Letterboxed image offsets the panel");
}

void CheckAmbiguousMapping()
{
    // D3D9 只给整块视口：拉伸与等比留边两种解释不同，必须标为不确定并保守避开雷达。
    const GameImagePlacement d3d9Viewport{{0.0f, 0.0f, 1024.0f, 1024.0f}, false};
    const auto tall = GetSidebarPanelPlacement(MakeLayout(1024, 768), 1024.0f, 1024.0f, d3d9Viewport);
    Require(tall && !tall->mIsExact, "Ambiguous tall window is not exact");
    // 拉伸解释的雷达下沿 210.67；留边解释为 128 + 158 = 286。保守上沿取较低者。
    Require(tall->mRect.mTop >= 286.0f, "Ambiguous top never covers radar in either interpretation");

    const auto wide = GetSidebarPanelPlacement(MakeLayout(1024, 768), 1600.0f, 900.0f, {});
    Require(wide && !wide->mIsExact, "Ambiguous wide window is not exact");
    // 拉伸左沿 1337.5，留边左沿 200 + 856 × 1.171875 = 1203.1：取并集覆盖两种可能。
    Require(wide->mRect.mLeft <= 1203.2f && wide->mRect.mRight >= 1600.0f, "Ambiguous sides take the union");
    Require(wide->mRect.mTop >= 185.0f, "Ambiguous wide top stays below radar");

    // 宿主未确认的视口若与拉伸、留边一致（同宽高比），仍可确定。
    const auto same = GetSidebarPanelPlacement(MakeLayout(1600, 900), 1600.0f, 900.0f,
        {{0.0f, 0.0f, 1600.0f, 900.0f}, false});
    Require(same && same->mIsExact, "Same aspect D3D9 viewport is exact");

    // 未确认的视口与拉伸不一致时也不能确定。
    const auto offset = GetSidebarPanelPlacement(MakeLayout(1600, 900), 1600.0f, 900.0f,
        {{0.0f, 40.0f, 1600.0f, 860.0f}, false});
    Require(offset && !offset->mIsExact && offset->mRect.mTop >= SIDEBAR_TOP,
            "Conflicting reference viewport is not exact and stays below radar");
}

void CheckRejectedLayouts()
{
    Require(!GetSidebarPanelPlacement(std::nullopt, 1024.0f, 768.0f, {}), "Unknown layout cannot show a panel");
    Require(!GetSidebarPanelPlacement(NativeUiLayout{1024, 0, {856, 158, 168, 610}}, 1024.0f, 768.0f, {}),
            "Invalid canvas rejected");
    // 侧栏在左时 SidebarBounds.X 等于画布宽，落在画布外。
    Require(!GetSidebarPanelPlacement(NativeUiLayout{1024, 768, {1024, 158, 168, 610}}, 1024.0f, 768.0f, {}),
            "Panel outside canvas rejected");
    Require(!GetSidebarPanelPlacement(MakeLayout(1600, 900), 0.0f, 900.0f, {}), "Invalid display rejected");
    // 显示缩到很小时面板无法排版，不显示面板，原生侧栏照常。
    Require(!GetSidebarPanelPlacement(MakeLayout(1600, 900), 800.0f, 450.0f, {}), "Too narrow panel rejected");
    // 未确认但越界的视口不参与换算。
    const auto ignored = GetSidebarPanelPlacement(MakeLayout(1600, 900), 1600.0f, 900.0f,
        {{0.0f, 0.0f, 3200.0f, 900.0f}, true});
    Require(ignored && ignored->mIsExact && SameRect(ignored->mRect, 1432.0f, 158.0f, 1600.0f, 900.0f),
            "Out-of-display image rect is ignored");
}

void CheckReserveAbovePanel()
{
    // 资金条与雷达块在面板上方，换算后与面板上沿相接、不重叠。
    const auto layout = MakeLayout(1024, 768);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const auto reserve = MapNativeRect(layout, NativeRect{856, 0, 168, SIDEBAR_TOP}, 1600.0f, 900.0f, image);
    const auto panel = GetSidebarPanelPlacement(layout, 1600.0f, 900.0f, image);
    Require(reserve && panel && reserve->mRect.mBottom >= panel->mRect.mTop &&
            reserve->mRect.mBottom - panel->mRect.mTop <= 1.0f,
            "Radar reserve ends where the panel starts");
}

void CheckAutoHudScale()
{
    using rareplay::ui::GetAutoHudScalePercent;
    using rareplay::ui::ResolveHudScalePercent;
    using rareplay::viewer::HudScaleSetting;
    // 客户区高度 → 自动档；正中（1080、1320、1800）取较大档。报告中的映射表以此为准。
    const std::pair<float, int> table[] = {
        {600.0f, 100}, {720.0f, 100}, {768.0f, 100}, {900.0f, 100}, {960.0f, 100}, {1050.0f, 100},
        {1080.0f, 125}, {1200.0f, 125}, {1260.0f, 125}, {1320.0f, 150}, {1440.0f, 150}, {1600.0f, 175},
        {1800.0f, 200}, {2160.0f, 200}, {2880.0f, 200}};
    for (const auto& [height, expected] : table)
        Require(GetAutoHudScalePercent(height) == expected,
                ("Auto HUD scale for height " + std::to_string(static_cast<int>(height))).c_str());
    Require(GetAutoHudScalePercent(0.0f) == 100 && GetAutoHudScalePercent(-5.0f) == 100 &&
            GetAutoHudScalePercent(std::nanf("")) == 100, "Invalid client height falls back to 100%");
    Require(ResolveHudScalePercent(HudScaleSetting::Auto, 1440.0f) == 150, "Auto resolves by client height");
    Require(ResolveHudScalePercent(HudScaleSetting::Percent200, 600.0f) == 200, "Fixed level ignores height");
    Require(ResolveHudScalePercent(HudScaleSetting::Percent125, 2160.0f) == 125, "Fixed level is not clamped by auto");
    Require(rareplay::viewer::ToHudScaleSetting(150) == HudScaleSetting::Percent150 &&
            rareplay::viewer::ToHudScaleSetting(0) == HudScaleSetting::Auto &&
            !rareplay::viewer::ToHudScaleSetting(110) && !rareplay::viewer::ToHudScaleSetting(-1),
            "Only defined percentages map to a setting");
}

void CheckScaledPanelWidth()
{
    struct Scenario
    {
        const char* mName;
        int mCanvasWidth;
        int mCanvasHeight;
        float mDisplayWidth;
        float mDisplayHeight;
        float mTop;
        float mLefts[5];
    };
    // 面板宽度 = 原生侧栏映射宽度 × 缩放，右缘贴画面右缘向左延伸，左缘向下取整；上沿始终是雷达块下沿。
    const Scenario scenarios[] = {
        {"2560x1440 native", 2560, 1440, 2560.0f, 1440.0f, 158.0f, {2392.0f, 2350.0f, 2308.0f, 2266.0f, 2224.0f}},
        {"1600x900 native", 1600, 900, 1600.0f, 900.0f, 158.0f, {1432.0f, 1390.0f, 1348.0f, 1306.0f, 1264.0f}},
        // 原生映射为 1337–1600（宽 263），再按缩放加宽。
        {"1024x768 stretched to 1600x900", 1024, 768, 1600.0f, 900.0f, 185.0f,
         {1337.0f, 1271.0f, 1205.0f, 1139.0f, 1074.0f}}};
    const float scales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    for (const Scenario& scenario : scenarios)
    {
        const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
        for (std::size_t index = 0; index < std::size(scales); ++index)
        {
            const auto placement = GetSidebarPanelPlacement(MakeLayout(scenario.mCanvasWidth, scenario.mCanvasHeight),
                scenario.mDisplayWidth, scenario.mDisplayHeight, image, scales[index]);
            const std::string message = std::string(scenario.mName) + " at " +
                std::to_string(static_cast<int>(scales[index] * 100.0f)) + "%";
            Require(placement && placement->mIsExact, (message + ": widened panel keeps exact coverage").c_str());
            Require(SameRect(placement->mRect, scenario.mLefts[index], scenario.mTop, scenario.mDisplayWidth,
                             scenario.mDisplayHeight), (message + ": widened panel rect").c_str());
        }
    }
    // 加宽不能越过客户区左缘。
    const auto clamped = GetSidebarPanelPlacement(MakeLayout(1600, 900), 1600.0f, 900.0f, {}, 20.0f);
    Require(clamped && clamped->mRect.mLeft == 0.0f, "Widened panel is clamped at the client left edge");
    Require(SameRect(GetSidebarPanelPlacement(MakeLayout(1600, 900), 1600.0f, 900.0f, {}, 0.5f)->mRect,
                     1432.0f, 158.0f, 1600.0f, 900.0f), "Scale below 100% never narrows the native panel");
}

void CheckUnitCardColumns()
{
    using rareplay::ui::GetUnitCardColumns;
    // 100% 档原生 168 像素面板：内容 156 像素、字号 18、间距 5，仍为两列。
    Require(GetUnitCardColumns(156.0f, 18.0f, 5.0f) == 2, "Narrowest 100% panel fits two unit cards");
    // 各档位面板随缩放加宽，内容宽度与字号同比，始终两列，且不会超过两列。
    for (const float scale : {1.25f, 1.5f, 1.75f, 2.0f})
        Require(GetUnitCardColumns(156.0f * scale, 18.0f * scale, 5.0f * scale) == 2, "Scaled panel keeps two columns");
    Require(GetUnitCardColumns(2000.0f, 18.0f, 5.0f) == 2, "Wide panels are capped at two columns");
    Require(GetUnitCardColumns(90.0f, 18.0f, 5.0f) == 1, "Too narrow content falls back to one column");
    Require(GetUnitCardColumns(std::nanf(""), 18.0f, 5.0f) == 1, "Invalid width falls back to one column");
}

void CheckFooterRect()
{
    using rareplay::ui::FooterLayoutInput;
    using rareplay::ui::GetFooterRect;
    FooterLayoutInput input;
    input.mDisplayWidth = 1600.0f;
    input.mDisplayHeight = 900.0f;
    input.mContentHeight = 106.0f;
    // 面板未显示：贴左、下、右三边，高度为内容高度。
    Require(SameRect(GetFooterRect(input), 0.0f, 794.0f, 1600.0f, 900.0f), "Footer spans the full width without a panel");
    // 面板显示：右缘贴面板左缘。
    input.mPanelLeft = 1432.0f;
    Require(SameRect(GetFooterRect(input), 0.0f, 794.0f, 1432.0f, 900.0f), "Footer ends at the panel left edge");
    input.mPanelLeft = 1306.6f;
    Require(SameRect(GetFooterRect(input), 0.0f, 794.0f, 1306.0f, 900.0f), "Footer right edge is floored to the panel");
    // 命令栏比内容高：加高到命令栏上沿，完整盖住它。
    input.mPanelLeft = 1432.0f;
    input.mCommandBar = UiRect{0.0f, 760.0f, 1432.0f, 900.0f};
    Require(SameRect(GetFooterRect(input), 0.0f, 760.0f, 1432.0f, 900.0f), "Footer grows to cover a taller command bar");
    // 命令栏比内容矮：保持内容高度。
    input.mCommandBar = UiRect{0.0f, 868.0f, 1432.0f, 900.0f};
    Require(SameRect(GetFooterRect(input), 0.0f, 794.0f, 1432.0f, 900.0f), "Footer keeps its content height over a short bar");
    // 命令栏不贴画面下缘（上下留边）：覆盖到命令栏上沿为止。
    input.mCommandBar = UiRect{0.0f, 700.0f, 1432.0f, 732.0f};
    Require(SameRect(GetFooterRect(input), 0.0f, 700.0f, 1432.0f, 900.0f), "Footer reaches a letterboxed command bar");
    // 小数高度向上取整，且不超过画面。
    input.mCommandBar.reset();
    input.mContentHeight = 132.5f;
    Require(SameRect(GetFooterRect(input), 0.0f, 767.0f, 1432.0f, 900.0f), "Fractional footer height is rounded up");
    input.mContentHeight = 5000.0f;
    Require(SameRect(GetFooterRect(input), 0.0f, 0.0f, 1432.0f, 900.0f), "Footer height is clamped to the display");
}

// 原生命令栏：侧栏在右时为 {0, H − 32, W − 168, 32}（IDA 核实，与 game_api 读取规则一致）。
NativeUiLayout MakeLayoutWithCommandBar(int canvasWidth, int canvasHeight)
{
    NativeUiLayout layout = MakeLayout(canvasWidth, canvasHeight);
    layout.mCommandBar = NativeRect{0, canvasHeight - 32, canvasWidth - SIDEBAR_WIDTH, 32};
    return layout;
}

void CheckFooterCoversNativeCommandBar()
{
    using rareplay::ui::FooterLayoutInput;
    using rareplay::ui::GetCommandBarClientRect;
    using rareplay::ui::GetFooterRect;
    struct Scenario
    {
        const char* mName;
        int mCanvasWidth;
        int mCanvasHeight;
        float mDisplayWidth;
        float mDisplayHeight;
        UiRect mExpectedBar;
    };
    const Scenario scenarios[] = {
        {"2560x1440 native", 2560, 1440, 2560.0f, 1440.0f, {0.0f, 1408.0f, 2392.0f, 1440.0f}},
        {"1600x900 native", 1600, 900, 1600.0f, 900.0f, {0.0f, 868.0f, 1432.0f, 900.0f}},
        // 上 736 × 900/768 = 862.5 向下取整，右 856 × 1600/1024 = 1337.5 向上取整。
        {"1024x768 stretched to 1600x900", 1024, 768, 1600.0f, 900.0f, {0.0f, 862.0f, 1338.0f, 900.0f}}};
    for (const Scenario& scenario : scenarios)
    {
        const auto layout = MakeLayoutWithCommandBar(scenario.mCanvasWidth, scenario.mCanvasHeight);
        const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
        const auto bar = GetCommandBarClientRect(layout, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
        Require(bar && SameRect(*bar, scenario.mExpectedBar.mLeft, scenario.mExpectedBar.mTop,
                                scenario.mExpectedBar.mRight, scenario.mExpectedBar.mBottom),
                (std::string(scenario.mName) + ": command bar maps outward to client pixels").c_str());
        for (const float scale : {1.0f, 1.5f})
        {
            const std::string name = std::string(scenario.mName) + " at " +
                std::to_string(static_cast<int>(scale * 100.0f)) + "%";
            const auto panel = GetSidebarPanelPlacement(layout, scenario.mDisplayWidth, scenario.mDisplayHeight,
                                                        image, scale);
            Require(panel.has_value(), (name + ": panel is shown").c_str());
            for (const float contentHeight : {std::round(106.0f * scale), 20.0f})
            {
                FooterLayoutInput input;
                input.mDisplayWidth = scenario.mDisplayWidth;
                input.mDisplayHeight = scenario.mDisplayHeight;
                input.mContentHeight = contentHeight;
                input.mPanelLeft = panel->mRect.mLeft;
                input.mCommandBar = bar;
                const UiRect footer = GetFooterRect(input);
                const float expectedTop = std::min(scenario.mDisplayHeight - contentHeight, bar->mTop);
                Require(SameRect(footer, 0.0f, expectedTop, panel->mRect.mLeft, scenario.mDisplayHeight),
                        (name + ": footer is flush and takes the larger of content and bar height").c_str());
                // 底栏与面板合起来完整盖住命令栏：左、下、上沿由底栏覆盖，右段落在面板之下。
                Require(footer.mLeft <= bar->mLeft && footer.mTop <= bar->mTop && footer.mBottom >= bar->mBottom,
                        (name + ": footer covers the command bar left, top and bottom").c_str());
                Require(footer.mRight >= bar->mRight ||
                        (panel->mRect.mLeft <= footer.mRight && panel->mRect.mTop <= bar->mTop &&
                         panel->mRect.mRight >= bar->mRight),
                        (name + ": the panel covers the rest of the command bar").c_str());
            }
        }
    }

    // 面板不显示（显示区太小）：底栏到画面右缘，仍盖住命令栏。
    const auto small = MakeLayoutWithCommandBar(1600, 900);
    Require(!GetSidebarPanelPlacement(small, 800.0f, 450.0f, {}), "Panel hidden on a tiny display");
    FooterLayoutInput hidden;
    hidden.mDisplayWidth = 800.0f;
    hidden.mDisplayHeight = 450.0f;
    hidden.mContentHeight = 10.0f;
    hidden.mCommandBar = GetCommandBarClientRect(small, 800.0f, 450.0f, {});
    Require(hidden.mCommandBar && SameRect(GetFooterRect(hidden), 0.0f, hidden.mCommandBar->mTop, 800.0f, 450.0f),
            "Without a panel the footer spans to the right edge and covers the command bar");

    // 命令栏不可用（未就绪、Armageddon、侧栏在左）：底栏照常贴边，只按内容高度。
    const auto noBar = MakeLayout(1600, 900);
    FooterLayoutInput unavailable;
    unavailable.mDisplayWidth = 1600.0f;
    unavailable.mDisplayHeight = 900.0f;
    unavailable.mContentHeight = 106.0f;
    unavailable.mPanelLeft = 1432.0f;
    unavailable.mCommandBar = GetCommandBarClientRect(noBar, 1600.0f, 900.0f, {});
    Require(!unavailable.mCommandBar && SameRect(GetFooterRect(unavailable), 0.0f, 794.0f, 1432.0f, 900.0f),
            "Unavailable command bar leaves a flush content-height footer");
}

void CheckCommandBarMapping()
{
    using rareplay::ui::GetCommandBarClientRect;
    using rareplay::ui::NativeRectPolicy;
    // 命令栏按原生规则 {0, H − 32, W − 168, 32}。
    auto layout = MakeLayout(1024, 768);
    Require(!GetCommandBarClientRect(layout, 1600.0f, 900.0f, {}), "Unknown command bar is not mapped");
    Require(!GetCommandBarClientRect(std::nullopt, 1600.0f, 900.0f, {}), "Missing layout has no command bar");
    layout.mCommandBar = NativeRect{0, 736, 856, 32};
    const auto exact = GetCommandBarClientRect(layout, 1600.0f, 900.0f, {{0.0f, 0.0f, 1600.0f, 900.0f}, true});
    // 上 736 × 900/768 = 862.5 向下取整，右 856 × 1600/1024 = 1337.5 向上取整。
    Require(exact && SameRect(*exact, 0.0f, 862.0f, 1338.0f, 900.0f), "Command bar maps through the image rect");
    // 不确定时取覆盖所有解释的上沿（最高者），与侧栏面板避开雷达的策略相反。
    const auto cover = GetCommandBarClientRect(layout, 1024.0f, 1024.0f, {});
    const auto avoid = MapNativeRect(layout, *layout.mCommandBar, 1024.0f, 1024.0f, {}, NativeRectPolicy::AvoidAbove);
    // 拉伸解释上沿 736 × 1024/768 = 981.3，留边解释为 128 + 736 = 864。
    Require(cover && cover->mTop <= 864.0f && avoid && avoid->mRect.mTop >= 981.0f,
            "Cover policy takes the highest top while avoid policy takes the lowest");
}

// 经济大窗口：地图区（画面去掉右侧面板与底栏）的 75%，居中；位置夹在画面内；各尺寸与缩放都不出画面。
void CheckEconomyWindowRect()
{
    using rareplay::ui::ClampRectToDisplay;
    using rareplay::ui::EconomyWindowLayoutInput;
    using rareplay::ui::GetEconomyWindowRect;
    using rareplay::ui::UiPoint;

    EconomyWindowLayoutInput input;
    input.mDisplayWidth = 2560.0f;
    input.mDisplayHeight = 1440.0f;
    input.mPanelLeft = 2392.0f;
    input.mFooterTop = 1334.0f;
    const UiRect centered = GetEconomyWindowRect(input, std::nullopt);
    // 地图区 2392 × 1334 的 75% = 1794 × 1000.5（向下取整为 1000），居中。
    Require(SameRect(centered, 299.0f, 167.0f, 2093.0f, 1167.0f), "Default window is 75% of the map area, centered");

    // 没有面板时地图区一直到画面右缘。
    input.mPanelLeft.reset();
    const UiRect noPanel = GetEconomyWindowRect(input, std::nullopt);
    Require(SameRect(noPanel, 320.0f, 167.0f, 2240.0f, 1167.0f), "Without a panel the map area spans the full width");
    input.mPanelLeft = 2392.0f;

    // 1920×1080：地图区 1752 × 974。
    EconomyWindowLayoutInput small = input;
    small.mDisplayWidth = 1920.0f;
    small.mDisplayHeight = 1080.0f;
    small.mPanelLeft = 1752.0f;
    small.mFooterTop = 974.0f;
    Require(SameRect(GetEconomyWindowRect(small, std::nullopt), 219.0f, 122.0f, 1533.0f, 852.0f),
            "1920x1080 window is 1314 x 730 centered in the 1752 x 974 map area");

    // 地图区很小：窗口取最小尺寸 460 × 330，但不超过地图区。
    EconomyWindowLayoutInput tiny;
    tiny.mDisplayWidth = 800.0f;
    tiny.mDisplayHeight = 600.0f;
    tiny.mPanelLeft = 632.0f;
    tiny.mFooterTop = 494.0f;
    const UiRect minimum = GetEconomyWindowRect(tiny, std::nullopt);
    Require(Near(minimum.mRight - minimum.mLeft, 474.0f) || Near(minimum.mRight - minimum.mLeft, 460.0f),
            "A small map area keeps the window readable");
    Require(minimum.mRight - minimum.mLeft >= 460.0f && minimum.mBottom - minimum.mTop >= 330.0f &&
                minimum.mRight <= tiny.mPanelLeft.value() && minimum.mBottom <= tiny.mFooterTop,
            "The minimum size does not exceed the map area when it fits");
    EconomyWindowLayoutInput cramped = tiny;
    cramped.mPanelLeft = 300.0f;
    cramped.mFooterTop = 200.0f;
    const UiRect clamped = GetEconomyWindowRect(cramped, std::nullopt);
    Require(clamped.mRight <= 300.0f && clamped.mBottom <= 200.0f && clamped.mLeft >= 0.0f && clamped.mTop >= 0.0f,
            "A map area smaller than the minimum size caps the window at the map area");

    // 缩放：最小尺寸随缩放增大，窗口仍在画面内。
    EconomyWindowLayoutInput scaled = tiny;
    scaled.mScale = 2.0f;
    const UiRect doubled = GetEconomyWindowRect(scaled, std::nullopt);
    Require(doubled.mRight <= scaled.mPanelLeft.value() && doubled.mBottom <= scaled.mFooterTop &&
                doubled.mRight - doubled.mLeft >= 460.0f,
            "A larger scale never pushes the window out of the map area");

    // 拖动后的位置：原样保留；拖出画面被夹回；位置取整。
    const UiRect moved = GetEconomyWindowRect(input, UiPoint{100.0f, 80.0f});
    Require(SameRect(moved, 100.0f, 80.0f, 1894.0f, 1080.0f), "A placed window keeps its size and position");
    Require(SameRect(GetEconomyWindowRect(input, UiPoint{-500.0f, -300.0f}), 0.0f, 0.0f, 1794.0f, 1000.0f),
            "Dragging past the top-left corner is clamped to the screen");
    Require(SameRect(GetEconomyWindowRect(input, UiPoint{5000.0f, 5000.0f}), 766.0f, 440.0f, 2560.0f, 1440.0f),
            "Dragging past the bottom-right corner is clamped to the screen");
    Require(SameRect(GetEconomyWindowRect(input, UiPoint{10.7f, 20.2f}), 10.0f, 20.0f, 1804.0f, 1020.0f),
            "Positions are whole pixels");

    // 画面变小：以前的位置被推回画面内，大小随新地图区重算。
    const UiRect shrunk = GetEconomyWindowRect(small, UiPoint{766.0f, 440.0f});
    Require(shrunk.mRight <= 1920.0f && shrunk.mBottom <= 1080.0f && shrunk.mLeft >= 0.0f && shrunk.mTop >= 0.0f,
            "A saved position is pulled back inside a smaller display");

    // 异常输入不崩溃并给出画面内的矩形。
    EconomyWindowLayoutInput broken;
    const UiRect degenerate = GetEconomyWindowRect(broken, std::nullopt);
    Require(degenerate.mRight >= degenerate.mLeft && degenerate.mBottom >= degenerate.mTop, "Degenerate input is safe");

    Require(SameRect(ClampRectToDisplay({-10.0f, 700.0f, 90.0f, 900.0f}, 800.0f, 600.0f), 0.0f, 400.0f, 100.0f, 600.0f),
            "ClampRectToDisplay keeps the size and moves the rectangle inside");
    Require(SameRect(ClampRectToDisplay({10.0f, 10.0f, 910.0f, 710.0f}, 800.0f, 600.0f), 0.0f, 0.0f, 900.0f, 700.0f),
            "A rectangle larger than the display sticks to the top-left");
}
}

int main()
{
    try
    {
        CheckNativeSize();
        CheckUniformScale();
        CheckStretchWithImageRect();
        CheckLetterboxWithImageRect();
        CheckAmbiguousMapping();
        CheckRejectedLayouts();
        CheckReserveAbovePanel();
        CheckAutoHudScale();
        CheckScaledPanelWidth();
        CheckUnitCardColumns();
        CheckFooterRect();
        CheckCommandBarMapping();
        CheckFooterCoversNativeCommandBar();
        CheckEconomyWindowRect();
        std::cout << "Replay HUD layout tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
