#include "../src/render/OverlayFonts.h"
#include "../src/render/OverlayInputQueue.h"
#include "../src/render/OverlayPanelPresence.h"
#include "../src/ui/ReplayHud.h"
#include "../src/ui/ReplayHudParts.h"
#include "../src/ui/ReplayHudLayout.h"
#include "../src/viewer/ReplayViewExchange.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::ui::EconomyTimeRange;
using rareplay::ui::GameImagePlacement;
using rareplay::ui::ReplayHud;
using rareplay::ui::ReplayPanel;
using rareplay::ui::UiRect;
using rareplay::viewer::ReplayViewSnapshot;

constexpr char PANEL_WINDOW_NAME[] = "回放面板##ReplaySidebarPanel";

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

LPARAM Position(int x, int y) { return MAKELPARAM(x, y); }

rareplay::viewer::UnitSummary MakeUnit(const char* typeId, const char* name,
                                       rareplay::viewer::UnitCategory category, std::uint32_t count)
{
    rareplay::viewer::UnitSummary unit;
    unit.mTypeId = typeId;
    unit.mName = name;
    unit.mCategory = category;
    unit.mCount = count;
    return unit;
}

// 故意使用长名称与大数值，检查窄面板中的裁剪与换行。
ReplayViewSnapshot MakeSnapshot(int canvasWidth, int canvasHeight)
{
    ReplayViewSnapshot snapshot;
    snapshot.mSessionId = 7;
    snapshot.mActive = true;
    snapshot.mReplayName = "一个非常非常长的回放文件名称_2026-10-20_遭遇战_四人混战.yrreplay";
    snapshot.mMapName = "很长很长的地图名称 · 测试用";
    snapshot.mRecordedLocalHouseIndex = 0;
    snapshot.mHasRecordedGroups = true;
    snapshot.mIsNativeRadarAvailable = false;
    snapshot.mNativeUiLayout = rareplay::viewer::NativeUiLayout{canvasWidth, canvasHeight,
        {canvasWidth - 168, 158, 168, canvasHeight - 158}};
    for (int index = 0; index < 4; ++index)
    {
        rareplay::viewer::PlayerSummary player;
        player.mHouseIndex = index;
        player.mName = index == 0 ? "超长玩家名称超长玩家名称超长" : "玩家" + std::to_string(index);
        player.mFaction = "尤里的复仇阵营名称";
        player.mIsHuman = index % 2 == 0;
        player.mDefeated = index == 3;
        player.mCredits = 1234567890;
        player.mPowerOutput = 123456;
        player.mPowerDrain = 654321;
        player.mHasProductionData = true;
        player.mHasUnitData = true;
        for (std::uint32_t line = 0; line < 3; ++line)
        {
            rareplay::viewer::ProductionLine production;
            production.mId = line + 1;
            production.mLabel = "战车工厂（北侧前线基地）";
            production.mProductName = "非常长的单位名称用于换行测试";
            production.mProgress = 0.4f;
            production.mQueuedCount = 12;
            player.mProductionLines.push_back(production);
        }
        player.mUnits = {
            MakeUnit("E1", "美国大兵加长名称", rareplay::viewer::UnitCategory::Infantry, 123),
            MakeUnit("MTNK", "灰熊坦克", rareplay::viewer::UnitCategory::Vehicle, 7),
            MakeUnit("FV", "多功能步兵车", rareplay::viewer::UnitCategory::Vehicle, 9999),
            MakeUnit("ORCA", "入侵者战机", rareplay::viewer::UnitCategory::Aircraft, 3),
            MakeUnit("GAPOWR", "发电厂", rareplay::viewer::UnitCategory::Building, 4)};
        snapshot.mPlayers.push_back(std::move(player));
    }
    rareplay::viewer::ControlGroup group;
    group.mNumber = 1;
    group.mUnits = {MakeUnit("E1", "美国大兵加长名称再加长一些", rareplay::viewer::UnitCategory::Infantry, 6)};
    group.mUnmatchedMemberCount = 2;
    snapshot.mRecordedGroups.push_back(group);
    return snapshot;
}

struct StatsVariant
{
    const char* mName;
    /** 每位玩家资金曲线的点数；负数表示没有曲线（缺失）。 */
    int mCurvePoints;
    /** 每条产线的停产历史条数。 */
    int mHistoryCount;
    bool mHasStatistics;
    bool mIsNominal;
    /** 各玩家兵营状态在六种状态中的轮转起点，使不同变体覆盖不同状态。 */
    int mStateOffset;
};

// 为玩家加上游戏时间、统计、产线状态与资金曲线；数值故意取得很大、名称很长，检查窄面板中的裁剪与换行。
void AddStatistics(ReplayViewSnapshot& snapshot, const StatsVariant& variant)
{
    using namespace rareplay::viewer;
    snapshot.mGameTime = GameTime{5'400'000 + 12'345, variant.mIsNominal};
    for (auto& player : snapshot.mPlayers)
    {
        player.mUnitTotal = 123456;
        if (variant.mHasStatistics)
        {
            PlayerStatistics stats;
            stats.mCombat = CombatStatistics{1234567, 7654321, 1111111, 2222222, 12.3456f};
            stats.mBuiltBuildings = 1234567;
            stats.mBuiltVehicles = 1234567;
            stats.mBuiltInfantry = 1234567;
            stats.mBuiltAircraft = 123;
            stats.mScore = 987654321;
            if (player.mIsHuman)
            {
                stats.mOperationCount = 4'000'000;
                stats.mOperationEventCount = 123'456'789;
                stats.mApm = 12345.0f;
            }
            stats.mBarracksUpgrade = {true, 3'599'000};
            stats.mWarFactoryUpgrade = {true, 36'000'000};
            player.mStatistics = stats;
        }
        auto history = std::make_shared<StallHistory>();
        for (int index = 0; index < variant.mHistoryCount; ++index)
            history->push_back({index * 60'000LL, index * 60'000LL + 5'000 + index, index % 2 == 0});
        const ProductionLineState states[] = {ProductionLineState::Producing, ProductionLineState::Paused,
                                              ProductionLineState::PowerLoss, ProductionLineState::NoFunds,
                                              ProductionLineState::Stalled, ProductionLineState::NoFactory};
        ProductionLineStatus barracks;
        barracks.mState = states[(player.mHouseIndex + variant.mStateOffset) % 6];
        barracks.mStalledSinceMs = 5'000'000;
        barracks.mStallHadPowerOutage = true;
        barracks.mHistory = history;
        // 汇总取很大的值（10 小时、100.0%）检查窄面板中的换行；部分玩家没有停产率（游戏时间为 0）。
        barracks.mTotalStalledMs = 36'000'000;
        barracks.mStallRatePercent = player.mHouseIndex % 3 == 2 ? std::nullopt : std::optional<float>(100.0f);
        ProductionLineStatus warFactory = barracks;
        warFactory.mState = ProductionLineState::Stalled;
        warFactory.mTotalStalledMs = 1'234'567;
        warFactory.mStallRatePercent = 23.456f;
        player.mBarracksLine = barracks;
        player.mWarFactoryLine = warFactory;
        if (variant.mCurvePoints >= 0)
        {
            auto curve = std::make_shared<EconomyCurve>();
            for (int index = 0; index < variant.mCurvePoints; ++index)
                curve->mPoints.push_back({index * 1000LL, 1000LL * (player.mHouseIndex + 1) + (index * 37 % 9000)});
            player.mEconomy = curve;
        }
    }
}

constexpr StatsVariant STATS_VARIANTS[] = {
    {"full stats", 600, 3, true, false, 0},
    {"nominal time", 80, 0, true, true, 2},
    {"no curve points", 0, 1, true, false, 4},
    {"single curve point", 1, 2, true, false, 1},
    {"many points and history", 600, 40, true, false, 3},
    {"missing statistics and curves", -1, 0, false, false, 5},
    {"1500 points", 1500, 5, true, false, 1}};

// 各档位的 HUD 缩放；字体按“18 像素 × 档位”重建，与游戏内宿主一致。
constexpr int SCALE_PERCENTS[] = {100, 125, 150, 175, 200};

class HeadlessImGui final
{
public:
    HeadlessImGui()
    {
        mContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(mContext);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        SetScalePercent(100);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(mContext); }

    // 与游戏内同一中文字体与重建方式，宽度检查才能反映真实字形；找不到时退回同比例的默认字体。
    void SetScalePercent(int percent)
    {
        if (percent == mScalePercent)
            return;
        mHasChineseFont = rareplay::render::LoadOverlayFont(ImGui::GetIO(), static_cast<float>(percent) / 100.0f);
        ImGui::GetIO().Fonts->Build();
        mScalePercent = percent;
    }

    void Draw(ReplayHud& hud, const ReplayViewSnapshot& snapshot, float width, float height,
              const GameImagePlacement& image, int frames = 3)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(width, height);
        io.DeltaTime = 1.0f / 60.0f;
        for (int frame = 0; frame < frames; ++frame)
        {
            ImGui::NewFrame();
            hud.SetScalePercent(mScalePercent);
            hud.SetGameImagePlacement(image);
            hud.Draw(snapshot, mExchange);
            ImGui::Render();
        }
    }

    rareplay::viewer::ReplayViewExchange& GetExchange() { return mExchange; }
    bool HasChineseFont() const noexcept { return mHasChineseFont; }

private:
    ImGuiContext* mContext = nullptr;
    rareplay::viewer::ReplayViewExchange mExchange;
    int mScalePercent = 0;
    bool mHasChineseFont = false;
};

bool ContainsRect(const std::vector<UiRect>& regions, const UiRect& expected)
{
    for (const UiRect& region : regions)
    {
        if (std::abs(region.mLeft - expected.mLeft) < 0.5f && std::abs(region.mTop - expected.mTop) < 0.5f &&
            std::abs(region.mRight - expected.mRight) < 0.5f && std::abs(region.mBottom - expected.mBottom) < 0.5f)
            return true;
    }
    return false;
}

bool IsConsumedClick(rareplay::render::OverlayInputQueue& input, int x, int y)
{
    const bool consumed = input.RouteMessage(nullptr, WM_LBUTTONDOWN, MK_LBUTTON, Position(x, y));
    input.RouteMessage(nullptr, WM_LBUTTONUP, 0, Position(x, y));
    input.Drain();
    return consumed;
}

struct SizeScenario
{
    const char* mName;
    int mCanvasWidth;
    int mCanvasHeight;
    float mDisplayWidth;
    float mDisplayHeight;
    // 100% 时（即原生侧栏换算到客户区）的面板矩形。
    UiRect mNativePanel;
};

constexpr SizeScenario SIZE_SCENARIOS[] = {
    {"2560x1440 native", 2560, 1440, 2560.0f, 1440.0f, {2392.0f, 158.0f, 2560.0f, 1440.0f}},
    {"1600x900 native", 1600, 900, 1600.0f, 900.0f, {1432.0f, 158.0f, 1600.0f, 900.0f}},
    {"1024x768 native", 1024, 768, 1024.0f, 768.0f, {856.0f, 158.0f, 1024.0f, 768.0f}},
    {"1024x768 stretched to 1600x900", 1024, 768, 1600.0f, 900.0f, {1337.0f, 185.0f, 1600.0f, 900.0f}}};

// 面板宽度 = 原生侧栏换算宽度 × 缩放，右缘贴画面右缘，左缘向下取整。
UiRect GetScaledPanel(const SizeScenario& scenario, int percent)
{
    UiRect panel = scenario.mNativePanel;
    const float width = (panel.mRight - panel.mLeft) * static_cast<float>(percent) / 100.0f;
    panel.mLeft = std::max(0.0f, std::floor(panel.mRight - width));
    return panel;
}

std::string Describe(const SizeScenario& scenario, int percent)
{
    return std::string(scenario.mName) + " at " + std::to_string(percent) + "%";
}

void CheckPanelInputRegion(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    ReplayHud hud;
    const auto snapshot = MakeSnapshot(1600, 900);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, {});
    Require(hud.IsSidebarPanelCovering(), "Native-size panel covers the native sidebar");
    const UiRect panel{1432.0f, 158.0f, 1600.0f, 900.0f};
    Require(ContainsRect(hud.GetInputRegions(), panel), "Panel input region equals SidebarBounds");

    rareplay::render::OverlayInputQueue input;
    input.PublishPolicy(true, true, false, hud.GetInputRegions());
    Require(IsConsumedClick(input, 1500, 500), "Click inside the panel is swallowed");
    Require(IsConsumedClick(input, 1432, 158), "Panel top-left pixel is swallowed");
    Require(IsConsumedClick(input, 1599, 899), "Panel bottom-right pixel is swallowed");
    Require(!IsConsumedClick(input, 1500, 157), "Radar bottom row passes through");
    Require(!IsConsumedClick(input, 1516, 100), "Radar map click passes through");
    Require(!IsConsumedClick(input, 1500, 8), "Money bar click passes through");
    // 外交、选项按钮位于雷达框 y≈20，保持可点击，不拦截。
    Require(!IsConsumedClick(input, 1445, 22), "Diplomacy button area passes through");
    Require(!IsConsumedClick(input, 1585, 22), "Options button area passes through");
    Require(!IsConsumedClick(input, 1431, 500), "Tactical map next to the panel passes through");

    // 每个档位：拦截区域等于加宽后的面板；雷达块与资金条保持原生宽度并放行，面板左侧、上方的地图放行。
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        for (const SizeScenario& scenario : SIZE_SCENARIOS)
        {
            ReplayHud scaledHud;
            const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
            imgui.Draw(scaledHud, MakeSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight),
                       scenario.mDisplayWidth, scenario.mDisplayHeight, image);
            const UiRect expected = GetScaledPanel(scenario, percent);
            const std::string name = Describe(scenario, percent);
            Require(scaledHud.IsSidebarPanelCovering(), name + ": widened panel still covers the native sidebar");
            Require(ContainsRect(scaledHud.GetInputRegions(), expected), name + ": input region is the widened panel");
            // 底栏贴左、下边缘，右到面板左缘，高度为内容高度（106 × 缩放，取整）。
            const float footerTop = scenario.mDisplayHeight - std::round(106.0f * static_cast<float>(percent) / 100.0f);
            Require(ContainsRect(scaledHud.GetInputRegions(),
                                 {0.0f, footerTop, expected.mLeft, scenario.mDisplayHeight}),
                    name + ": footer is flush with the left and bottom edges and ends at the panel");
            input.PublishPolicy(true, true, false, scaledHud.GetInputRegions());
            const int left = static_cast<int>(expected.mLeft);
            const int top = static_cast<int>(expected.mTop);
            const int radarX = static_cast<int>(scenario.mNativePanel.mLeft) + 20;
            Require(IsConsumedClick(input, left, top + 40), name + ": widened left edge is swallowed");
            Require(IsConsumedClick(input, static_cast<int>(expected.mRight) - 1, top),
                    name + ": panel top-right pixel is swallowed");
            Require(!IsConsumedClick(input, left - 1, top + 40), name + ": map left of the panel passes through");
            Require(!IsConsumedClick(input, radarX, top - 1), name + ": radar bottom row passes through");
            Require(!IsConsumedClick(input, radarX, 8), name + ": money bar passes through");
            if (left < static_cast<int>(scenario.mNativePanel.mLeft))
                Require(!IsConsumedClick(input, left + 2, top - 1),
                        name + ": map above the widened strip passes through");
        }
    }
    imgui.SetScalePercent(100);
}

void CheckNoHorizontalOverflow(const std::string& scenario, const UiRect& expectedPanel);

// 底栏盖住原生命令栏并吞掉其中的点击。
void CheckFooterCoversCommandBar(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(1600, 900);
    // 原生几何 {0, H − 32, W − 168, 32}：比底栏内容矮，底栏保持内容高度并盖住它。
    snapshot.mNativeUiLayout->mCommandBar = rareplay::viewer::NativeRect{0, 868, 1432, 32};
    {
        ReplayHud nativeHud;
        imgui.Draw(nativeHud, snapshot, 1600.0f, 900.0f, {});
        Require(ContainsRect(nativeHud.GetInputRegions(), {0.0f, 794.0f, 1432.0f, 900.0f}),
                "Footer keeps its content height over the native command bar");
        rareplay::render::OverlayInputQueue nativeInput;
        nativeInput.PublishPolicy(true, true, false, nativeHud.GetInputRegions());
        Require(IsConsumedClick(nativeInput, 40, 880), "Click on the native command bar is swallowed");
        Require(IsConsumedClick(nativeInput, 1431, 899), "Command bar end under the footer is swallowed");
    }
    // 合成的较高命令栏，用来验证底栏会加高到它的上沿。
    snapshot.mNativeUiLayout->mCommandBar = rareplay::viewer::NativeRect{0, 700, 1432, 200};
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, {});
    Require(ContainsRect(hud.GetInputRegions(), {0.0f, 700.0f, 1432.0f, 900.0f}),
            "Footer grows to the command bar top");
    rareplay::render::OverlayInputQueue input;
    input.PublishPolicy(true, true, false, hud.GetInputRegions());
    Require(IsConsumedClick(input, 300, 710), "Click on the covered command bar is swallowed");
    Require(IsConsumedClick(input, 0, 899), "Footer bottom-left pixel is swallowed");
    Require(!IsConsumedClick(input, 300, 699), "Map above the command bar passes through");

    // 面板未显示（无布局）时底栏铺满整宽，也不知道命令栏，只按内容高度。
    ReplayHud noPanelHud;
    snapshot.mNativeUiLayout.reset();
    imgui.Draw(noPanelHud, snapshot, 1600.0f, 900.0f, {});
    Require(ContainsRect(noPanelHud.GetInputRegions(), {0.0f, 794.0f, 1600.0f, 900.0f}),
            "Without a panel the footer spans the full width");
}

// 部队页：每行两张卡片，且类别子页签只列出所选类别。
void CheckUnitGrid(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(1600, 900);
    auto& units = snapshot.mPlayers.front().mUnits;
    const char* names[] = {"工程师", "军犬", "狙击手", "超时空军团兵（长名称）", "间谍", "谭雅"};
    for (const char* name : names)
        units.push_back(MakeUnit(name, name, rareplay::viewer::UnitCategory::Infantry, 2));
    std::size_t infantryCount = 0;
    for (const auto& unit : units)
        infantryCount += unit.mCategory == rareplay::viewer::UnitCategory::Infantry ? 1 : 0;

    ReplayHud hud;
    hud.SelectPanel(ReplayPanel::Units);
    hud.SelectUnitCategory(rareplay::viewer::UnitCategory::Infantry);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, {});
    CheckNoHorizontalOverflow("unit grid infantry", {1432.0f, 158.0f, 1600.0f, 900.0f});
    const ImGuiWindow* list = nullptr;
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if (window->Active && std::strstr(window->Name, "单位列表") != nullptr)
            list = window;
    }
    Require(list != nullptr, "Unit list child window is drawn below the category tabs");
    // 单列至少需要“每张卡片一行名称 + 一个 48 像素图标”的高度；两列应明显更矮。
    const float singleColumnHeight = static_cast<float>(infantryCount) * (18.0f + 48.0f);
    Require(list->ContentSize.y < singleColumnHeight * 0.8f,
            "Infantry cards are laid out two per row: content height " + std::to_string(list->ContentSize.y));
}

void CheckPresenceReport(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto& exchange = imgui.GetExchange();
    ReplayHud hud;
    auto snapshot = MakeSnapshot(1600, 900);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, {});
    rareplay::render::ReportPanelPresence(exchange, snapshot, hud);
    const auto presence = exchange.ReadHudPresence();
    Require(presence && presence->mSessionId == snapshot.mSessionId && presence->mIsPanelCovering,
            "Covering panel reports a covering heartbeat");

    // D3D9 整块视口、宽高比不同：位置不确定，面板仍显示但必须撤回心跳，原生侧栏继续绘制。
    ReplayHud ambiguousHud;
    const auto scaled = MakeSnapshot(1024, 768);
    imgui.Draw(ambiguousHud, scaled, 1600.0f, 900.0f, {{0.0f, 0.0f, 1600.0f, 900.0f}, false});
    Require(!ambiguousHud.IsSidebarPanelCovering(), "Ambiguous mapping is not covering");
    Require(ambiguousHud.GetInputRegions().size() >= 2, "Ambiguous mapping still shows the panel");
    rareplay::render::ReportPanelPresence(exchange, scaled, ambiguousHud);
    Require(exchange.ReadHudPresence() && !exchange.ReadHudPresence()->mIsPanelCovering,
            "Ambiguous mapping reports a visible HUD that does not cover the sidebar");

    // 没有布局（侧栏在左、Armageddon 或未就绪）时不显示面板。
    ReplayHud noLayoutHud;
    snapshot.mNativeUiLayout.reset();
    imgui.Draw(noLayoutHud, snapshot, 1600.0f, 900.0f, {});
    Require(!noLayoutHud.IsSidebarPanelCovering(), "No layout means no covering panel");
    Require(noLayoutHud.GetInputRegions().size() == 1, "Without layout only the footer takes input");
    rareplay::render::ReportPanelPresence(exchange, snapshot, noLayoutHud);
    Require(exchange.ReadHudPresence() && !exchange.ReadHudPresence()->mIsPanelCovering,
            "Without a panel the HUD is visible but not covering");
    auto inactive = snapshot;
    inactive.mActive = false;
    rareplay::render::ReportPanelPresence(exchange, inactive, noLayoutHud);
    Require(!exchange.ReadHudPresence(), "Inactive snapshot withdraws the heartbeat");
}

// 面板及其内容子窗口内所有控件的最右端不得越过可用区域，否则文字或按钮会溢出面板。
void CheckNoHorizontalOverflow(const std::string& scenario, const UiRect& expectedPanel)
{
    ImGuiContext& context = *ImGui::GetCurrentContext();
    bool foundPanel = false;
    for (ImGuiWindow* window : context.Windows)
    {
        if (!window->Active || std::strncmp(window->Name, PANEL_WINDOW_NAME, std::strlen(PANEL_WINDOW_NAME)) != 0)
            continue;
        if (std::strcmp(window->Name, PANEL_WINDOW_NAME) == 0)
        {
            foundPanel = true;
            Require(std::abs(window->Pos.x - expectedPanel.mLeft) < 0.5f &&
                    std::abs(window->Pos.y - expectedPanel.mTop) < 0.5f &&
                    std::abs(window->Pos.x + window->Size.x - expectedPanel.mRight) < 0.5f &&
                    std::abs(window->Pos.y + window->Size.y - expectedPanel.mBottom) < 0.5f,
                    scenario + ": panel window fills the widened sidebar rect");
        }
        Require(window->DC.CursorMaxPos.x <= window->WorkRect.Max.x + 0.5f,
                scenario + ": horizontal overflow in " + window->Name + " max=" +
                    std::to_string(window->DC.CursorMaxPos.x) + " limit=" + std::to_string(window->WorkRect.Max.x) +
                    " contentMax=" + std::to_string(window->ContentRegionRect.Max.x) + " pos=" + std::to_string(window->Pos.x) +
                    " size=" + std::to_string(window->Size.x) + " scrollbar=" + std::to_string(window->ScrollbarSizes.x));
    }
    Require(foundPanel, scenario + ": panel window was drawn");
}

void CheckTabsFitPanel(HeadlessImGui& imgui)
{
    const ReplayPanel panels[] = {ReplayPanel::Overview, ReplayPanel::Production, ReplayPanel::Units,
                                  ReplayPanel::Groups, ReplayPanel::Economy};
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        // 字号必须正好是基准 × 档位，不能再被窄面板自动缩小。
        const float expectedFont = 18.0f * static_cast<float>(percent) / 100.0f;
        for (const SizeScenario& scenario : SIZE_SCENARIOS)
        {
            const auto snapshot = MakeSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight);
            const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
            for (const ReplayPanel panel : panels)
            {
                ReplayHud hud;
                hud.SelectPanel(panel);
                imgui.Draw(hud, snapshot, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
                const std::string name = Describe(scenario, percent) + " tab " + std::to_string(static_cast<int>(panel));
                Require(hud.IsSidebarPanelCovering(), name + ": panel covers sidebar");
                CheckNoHorizontalOverflow(name, GetScaledPanel(scenario, percent));
                if (imgui.HasChineseFont())
                    Require(std::abs(ImGui::GetIO().Fonts->Fonts[0]->FontSize - expectedFont) < 0.01f,
                            name + ": font atlas matches the HUD scale");
            }
        }
    }
    imgui.SetScalePercent(100);
}

// 总览、生产、经济三页在每个档位、画布尺寸与统计变体（含 0 个点、1 个点、大量点、统计缺失、名义时间）下
// 都不水平溢出；生产页还依次选中每位玩家，覆盖各种产线状态。
// 已完成的统计页检查次数，结束时输出，便于确认这些检查确实运行。
int g_StatsPageChecks = 0;
int g_EconomyHoverChecks = 0;

void CheckStatsPagesFit(HeadlessImGui& imgui)
{
    const ReplayPanel panels[] = {ReplayPanel::Overview, ReplayPanel::Production, ReplayPanel::Economy};
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        for (const SizeScenario& scenario : SIZE_SCENARIOS)
        {
            const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
            for (const StatsVariant& variant : STATS_VARIANTS)
            {
                for (int selected = 0; selected < 4; ++selected)
                {
                    auto snapshot = MakeSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight);
                    snapshot.mRecordedLocalHouseIndex = selected;
                    AddStatistics(snapshot, variant);
                    for (const ReplayPanel panel : panels)
                    {
                        // 总览与经济页与所选玩家无关，只在第一位玩家上检查一次。
                        if (selected != 0 && panel != ReplayPanel::Production)
                            continue;
                        ReplayHud hud;
                        hud.SelectPanel(panel);
                        imgui.Draw(hud, snapshot, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
                        const std::string name = Describe(scenario, percent) + " " + variant.mName + " panel " +
                            std::to_string(static_cast<int>(panel)) + " player " + std::to_string(selected);
                        Require(hud.IsSidebarPanelCovering(), name + ": panel covers sidebar");
                        CheckNoHorizontalOverflow(name, GetScaledPanel(scenario, percent));
                        ++g_StatsPageChecks;
                    }
                }
            }
        }
    }
    imgui.SetScalePercent(100);
}

// 鼠标划过经济图表（含图例）时悬停读数不崩溃，且不引起溢出；曲线点数为 0 与 1 时同样。
void CheckEconomyHover(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    for (const StatsVariant& variant : STATS_VARIANTS)
    {
        auto snapshot = MakeSnapshot(1600, 900);
        AddStatistics(snapshot, variant);
        ReplayHud hud;
        hud.SelectPanel(ReplayPanel::Economy);
        const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
        int tooltipFrames = 0;
        for (int y = 200; y < 880; y += 37)
        {
            for (int x = 1420; x < 1600; x += 11)
            {
                ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
                imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 2);
                CheckNoHorizontalOverflow(std::string("hover ") + variant.mName, {1432.0f, 158.0f, 1600.0f, 900.0f});
                ++g_EconomyHoverChecks;
                const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
                tooltipFrames += tooltip != nullptr && tooltip->Active ? 1 : 0;
            }
        }
        // 有曲线点时，鼠标划过图表必须弹出悬停读数；没有点或没有曲线的变体不弹出。
        if (variant.mCurvePoints > 0)
            Require(tooltipFrames > 0, std::string("hover readout appears over the chart: ") + variant.mName);
    }
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// ---- 经济大窗口 ----

constexpr char ECONOMY_WINDOW_NAME[] = "资金曲线##ReplayEconomyWindow";

// 把快照补足到 count 位玩家（复制第一位并换下标、名称与颜色），用来检查多玩家时的顶点数与图例。
void ExtendPlayers(ReplayViewSnapshot& snapshot, int count)
{
    const std::uint32_t colors[] = {0xD4A753, 0xE04A4A, 0x4A7BE0, 0x58C26B, 0xE0873A, 0x4AC7D8, 0xB06AD8, 0xE06AB0};
    while (static_cast<int>(snapshot.mPlayers.size()) < count)
    {
        auto player = snapshot.mPlayers.front();
        player.mHouseIndex = static_cast<int>(snapshot.mPlayers.size());
        player.mName = "额外玩家" + std::to_string(player.mHouseIndex);
        snapshot.mPlayers.push_back(player);
    }
    for (std::size_t index = 0; index < snapshot.mPlayers.size(); ++index)
    {
        auto& player = snapshot.mPlayers[index];
        player.mColorRgb = colors[index % 8];
        if (player.mEconomy)
        {
            // 每位玩家一条不同的曲线，避免所有线重合。
            auto curve = std::make_shared<rareplay::viewer::EconomyCurve>(*player.mEconomy);
            for (auto& point : curve->mPoints)
                point.mCredits += static_cast<std::int64_t>(index) * 777 + point.mTimeMs / 7;
            player.mEconomy = curve;
        }
    }
}

const ImGuiWindow* FindWindowExact(const char* name)
{
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if (window->Active && std::strcmp(window->Name, name) == 0)
            return window;
    }
    return nullptr;
}

const ImGuiWindow* FindWindowContaining(const char* part)
{
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if (window->Active && std::strstr(window->Name, part) != nullptr)
            return window;
    }
    return nullptr;
}

// 大窗口及其子窗口不水平溢出，主窗口内容也不越过窗口下缘；窗口矩形等于期望值。
void CheckEconomyWindowFits(const std::string& scenario, const UiRect& expected)
{
    bool foundMain = false;
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if (!window->Active || std::strncmp(window->Name, ECONOMY_WINDOW_NAME, std::strlen(ECONOMY_WINDOW_NAME)) != 0)
            continue;
        Require(window->DC.CursorMaxPos.x <= window->WorkRect.Max.x + 0.5f,
                scenario + ": horizontal overflow in " + window->Name + " max=" +
                    std::to_string(window->DC.CursorMaxPos.x) + " limit=" + std::to_string(window->WorkRect.Max.x));
        if (std::strcmp(window->Name, ECONOMY_WINDOW_NAME) != 0)
            continue;
        foundMain = true;
        Require(std::abs(window->Pos.x - expected.mLeft) < 0.5f && std::abs(window->Pos.y - expected.mTop) < 0.5f &&
                    std::abs(window->Pos.x + window->Size.x - expected.mRight) < 0.5f &&
                    std::abs(window->Pos.y + window->Size.y - expected.mBottom) < 0.5f,
                scenario + ": the window is where layout says");
        Require(window->DC.CursorMaxPos.y <= window->Pos.y + window->Size.y + 0.5f,
                scenario + ": content runs past the window bottom: " + std::to_string(window->DC.CursorMaxPos.y) +
                    " > " + std::to_string(window->Pos.y + window->Size.y));
        Require(window->Pos.x >= -0.5f && window->Pos.y >= -0.5f &&
                    window->Pos.x + window->Size.x <= ImGui::GetIO().DisplaySize.x + 0.5f &&
                    window->Pos.y + window->Size.y <= ImGui::GetIO().DisplaySize.y + 0.5f,
                scenario + ": the window stays on screen");
    }
    Require(foundMain, scenario + ": the economy window was drawn");
}

// 大窗口在各档位、各尺寸、各数据变体下：位置在地图区居中、约 75%、不溢出、输入区域等于窗口矩形。
int g_EconomyWindowChecks = 0;

void CheckEconomyWindowLayoutAndRegions(HeadlessImGui& imgui)
{
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        for (const SizeScenario& scenario : SIZE_SCENARIOS)
        {
            const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
            for (const StatsVariant& variant : STATS_VARIANTS)
            {
                auto snapshot = MakeSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight);
                AddStatistics(snapshot, variant);
                ReplayHud hud;
                hud.SetEconomyWindowOpen(true);
                imgui.Draw(hud, snapshot, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
                const std::string name = Describe(scenario, percent) + " " + variant.mName;
                Require(hud.GetEconomyWindowRect().has_value(), name + ": window rect is reported");
                const UiRect rect = *hud.GetEconomyWindowRect();
                const UiRect panel = GetScaledPanel(scenario, percent);
                const float footerTop = scenario.mDisplayHeight - std::round(106.0f * static_cast<float>(percent) / 100.0f);
                const float mapWidth = panel.mLeft;
                // 约 75%（最小尺寸生效时更大），完全落在地图区内（地图区本身比最小尺寸小时贴满地图区）。
                const float expectedWidth = std::min(mapWidth, std::max(std::floor(mapWidth * 0.75f),
                    std::round(460.0f * static_cast<float>(percent) / 100.0f)));
                Require(std::abs((rect.mRight - rect.mLeft) - expectedWidth) <= 1.0f,
                        name + ": window width is 75% of the map area: " + std::to_string(rect.mRight - rect.mLeft));
                Require(rect.mLeft >= 0.0f && rect.mTop >= 0.0f && rect.mRight <= mapWidth + 0.5f &&
                            rect.mBottom <= footerTop + 0.5f,
                        name + ": window lies in the map area");
                Require(std::abs((rect.mLeft + rect.mRight) * 0.5f - mapWidth * 0.5f) <= 1.0f,
                        name + ": window is centered horizontally in the map area");
                Require(ContainsRect(hud.GetInputRegions(), rect), name + ": window rect is an input region");
                Require(hud.IsSidebarPanelCovering(), name + ": panel still covers the sidebar with the window open");
                CheckEconomyWindowFits(name, rect);
                CheckNoHorizontalOverflow(name, panel);
                ++g_EconomyWindowChecks;
            }
        }
    }
    imgui.SetScalePercent(100);
}

// 窗口打开与关闭两种状态下的输入拦截：打开时窗口内点击被吞，关闭后同一点放行。
void CheckEconomyWindowInputRegions(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(1600, 900);
    AddStatistics(snapshot, STATS_VARIANTS[0]);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    Require(!hud.GetEconomyWindowRect() && hud.GetInputRegions().size() == 2,
            "closed window adds no input region or rect");
    rareplay::render::OverlayInputQueue closedInput;
    closedInput.PublishPolicy(true, true, false, hud.GetInputRegions());
    Require(!IsConsumedClick(closedInput, 700, 400), "a click in the map center passes through while the window is closed");

    hud.SetEconomyWindowOpen(true);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    Require(hud.GetEconomyWindowRect() && hud.GetInputRegions().size() == 3, "open window adds one input region");
    const UiRect rect = *hud.GetEconomyWindowRect();
    rareplay::render::OverlayInputQueue openInput;
    openInput.PublishPolicy(true, true, false, hud.GetInputRegions());
    const int centerX = static_cast<int>((rect.mLeft + rect.mRight) * 0.5f);
    const int centerY = static_cast<int>((rect.mTop + rect.mBottom) * 0.5f);
    Require(IsConsumedClick(openInput, centerX, centerY), "a click inside the open window is swallowed");
    Require(IsConsumedClick(openInput, static_cast<int>(rect.mLeft), static_cast<int>(rect.mTop)),
            "the window top-left pixel is swallowed");
    Require(IsConsumedClick(openInput, static_cast<int>(rect.mRight) - 1, static_cast<int>(rect.mBottom) - 1),
            "the window bottom-right pixel is swallowed");
    Require(!IsConsumedClick(openInput, static_cast<int>(rect.mLeft) - 1, centerY), "map left of the window passes through");
    Require(!IsConsumedClick(openInput, centerX, static_cast<int>(rect.mTop) - 1), "map above the window passes through");
    Require(!IsConsumedClick(openInput, static_cast<int>(rect.mRight), centerY), "map right of the window passes through");
    Require(IsConsumedClick(openInput, 1500, 500), "the sidebar panel is still swallowed");

    // 再关闭：同一点重新放行。
    hud.SetEconomyWindowOpen(false);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    Require(!hud.GetEconomyWindowRect() && hud.GetInputRegions().size() == 2, "closing removes the region");
    rareplay::render::OverlayInputQueue reopened;
    reopened.PublishPolicy(true, true, false, hud.GetInputRegions());
    Require(!IsConsumedClick(reopened, centerX, centerY), "the same click passes through after closing");
}

void MovePointer(ReplayHud& hud, HeadlessImGui& imgui, const ReplayViewSnapshot& snapshot, float width, float height,
                 const GameImagePlacement& image, float x, float y, int frames = 2)
{
    ImGui::GetIO().AddMousePosEvent(x, y);
    imgui.Draw(hud, snapshot, width, height, image, frames);
}

// 点击一次（按下与抬起在不同帧，ImGui 才会登记为点击）。
void ClickPointer(ReplayHud& hud, HeadlessImGui& imgui, const ReplayViewSnapshot& snapshot, float width, float height,
                  const GameImagePlacement& image, float x, float y)
{
    MovePointer(hud, imgui, snapshot, width, height, image, x, y);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    imgui.Draw(hud, snapshot, width, height, image, 1);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    imgui.Draw(hud, snapshot, width, height, image, 2);
}

// 拖动标题栏：窗口跟随鼠标；拖出画面时被夹在画面内；松开后位置保持。
void CheckEconomyWindowDragAndClose(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(1600, 900);
    AddStatistics(snapshot, STATS_VARIANTS[0]);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    ReplayHud hud;
    hud.SetEconomyWindowOpen(true);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    const UiRect start = *hud.GetEconomyWindowRect();
    const float width = start.mRight - start.mLeft;
    const float height = start.mBottom - start.mTop;
    Require(!hud.GetEconomyState().mWindow.mHasPosition, "an untouched window has no saved position (stays centered)");

    // 在标题栏中部按下（离关闭按钮足够远）。
    const float grabX = start.mLeft + 120.0f;
    const float grabY = start.mTop + 14.0f;
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, grabX, grabY);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    Require(hud.GetEconomyState().mWindow.mIsDragging, "pressing the title bar starts a drag");
    // 向左上移动 60、30 像素。
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, grabX - 60.0f, grabY - 30.0f, 1);
    UiRect moved = *hud.GetEconomyWindowRect();
    Require(std::abs(moved.mLeft - (start.mLeft - 60.0f)) <= 1.0f && std::abs(moved.mTop - (start.mTop - 30.0f)) <= 1.0f &&
                std::abs((moved.mRight - moved.mLeft) - width) < 0.5f && std::abs((moved.mBottom - moved.mTop) - height) < 0.5f,
            "the window follows the pointer without changing size");
    Require(ContainsRect(hud.GetInputRegions(), moved), "the input region follows the window");

    // 拖出画面左上：夹在 (0, 0)。
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, -400.0f, -400.0f, 1);
    moved = *hud.GetEconomyWindowRect();
    Require(moved.mLeft == 0.0f && moved.mTop == 0.0f, "dragging past the top-left corner stops at the screen edge");
    // 拖到右下角之外：夹在右下。
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, 5000.0f, 5000.0f, 1);
    moved = *hud.GetEconomyWindowRect();
    Require(std::abs(moved.mRight - 1600.0f) < 0.5f && std::abs(moved.mBottom - 900.0f) < 0.5f,
            "dragging past the bottom-right corner stops at the screen edge");
    CheckEconomyWindowFits("dragged to the corner", moved);
    // 在画面内松开：位置保持，不再跟随。
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, 900.0f, 500.0f, 1);
    const UiRect released = *hud.GetEconomyWindowRect();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 2);
    Require(!hud.GetEconomyState().mWindow.mIsDragging, "releasing the button ends the drag");
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, 200.0f, 200.0f, 2);
    const UiRect settled = *hud.GetEconomyWindowRect();
    Require(std::abs(settled.mLeft - released.mLeft) < 0.5f && std::abs(settled.mTop - released.mTop) < 0.5f,
            "the window stays where it was released");
    Require(hud.GetEconomyState().mWindow.mHasPosition, "a dragged window remembers its position");

    // 画面变小：保存的位置被推回画面内。
    imgui.Draw(hud, snapshot, 1100.0f, 700.0f, {{0.0f, 0.0f, 1100.0f, 700.0f}, true}, 2);
    const UiRect shrunk = *hud.GetEconomyWindowRect();
    Require(shrunk.mLeft >= 0.0f && shrunk.mTop >= 0.0f && shrunk.mRight <= 1100.5f && shrunk.mBottom <= 700.5f,
            "a saved position is pulled back when the display shrinks");

    // 点击关闭按钮 × 关闭窗口（按钮在标题栏右端）。
    const UiRect now = *hud.GetEconomyWindowRect();
    ClickPointer(hud, imgui, snapshot, 1100.0f, 700.0f, {{0.0f, 0.0f, 1100.0f, 700.0f}, true}, now.mRight - 16.0f,
                 now.mTop + 15.0f);
    Require(!hud.GetEconomyState().mWindow.mIsOpen && !hud.GetEconomyWindowRect(), "the close button closes the window");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// 侧栏经济页的“展开大图”按钮：点一次打开、再点一次关闭；按钮状态反映窗口是否打开。
void CheckEconomyToggleButton(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(1600, 900);
    AddStatistics(snapshot, STATS_VARIANTS[0]);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    ReplayHud hud;
    hud.SelectPanel(ReplayPanel::Economy);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    Require(!hud.GetEconomyState().mWindow.mIsOpen, "the window starts closed");

    // 按钮是经济页标题下的第一个控件：沿面板内容区自上而下扫一遍找到它。
    float buttonY = -1.0f;
    for (float y = 296.0f; y < 420.0f && buttonY < 0.0f; y += 5.0f)
    {
        ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, 1500.0f, y);
        if (hud.GetEconomyState().mWindow.mIsOpen)
            buttonY = y;
    }
    Require(buttonY > 0.0f, "clicking the economy page button opens the large window");
    Require(hud.GetEconomyWindowRect().has_value(), "the window is drawn after the click");
    // 再点同一位置：按钮此时显示“收起大图”，点击关闭。
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, 1500.0f, buttonY);
    Require(!hud.GetEconomyState().mWindow.mIsOpen && !hud.GetEconomyWindowRect(), "clicking again closes the window");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// 筛选：图例里的“只看”、勾选框与“全部显示”；侧栏小图共用同一状态。
void CheckEconomyFilterThroughUi(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(1600, 900);
    AddStatistics(snapshot, STATS_VARIANTS[0]);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    ReplayHud hud;
    hud.SetEconomyWindowOpen(true);
    hud.SelectPanel(ReplayPanel::Economy);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    const auto& filter = hud.GetEconomyState().mFilter;
    const std::array<int, 4> houses = {0, 1, 2, 3};
    Require(filter.CountVisible(houses) == 4, "every player is visible at first");

    const ImGuiWindow* legend = FindWindowContaining("##经济图例");
    Require(legend != nullptr, "the legend is drawn beside the plot in a 1600x900 window");
    const float rowHeight = ImGui::GetFrameHeight() + 5.0f;
    const float gap = 6.0f;
    // “只看”按钮贴着每行右端：点击第三行（房屋 2）。
    const float onlyX = legend->Pos.x + legend->Size.x - 5.0f - 10.0f - (legend->ScrollbarSizes.x);
    const auto rowCenterY = [&](int row) { return legend->Pos.y + (rowHeight + gap) * static_cast<float>(row) + rowHeight * 0.5f; };
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, onlyX, rowCenterY(2));
    Require(filter.CountVisible(houses) == 1 && filter.IsVisible(2),
            "the Only button isolates that player (visible: " + std::to_string(filter.CountVisible(houses)) + ")");
    Require(hud.GetEconomyState().mFilter.GetOnlyVisible(houses) == 2, "player 2 is the sole visible player");

    // 同一状态下侧栏小图仍绘制且不溢出（筛选提示一行）。
    CheckNoHorizontalOverflow("small chart with a filter", {1432.0f, 158.0f, 1600.0f, 900.0f});

    // 点击玩家名区域同样只看该玩家（改选第一行）。
    const float nameX = legend->Pos.x + 30.0f;
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, nameX, rowCenterY(0));
    Require(filter.CountVisible(houses) == 1 && filter.IsVisible(0), "clicking the name isolates that player");

    // “全部显示”在工具栏第四个按钮：沿工具栏一行扫描找到它。
    const ImGuiWindow* window = FindWindowExact(ECONOMY_WINDOW_NAME);
    Require(window != nullptr, "window is drawn");
    bool restored = false;
    for (float x = window->Pos.x + 100.0f; x < window->Pos.x + 700.0f && !restored; x += 12.0f)
    {
        ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, x, window->Pos.y + 46.0f);
        restored = filter.CountVisible(houses) == 4;
    }
    Require(restored, "the Show all button restores every player");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// 悬停读数：鼠标划过大图的绘图区时弹出提示；0 点、1 点、1500 点都不崩溃；各档位下不溢出。
void CheckEconomyWindowHover(HeadlessImGui& imgui)
{
    for (const int percent : {100, 150})
    {
        imgui.SetScalePercent(percent);
        for (const StatsVariant& variant : STATS_VARIANTS)
        {
            auto snapshot = MakeSnapshot(1600, 900);
            AddStatistics(snapshot, variant);
            const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
            ReplayHud hud;
            hud.SetEconomyWindowOpen(true);
            hud.SelectEconomyRange(variant.mCurvePoints > 100 ? EconomyTimeRange::LastFiveMinutes : EconomyTimeRange::All);
            imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
            const UiRect rect = *hud.GetEconomyWindowRect();
            int tooltipFrames = 0;
            // 只扫绘图区（窗口左半部分、工具栏之下、横轴之上）：图例与按钮自己有悬停提示，不能算作读数。
            const float width = rect.mRight - rect.mLeft;
            const float height = rect.mBottom - rect.mTop;
            for (float y = rect.mTop + height * 0.25f; y < rect.mBottom - height * 0.2f; y += 41.0f)
            {
                for (float x = rect.mLeft + width * 0.15f; x < rect.mLeft + width * 0.5f; x += 53.0f)
                {
                    ImGui::GetIO().AddMousePosEvent(x, y);
                    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 2);
                    const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
                    tooltipFrames += tooltip != nullptr && tooltip->Active ? 1 : 0;
                    ++g_EconomyHoverChecks;
                }
            }
            const std::string name = std::string("window hover ") + variant.mName + " at " + std::to_string(percent) + "%";
            CheckEconomyWindowFits(name, *hud.GetEconomyWindowRect());
            if (variant.mCurvePoints > 0)
                Require(tooltipFrames > 0, name + ": the hover readout appears over the plot");
            else
                Require(tooltipFrames == 0, name + ": no readout without samples");
        }
    }
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    imgui.SetScalePercent(100);
}

// 每个绘制列表的顶点数必须低于 16 位索引的上限（无 VtxOffset 的后端也不会溢出）；
// 8 位玩家、每人 1500 点的最坏情形，同时记录无图形设备时一帧的 CPU 耗时。
void CheckEconomyVertexBudget(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(2560, 1440);
    AddStatistics(snapshot, STATS_VARIANTS[6]);
    ExtendPlayers(snapshot, 8);
    const GameImagePlacement image{{0.0f, 0.0f, 2560.0f, 1440.0f}, true};
    for (const EconomyTimeRange range : {EconomyTimeRange::All, EconomyTimeRange::LastTenMinutes})
    {
        ReplayHud hud;
        hud.SetEconomyWindowOpen(true);
        hud.SelectEconomyRange(range);
        hud.SelectPanel(ReplayPanel::Economy);
        imgui.Draw(hud, snapshot, 2560.0f, 1440.0f, image, 3);
        const ImDrawData* drawData = ImGui::GetDrawData();
        int worstList = 0;
        for (int index = 0; index < drawData->CmdListsCount; ++index)
            worstList = std::max(worstList, drawData->CmdLists[index]->VtxBuffer.Size);
        Require(worstList < 65'536, "the busiest draw list stays below 64K vertices: " + std::to_string(worstList));
        const auto begin = std::chrono::steady_clock::now();
        constexpr int FRAMES = 30;
        imgui.Draw(hud, snapshot, 2560.0f, 1440.0f, image, FRAMES);
        const double millisecondsPerFrame =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / FRAMES;
        std::cout << "economy window, 8 players x 1500 points, range " << static_cast<int>(range)
                  << ": busiest list " << worstList << " vertices, total " << drawData->TotalVtxCount
                  << " vertices, " << millisecondsPerFrame << " ms per frame (Draw + Render, no GPU)\n";
    }
}

// 会话状态：F10 隐藏期间不调用 Draw，窗口原样保留；会话变化回到关闭；首次 Draw 前预置的状态被保留。
void CheckEconomyWindowSessionState(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    auto snapshot = MakeSnapshot(1600, 900);
    AddStatistics(snapshot, STATS_VARIANTS[0]);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    ReplayHud hud;
    hud.SetEconomyWindowOpen(true);
    hud.SelectEconomyRange(EconomyTimeRange::LastTenMinutes);
    hud.ShowOnlyEconomyPlayer(2);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    const std::array<int, 4> houses = {0, 1, 2, 3};
    Require(hud.GetEconomyState().mWindow.mIsOpen && hud.GetEconomyState().mRange == EconomyTimeRange::LastTenMinutes &&
                hud.GetEconomyState().mFilter.GetOnlyVisible(houses) == 2,
            "state preset before the first Draw survives the session start");
    const UiRect before = *hud.GetEconomyWindowRect();

    // F10 隐藏：宿主不调用 Draw；再显示时窗口恢复到隐藏前的开合与位置。
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    const UiRect after = *hud.GetEconomyWindowRect();
    Require(std::abs(before.mLeft - after.mLeft) < 0.5f && std::abs(before.mTop - after.mTop) < 0.5f &&
                hud.GetEconomyState().mWindow.mIsOpen,
            "a hidden HUD resumes with the window as it was");

    // 新会话：全部回到默认。
    snapshot.mSessionId += 1;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    Require(!hud.GetEconomyState().mWindow.mIsOpen && hud.GetEconomyState().mRange == EconomyTimeRange::All &&
                hud.GetEconomyState().mFilter.CountVisible(houses) == 4 && !hud.GetEconomyWindowRect(),
            "a new session starts with the window closed and no filter");
    hud.SetEconomyWindowOpen(true);
    hud.Reset();
    Require(!hud.GetEconomyState().mWindow.mIsOpen, "Reset closes the window");
}

// ---- 雷达事件提示栏与超武落点图标 ----

constexpr char RADAR_BAR_NAME[] = "雷达事件##ReplayRadarBar";
constexpr std::int64_t RADAR_NOW_MS = 600'000;

rareplay::viewer::RadarEvent MakeRadarEvent(std::uint64_t id, std::int64_t ageMs, bool isInfiltration, bool hasLocation)
{
    rareplay::viewer::RadarEvent event;
    event.mId = id;
    event.mKind = isInfiltration ? rareplay::viewer::RadarEventKind::BuildingInfiltrated
                                 : rareplay::viewer::RadarEventKind::SuperWeaponLaunched;
    event.mGameTimeMs = RADAR_NOW_MS - ageMs;
    // 故意使用长名称，检查两行折行与裁剪。
    event.mActor = {1, "超长玩家名称超长玩家名称超长", 0x4E9A78};
    event.mVictim = {2, "另一位名称很长的玩家", 0xB45044};
    event.mSubjectName = isInfiltration ? "非常长的建筑名称用于折行测试" : "非常长的超级武器名称";
    if (hasLocation)
        event.mLocation = rareplay::viewer::WorldPoint{128 + 256 * static_cast<int>(id), 5000, 0};
    return event;
}

// 带游戏时间与事件列表的快照：ages 为各事件的年龄（毫秒），按给定顺序（最旧的在前）。
ReplayViewSnapshot MakeRadarSnapshot(int canvasWidth, int canvasHeight, std::initializer_list<std::int64_t> ages,
                                     bool withLocations = true)
{
    auto snapshot = MakeSnapshot(canvasWidth, canvasHeight);
    snapshot.mGameTime = rareplay::viewer::GameTime{RADAR_NOW_MS, false};
    auto list = std::make_shared<rareplay::viewer::RadarEventList>();
    std::uint64_t id = 1;
    for (const std::int64_t age : ages)
    {
        list->mEvents.push_back(MakeRadarEvent(id, age, id % 2 == 0, withLocations));
        ++id;
    }
    snapshot.mRadarEvents = list;
    return snapshot;
}

UiRect ExpectedRadarBar(const SizeScenario& scenario, int percent, bool collapsed, bool history, std::size_t rows)
{
    rareplay::ui::RadarBarLayoutInput input;
    input.mDisplayWidth = scenario.mDisplayWidth;
    input.mDisplayHeight = scenario.mDisplayHeight;
    input.mPanelLeft = GetScaledPanel(scenario, percent).mLeft;
    input.mFooterTop = scenario.mDisplayHeight - std::round(106.0f * static_cast<float>(percent) / 100.0f);
    input.mScale = static_cast<float>(percent) / 100.0f;
    input.mIsCollapsed = collapsed;
    input.mShowHistory = history;
    // 最近模式只显示放得下的最新几条。
    input.mRowCount = std::min(rows, rareplay::ui::GetRadarBarRowCapacity(input));
    return rareplay::ui::GetRadarBarRect(input);
}

// 各档位与画面尺寸下：提示栏在左侧中部，矩形等于布局计算值、整块是输入区域、窗口内容不水平溢出；
// 折叠只占标题行；历史模式高度固定；没有事件列表时不绘制。
int g_RadarBarChecks = 0;

void CheckRadarBarLayoutAndRegions(HeadlessImGui& imgui)
{
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        for (const SizeScenario& scenario : SIZE_SCENARIOS)
        {
            const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
            // 6 条事件，其中 5 条在 30 秒内（年龄 2、8、14、20、26 秒），1 条已过期：最近模式只显示 5 条。
            const auto snapshot = MakeRadarSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight,
                {90'000, 26'000, 20'000, 14'000, 8'000, 2'000});
            const std::string name = Describe(scenario, percent);
            struct Mode { bool mCollapsed; bool mHistory; std::size_t mRows; const char* mName; };
            for (const Mode mode : {Mode{false, false, 5, "recent"}, Mode{true, false, 5, "collapsed"},
                                    Mode{false, true, 5, "history"}})
            {
                ReplayHud hud;
                hud.SetRadarBarCollapsed(mode.mCollapsed);
                hud.SetRadarBarHistory(mode.mHistory);
                imgui.Draw(hud, snapshot, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
                const UiRect expected = ExpectedRadarBar(scenario, percent, mode.mCollapsed, mode.mHistory, mode.mRows);
                const std::string label = name + " " + mode.mName;
                Require(hud.GetRadarBarRect().has_value(), label + ": the bar rect is reported");
                const UiRect rect = *hud.GetRadarBarRect();
                Require(std::abs(rect.mLeft - expected.mLeft) < 0.5f && std::abs(rect.mTop - expected.mTop) < 0.5f &&
                            std::abs(rect.mRight - expected.mRight) < 0.5f && std::abs(rect.mBottom - expected.mBottom) < 0.5f,
                        label + ": the bar is where layout says");
                Require(ContainsRect(hud.GetInputRegions(), rect), label + ": the whole bar is an input region");
                Require(ContainsRect(hud.GetVisibleRegions(), rect), label + ": the bar hides the native cursor");
                Require(hud.IsSidebarPanelCovering(), label + ": the panel still covers the sidebar");
                const float titleHeight = std::round(30.0f * static_cast<float>(percent) / 100.0f);
                if (mode.mCollapsed)
                    Require(std::abs((rect.mBottom - rect.mTop) - titleHeight) < 0.5f, label + ": collapsed is the title row only");
                const ImGuiWindow* window = FindWindowExact(RADAR_BAR_NAME);
                Require(window != nullptr, label + ": the bar window was drawn");
                Require(std::abs(window->Pos.x - rect.mLeft) < 0.5f && std::abs(window->Size.x - (rect.mRight - rect.mLeft)) < 0.5f,
                        label + ": the window matches the rect");
                Require(window->DC.CursorMaxPos.x <= window->Pos.x + window->Size.x + 0.5f &&
                            window->DC.CursorMaxPos.y <= window->Pos.y + window->Size.y + 0.5f,
                        label + ": the bar content stays inside the window");
                // 左侧中部：贴左缘，竖直方向在 FPS 卡下沿与底栏上沿之间。
                const float scale = static_cast<float>(percent) / 100.0f;
                Require(rect.mLeft == std::round(14.0f * scale) && rect.mTop >= std::round(14.0f * scale) + std::round(180.0f * scale),
                        label + ": the bar is at the left edge below the FPS card");
                if (!mode.mHistory)
                    Require(rect.mBottom - rect.mTop <= std::round((30.0f + 5.0f * 44.0f + 6.0f) * scale) + 5.0f,
                            label + ": at most five rows tall");
                ++g_RadarBarChecks;
            }
            // 没有事件列表（功能未启用）：不绘制。空列表：标题行与一行提示。
            auto disabled = snapshot;
            disabled.mRadarEvents.reset();
            ReplayHud disabledHud;
            imgui.Draw(disabledHud, disabled, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
            Require(!disabledHud.GetRadarBarRect() && FindWindowExact(RADAR_BAR_NAME) == nullptr,
                    name + ": no event list means no bar");
            auto empty = snapshot;
            empty.mRadarEvents = std::make_shared<rareplay::viewer::RadarEventList>();
            ReplayHud emptyHud;
            imgui.Draw(emptyHud, empty, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
            const UiRect emptyExpected = ExpectedRadarBar(scenario, percent, false, false, 0);
            Require(emptyHud.GetRadarBarRect() && std::abs(emptyHud.GetRadarBarRect()->mBottom - emptyExpected.mBottom) < 0.5f,
                    name + ": an empty list shows the title row and a hint row");
        }
    }
    imgui.SetScalePercent(100);
}

// 提示栏的输入拦截：栏内点击被吞，栏外的地图放行；折叠后只吞标题行。
void CheckRadarBarInputRegions(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const auto snapshot = MakeRadarSnapshot(1600, 900, {20'000, 8'000, 2'000});
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    const UiRect rect = *hud.GetRadarBarRect();
    rareplay::render::OverlayInputQueue input;
    input.PublishPolicy(true, true, false, hud.GetInputRegions());
    const int centerY = static_cast<int>((rect.mTop + rect.mBottom) * 0.5f);
    Require(IsConsumedClick(input, static_cast<int>(rect.mLeft) + 40, centerY), "a click on an event row is swallowed");
    Require(IsConsumedClick(input, static_cast<int>(rect.mLeft), static_cast<int>(rect.mTop)), "the bar top-left pixel is swallowed");
    Require(IsConsumedClick(input, static_cast<int>(rect.mRight) - 1, static_cast<int>(rect.mBottom) - 1),
            "the bar bottom-right pixel is swallowed");
    Require(!IsConsumedClick(input, static_cast<int>(rect.mLeft) - 1, centerY), "map left of the bar passes through");
    Require(!IsConsumedClick(input, static_cast<int>(rect.mRight), centerY), "map right of the bar passes through");
    Require(!IsConsumedClick(input, static_cast<int>(rect.mLeft) + 40, static_cast<int>(rect.mBottom)), "map below the bar passes through");
    Require(!IsConsumedClick(input, static_cast<int>(rect.mLeft) + 40, static_cast<int>(rect.mTop) - 1), "map above the bar passes through");

    ReplayHud collapsedHud;
    collapsedHud.SetRadarBarCollapsed(true);
    imgui.Draw(collapsedHud, snapshot, 1600.0f, 900.0f, image);
    const UiRect collapsed = *collapsedHud.GetRadarBarRect();
    rareplay::render::OverlayInputQueue collapsedInput;
    collapsedInput.PublishPolicy(true, true, false, collapsedHud.GetInputRegions());
    Require(IsConsumedClick(collapsedInput, static_cast<int>(collapsed.mLeft) + 40, static_cast<int>(collapsed.mTop) + 10),
            "the collapsed title row is swallowed");
    Require(!IsConsumedClick(collapsedInput, static_cast<int>(collapsed.mLeft) + 40, static_cast<int>(rect.mTop) + 100),
            "the space the expanded rows occupied is open again after collapsing");
}

// 点击折叠按钮、“历史”开关与事件行：状态切换，点击事件向游戏线程提交居中命令；无位置的事件不提交。
void CheckRadarBarInteraction(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    auto snapshot = MakeRadarSnapshot(1600, 900, {26'000, 20'000, 8'000, 2'000});
    // 事件 2 没有位置（最旧的第二条，id 2）。
    auto list = std::make_shared<rareplay::viewer::RadarEventList>(*snapshot.mRadarEvents);
    list->mEvents[1].mLocation.reset();
    snapshot.mRadarEvents = list;
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    imgui.GetExchange().Drain();
    UiRect rect = *hud.GetRadarBarRect();

    // 事件行：最近模式最新的在上，第一行是 id 4。
    const float rowX = rect.mLeft + 120.0f;
    const float firstRowY = rect.mTop + 30.0f + 22.0f;
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rowX, firstRowY);
    auto commands = imgui.GetExchange().Drain();
    Require(commands.size() == 1 && commands[0].mKind == rareplay::viewer::ViewerCommandKind::CenterOnRadarEvent &&
                commands[0].mValue == 4 && commands[0].mSessionId == snapshot.mSessionId,
            "clicking the newest row asks the game thread to center on that event");
    // 第三行是 id 2（无位置）。
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rowX, firstRowY + 2.0f * 44.0f);
    Require(imgui.GetExchange().Drain().empty(), "an event without a location cannot be jumped to");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rowX, firstRowY + 3.0f * 44.0f);
    commands = imgui.GetExchange().Drain();
    Require(commands.size() == 1 && commands[0].mValue == 1, "the fourth row is the oldest event");

    // 折叠按钮：左端方块。
    Require(!hud.GetRadarBarState().mIsCollapsed, "the bar starts expanded");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rect.mLeft + 15.0f, rect.mTop + 15.0f);
    Require(hud.GetRadarBarState().mIsCollapsed, "the fold button collapses the bar");
    rect = *hud.GetRadarBarRect();
    Require(std::abs((rect.mBottom - rect.mTop) - 30.0f) < 0.5f, "a collapsed bar is the title row only");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rect.mLeft + 15.0f, rect.mTop + 15.0f);
    Require(!hud.GetRadarBarState().mIsCollapsed, "the fold button expands the bar again");

    // “历史”开关：右端。
    rect = *hud.GetRadarBarRect();
    Require(!hud.GetRadarBarState().mShowHistory, "recent mode is the default");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rect.mRight - 20.0f, rect.mTop + 15.0f);
    Require(hud.GetRadarBarState().mShowHistory, "the history switch turns on the full history");
    rect = *hud.GetRadarBarRect();
    // 历史模式的高度固定为 10 条，放不下时收到上沿到底栏之间的可用高度（1600×900 的 100% 档约 8 条）。
    const UiRect expectedHistory = ExpectedRadarBar(SIZE_SCENARIOS[1], 100, false, true, 4);
    Require(std::abs(rect.mBottom - expectedHistory.mBottom) < 0.5f && rect.mBottom - rect.mTop > 30.0f + 6.0f * 44.0f,
            "history mode has a fixed height that shows many rows");
    // 历史里点击第一行同样跳转（最新的在上）。
    imgui.GetExchange().Drain();
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rect.mLeft + 120.0f, rect.mTop + 30.0f + 22.0f);
    commands = imgui.GetExchange().Drain();
    Require(commands.size() == 1 && commands[0].mValue == 4, "history rows are clickable too");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rect.mRight - 20.0f, rect.mTop + 15.0f);
    Require(!hud.GetRadarBarState().mShowHistory, "the switch returns to the recent view");

    // 会话变化：折叠与历史回到默认；首次 Draw 前预置的状态被保留。
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rect.mLeft + 15.0f, rect.mTop + 15.0f);
    Require(hud.GetRadarBarState().mIsCollapsed, "collapsed before the session changes");
    auto next = snapshot;
    next.mSessionId += 1;
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    imgui.Draw(hud, next, 1600.0f, 900.0f, image);
    Require(!hud.GetRadarBarState().mIsCollapsed && !hud.GetRadarBarState().mShowHistory,
            "a new session starts expanded in recent mode");
    ReplayHud preset;
    preset.SetRadarBarCollapsed(true);
    imgui.Draw(preset, snapshot, 1600.0f, 900.0f, image);
    Require(preset.GetRadarBarState().mIsCollapsed, "state preset before the first Draw survives the session start");
    preset.Reset();
    Require(!preset.GetRadarBarState().mIsCollapsed, "Reset expands the bar");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// 悬停暂停淡出：鼠标在栏上时事件不会过期，移开后继续。
void CheckRadarBarHoverPause(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    auto snapshot = MakeRadarSnapshot(1600, 900, {10'000});
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    const UiRect rect = *hud.GetRadarBarRect();
    ImGui::GetIO().AddMousePosEvent(rect.mLeft + 120.0f, rect.mTop + 52.0f);
    // 游戏时间前进 60 秒，期间鼠标一直悬停在栏上：事件仍显示（FindWindow 里仍有 1 行的高度）。
    for (int step = 1; step <= 60; ++step)
    {
        snapshot.mGameTime = rareplay::viewer::GameTime{RADAR_NOW_MS + step * 1'000LL, false};
        imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    }
    Require(hud.GetRadarBarState().mWasHovered, "the pointer over the bar is reported as hovering");
    const UiRect hovered = *hud.GetRadarBarRect();
    Require(hovered.mBottom - hovered.mTop > 30.0f + 26.0f, "a hovered event never expires: the bar still has a row");
    // 移开：事件以冻结的年龄继续淡出，几十秒后消失，栏退回标题行加提示行。
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    for (int step = 61; step <= 100; ++step)
    {
        snapshot.mGameTime = rareplay::viewer::GameTime{RADAR_NOW_MS + step * 1'000LL, false};
        imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    }
    const UiRect after = *hud.GetRadarBarRect();
    Require(std::abs((after.mBottom - after.mTop) - (30.0f + 26.0f)) < 0.5f, "after the hover the event ages out");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// 提示栏与经济大窗口、FPS 卡的关系：重叠时大窗口盖住提示栏（窗口序在后）；提示栏不与 FPS 卡相交。
void CheckRadarBarVersusOtherWindows(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    for (const SizeScenario& scenario : SIZE_SCENARIOS)
    {
        const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
        auto snapshot = MakeRadarSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight, {20'000, 8'000, 2'000});
        AddStatistics(snapshot, STATS_VARIANTS[0]);
        ReplayHud hud;
        hud.SetEconomyWindowOpen(true);
        imgui.Draw(hud, snapshot, scenario.mDisplayWidth, scenario.mDisplayHeight, image);
        const std::string name = scenario.mName;
        const ImGuiWindow* bar = FindWindowExact(RADAR_BAR_NAME);
        const ImGuiWindow* fps = FindWindowContaining("录制帧率");
        const ImGuiWindow* economy = FindWindowExact(ECONOMY_WINDOW_NAME);
        Require(bar && fps && economy, name + ": all three windows were drawn");
        Require(bar->Pos.y >= fps->Pos.y + fps->Size.y, name + ": the bar does not overlap the FPS card");
        int barIndex = -1;
        int economyIndex = -1;
        int index = 0;
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
        {
            barIndex = window == bar ? index : barIndex;
            economyIndex = window == economy ? index : economyIndex;
            ++index;
        }
        Require(barIndex >= 0 && economyIndex > barIndex, name + ": the economy window is above the bar");
    }
}

// 超武落点图标：读取独立通道，只画战术视图内、未淡出、会话一致的落点。
std::shared_ptr<rareplay::viewer::SuperWeaponMarkInfo> MakeMarkInfo(std::uint64_t id, std::int64_t releasedAtMs)
{
    auto info = std::make_shared<rareplay::viewer::SuperWeaponMarkInfo>();
    info->mId = id;
    info->mReleasedAtMs = releasedAtMs;
    info->mOwnerColorRgb = 0x4E9A78;
    info->mLabel = "超长的超级武器名称用于检查标签";
    return info;
}

void CheckSuperWeaponMarks(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const auto snapshot = MakeRadarSnapshot(1600, 900, {2'000});
    auto frame = std::make_shared<rareplay::viewer::SuperWeaponMarkFrame>();
    frame->mSessionId = snapshot.mSessionId;
    frame->mGameTimeMs = RADAR_NOW_MS;
    frame->mCanvasWidth = 1600;
    frame->mCanvasHeight = 900;
    frame->mView = {0, 0, 1432, 868};
    frame->mMarks.push_back({MakeMarkInfo(1, RADAR_NOW_MS - 1'000), {400, 300}});         // 视图内
    frame->mMarks.push_back({MakeMarkInfo(2, RADAR_NOW_MS - 14'000), {700, 500}});        // 淡出中
    frame->mMarks.push_back({MakeMarkInfo(3, RADAR_NOW_MS - 1'000), {-300, 300}});        // 屏幕外
    frame->mMarks.push_back({MakeMarkInfo(4, RADAR_NOW_MS - 16'000), {800, 300}});        // 已淡出
    frame->mMarks.push_back({MakeMarkInfo(5, RADAR_NOW_MS - 1'000), {1500, 300}});        // 在侧栏一侧的视图之外
    frame->mMarks.push_back({MakeMarkInfo(6, RADAR_NOW_MS - 1'000), {400, 1200}});        // 画面之下
    auto& exchange = imgui.GetExchange();

    ReplayHud noMarks;
    imgui.Draw(noMarks, snapshot, 1600.0f, 900.0f, image);
    const int baseline = ImGui::GetDrawData()->TotalVtxCount;
    Require(noMarks.GetDrawnMarkCount() == 0, "no published marks means nothing is drawn");

    exchange.PublishMarks(frame);
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    Require(hud.GetDrawnMarkCount() == 2, "only on-screen, not-yet-faded marks are drawn: " + std::to_string(hud.GetDrawnMarkCount()));
    Require(ImGui::GetDrawData()->TotalVtxCount > baseline, "the visible marks add draw geometry");

    // 图标画在背景绘制层（所有 HUD 窗口之下）：背景层有内容。
    Require(ImGui::GetBackgroundDrawList()->VtxBuffer.Size == 0 || ImGui::GetDrawData()->CmdListsCount > 0,
            "marks go to the background layer");

    // 会话不一致的帧被忽略。
    auto stale = std::make_shared<rareplay::viewer::SuperWeaponMarkFrame>(*frame);
    stale->mSessionId += 5;
    exchange.PublishMarks(stale);
    ReplayHud staleHud;
    imgui.Draw(staleHud, snapshot, 1600.0f, 900.0f, image);
    Require(staleHud.GetDrawnMarkCount() == 0, "a frame from another session is ignored");

    // 暂停时游戏时间不前进：落点保持，不继续淡出（同一帧反复绘制结果不变）。
    exchange.PublishMarks(frame);
    for (int repeat = 0; repeat < 5; ++repeat)
    {
        imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
        Require(hud.GetDrawnMarkCount() == 2, "a paused game keeps the marks as they are");
    }

    // 落点撤回后不再绘制；每个档位与画面拉伸下都不崩溃、数量一致。
    exchange.PublishMarks(nullptr);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    Require(hud.GetDrawnMarkCount() == 0, "withdrawn marks are not drawn");
    exchange.PublishMarks(frame);
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        ReplayHud scaledHud;
        imgui.Draw(scaledHud, snapshot, 1600.0f, 900.0f, image);
        Require(scaledHud.GetDrawnMarkCount() == 2, "the same marks are drawn at " + std::to_string(percent) + "%");
        // 画布拉伸到更大的窗口：位置按比例换算，数量不变。
        ReplayHud stretchedHud;
        imgui.Draw(stretchedHud, snapshot, 2400.0f, 1350.0f, {{0.0f, 0.0f, 2400.0f, 1350.0f}, true});
        Require(stretchedHud.GetDrawnMarkCount() == 2, "marks survive a stretched canvas at " + std::to_string(percent) + "%");
    }
    exchange.PublishMarks(nullptr);
    imgui.SetScalePercent(100);
}

// 卡片辨识度：卡片填充相对面板背景有明显的明度差；选中卡片比普通卡片更醒目；边框比填充更亮；间距随缩放增大。
double GetLightness(ImU32 color)
{
    const auto channel = [](ImU32 value) {
        const double linear = static_cast<double>(value & 0xFF) / 255.0;
        return linear <= 0.04045 ? linear / 12.92 : std::pow((linear + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * channel(color >> IM_COL32_R_SHIFT) + 0.7152 * channel(color >> IM_COL32_G_SHIFT) +
                             0.0722 * channel(color >> IM_COL32_B_SHIFT);
    // CIE L*。
    return luminance > 0.008856 ? 116.0 * std::cbrt(luminance) - 16.0 : 903.3 * luminance;
}

// ---- 战局缩放控件 ----

int g_ZoomControlChecks = 0;

rareplay::viewer::BattlefieldZoomView MakeZoomView(int percent, bool canZoomIn, bool canZoomOut, bool hasPending = false)
{
    rareplay::viewer::BattlefieldZoomView view;
    view.mIsAvailable = true;
    view.mIsActive = percent != 100;
    view.mPercent = percent;
    view.mCanZoomIn = canZoomIn;
    view.mCanZoomOut = canZoomOut;
    view.mHasPendingChange = hasPending;
    return view;
}

rareplay::viewer::BattlefieldZoomView MakeUnavailableZoomView(const char* reason)
{
    rareplay::viewer::BattlefieldZoomView view;
    view.mUnavailableReason = reason;
    return view;
}

bool IsInside(const UiRect& inner, const UiRect& outer)
{
    return inner.mLeft >= outer.mLeft - 0.5f && inner.mTop >= outer.mTop - 0.5f && inner.mRight <= outer.mRight + 0.5f &&
           inner.mBottom <= outer.mBottom + 0.5f && inner.mRight > inner.mLeft && inner.mBottom > inner.mTop;
}

bool SameRect(const UiRect& a, const UiRect& b)
{
    return std::abs(a.mLeft - b.mLeft) < 0.5f && std::abs(a.mTop - b.mTop) < 0.5f &&
           std::abs(a.mRight - b.mRight) < 0.5f && std::abs(a.mBottom - b.mBottom) < 0.5f;
}

float CenterX(const UiRect& rect) { return (rect.mLeft + rect.mRight) * 0.5f; }
float CenterY(const UiRect& rect) { return (rect.mTop + rect.mBottom) * 0.5f; }

void CheckZoomControlModel()
{
    using rareplay::ui::BuildZoomControlModel;
    using rareplay::ui::ZoomControlMode;
    auto model = BuildZoomControlModel({}, true);
    Require(model.mMode == ZoomControlMode::Hidden && !model.mCanZoomIn && !model.mCanZoomOut && !model.mCanReset,
            "a default zoom view hides the controls");
    model = BuildZoomControlModel(MakeUnavailableZoomView("内存不足，无法缩放"), true);
    Require(model.mMode == ZoomControlMode::Unavailable && model.mLabel == "缩放不可用：内存不足，无法缩放" &&
                model.mCompactLabel == "缩放不可用" && !model.mCanZoomIn && !model.mCanZoomOut && !model.mCanReset,
            "an unavailable zoom shows its reason without buttons");
    model = BuildZoomControlModel(MakeZoomView(50, true, true), true);
    Require(model.mMode == ZoomControlMode::Controls && model.mLabel == "缩放 50%" && model.mCompactLabel == "50%" &&
                model.mCanZoomIn && model.mCanZoomOut && model.mCanReset,
            "an active 50% can zoom both ways and reset");
    model = BuildZoomControlModel(MakeZoomView(100, true, true), true);
    Require(model.mLabel == "缩放 100%" && !model.mCanReset, "reset is disabled at a settled 1x");
    model = BuildZoomControlModel(MakeZoomView(100, false, true, true), true);
    Require(model.mLabel == "缩放 100%…" && model.mCompactLabel == "100%…" && model.mCanReset && !model.mCanZoomIn,
            "a pending change shows the waiting mark and can be cancelled by reset");
    model = BuildZoomControlModel(MakeZoomView(400, false, true), true);
    Require(!model.mCanZoomIn && model.mCanZoomOut, "4x cannot zoom in");
    model = BuildZoomControlModel(MakeZoomView(33, true, false), true);
    Require(model.mLabel == "缩放 33%" && model.mCanZoomIn && !model.mCanZoomOut, "the farthest level cannot zoom out");
    model = BuildZoomControlModel(MakeZoomView(50, true, true), false);
    Require(model.mMode == ZoomControlMode::Controls && !model.mCanZoomIn && !model.mCanZoomOut && !model.mCanReset,
            "an inactive snapshot disables every button");
}

void CheckBattlefieldZoomControls(HeadlessImGui& imgui)
{
    using rareplay::ui::ZoomControlMode;
    using rareplay::viewer::ViewerCommandKind;
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const UiRect footer{0.0f, 794.0f, 1432.0f, 900.0f};
    auto& exchange = imgui.GetExchange();

    // 未启用：不显示，底栏与面板的输入区域不变。
    auto snapshot = MakeSnapshot(1600, 900);
    ReplayHud hiddenHud;
    imgui.Draw(hiddenHud, snapshot, 1600.0f, 900.0f, image);
    Require(!hiddenHud.GetZoomControlRects(), "zoom controls are hidden when zoom is not enabled");
    const auto baselineRegions = hiddenHud.GetInputRegions();

    // 可用：50%，三个按钮都在底栏内，且被底栏输入区域拦截。
    snapshot.mBattlefieldZoom = MakeZoomView(50, true, true);
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    Require(hud.GetZoomControlRects() && hud.GetZoomControlRects()->mMode == ZoomControlMode::Controls,
            "available zoom draws the controls");
    const auto rects = *hud.GetZoomControlRects();
    Require(IsInside(rects.mGroup, footer), "the zoom group sits inside the footer");
    for (const UiRect* button : {&rects.mZoomOut, &rects.mZoomIn, &rects.mReset})
        Require(IsInside(*button, rects.mGroup), "each zoom button is inside the group");
    Require(rects.mZoomOut.mRight <= rects.mZoomIn.mLeft && rects.mZoomIn.mRight <= rects.mReset.mLeft,
            "zoom buttons are ordered －, ＋, 复原 without overlap");
    Require(rects.mGroup.mTop > 794.0f + 2.0f * 20.0f, "the zoom group is on the third footer row");
    const auto& regions = hud.GetInputRegions();
    Require(regions.size() == baselineRegions.size(), "zoom controls add no input region of their own");
    for (std::size_t index = 0; index < regions.size(); ++index)
        Require(SameRect(regions[index], baselineRegions[index]), "zoom controls leave the existing regions unchanged");
    rareplay::render::OverlayInputQueue input;
    input.PublishPolicy(true, true, false, regions);
    for (const UiRect* button : {&rects.mZoomOut, &rects.mZoomIn, &rects.mReset})
        Require(IsConsumedClick(input, static_cast<int>(CenterX(*button)), static_cast<int>(CenterY(*button))),
                "a click on a zoom button does not reach the game");

    // 点击各按钮：提交带会话 ID 的命令。
    exchange.Drain();
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, CenterX(rects.mZoomIn), CenterY(rects.mZoomIn));
    auto commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::AdjustBattlefieldZoomAtCenter &&
                commands[0].mValue == 1 && commands[0].mSessionId == snapshot.mSessionId,
            "＋ submits one level in, anchored at the tactical center");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, CenterX(rects.mZoomOut), CenterY(rects.mZoomOut));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::AdjustBattlefieldZoomAtCenter &&
                commands[0].mValue == -1,
            "－ submits one level out, anchored at the tactical center");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, CenterX(rects.mReset), CenterY(rects.mReset));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::ResetBattlefieldZoom,
            "复原 submits a reset");
    // 悬停在文字上显示提示，不提交命令。
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, rects.mGroup.mLeft + 4.0f, CenterY(rects.mGroup), 3);
    Require(exchange.Drain().empty(), "hovering the zoom label submits nothing");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);

    // 文字宽度固定：等待标记与不同百分比都不移动按钮。
    for (const auto& view : {MakeZoomView(400, false, true), MakeZoomView(33, true, false),
             MakeZoomView(100, true, true, true), MakeZoomView(67, true, true, true)})
    {
        auto variant = snapshot;
        variant.mBattlefieldZoom = view;
        ReplayHud variantHud;
        imgui.Draw(variantHud, variant, 1600.0f, 900.0f, image);
        const auto variantRects = *variantHud.GetZoomControlRects();
        Require(SameRect(variantRects.mZoomOut, rects.mZoomOut) && SameRect(variantRects.mZoomIn, rects.mZoomIn) &&
                    SameRect(variantRects.mReset, rects.mReset),
                "zoom buttons keep their position for " + std::to_string(view.mPercent) + "%");
    }

    // 到头与 1×：不可用的按钮点击无效。
    struct DisabledCase
    {
        rareplay::viewer::BattlefieldZoomView mView;
        const UiRect* mButton;
        const char* mName;
    };
    const DisabledCase disabledCases[] = {
        {MakeZoomView(400, false, true), &rects.mZoomIn, "＋ at 4x"},
        {MakeZoomView(33, true, false), &rects.mZoomOut, "－ at the farthest level"},
        {MakeZoomView(100, true, true), &rects.mReset, "复原 at a settled 1x"}};
    for (const DisabledCase& disabledCase : disabledCases)
    {
        auto edge = snapshot;
        edge.mBattlefieldZoom = disabledCase.mView;
        ReplayHud edgeHud;
        imgui.Draw(edgeHud, edge, 1600.0f, 900.0f, image);
        exchange.Drain();
        ClickPointer(edgeHud, imgui, edge, 1600.0f, 900.0f, image, CenterX(*disabledCase.mButton),
                     CenterY(*disabledCase.mButton));
        Require(exchange.Drain().empty(), std::string(disabledCase.mName) + " is disabled");
    }
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);

    // 不可用：只有灰色说明，原来按钮的位置点击无效，但仍在底栏内被拦截。
    for (const char* reason : {"内存不足，无法缩放", "缩放出错，已停用", "当前布局不支持缩放"})
    {
        auto unavailable = snapshot;
        unavailable.mBattlefieldZoom = MakeUnavailableZoomView(reason);
        ReplayHud unavailableHud;
        imgui.Draw(unavailableHud, unavailable, 1600.0f, 900.0f, image);
        const auto unavailableRects = unavailableHud.GetZoomControlRects();
        Require(unavailableRects && unavailableRects->mMode == ZoomControlMode::Unavailable &&
                    IsInside(unavailableRects->mGroup, footer),
                std::string("unavailable zoom shows a note in the footer: ") + reason);
        exchange.Drain();
        ClickPointer(unavailableHud, imgui, unavailable, 1600.0f, 900.0f, image, CenterX(rects.mZoomIn),
                     CenterY(rects.mZoomIn));
        Require(exchange.Drain().empty(), "an unavailable zoom has no buttons");
    }
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);

    // 各档位、各尺寸：控件不越出底栏内容区（放不下完整文字时改用短文字）。
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        const float padding = std::round(12.0f * static_cast<float>(percent) / 100.0f);
        for (const SizeScenario& scenario : SIZE_SCENARIOS)
        {
            const GameImagePlacement scenarioImage{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
            const UiRect panel = GetScaledPanel(scenario, percent);
            const float footerTop = scenario.mDisplayHeight - std::round(106.0f * static_cast<float>(percent) / 100.0f);
            const UiRect content{padding, footerTop, panel.mLeft - padding, scenario.mDisplayHeight};
            for (const auto& view : {MakeZoomView(67, true, true, true), MakeUnavailableZoomView("缩放出错，已停用")})
            {
                auto scaled = MakeSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight);
                scaled.mBattlefieldZoom = view;
                ReplayHud scaledHud;
                imgui.Draw(scaledHud, scaled, scenario.mDisplayWidth, scenario.mDisplayHeight, scenarioImage);
                const auto scaledRects = scaledHud.GetZoomControlRects();
                const std::string name = Describe(scenario, percent) + (view.mIsAvailable ? " controls" : " note");
                Require(scaledRects.has_value(), name + ": zoom controls are drawn");
                Require(IsInside(scaledRects->mGroup, content),
                        name + ": zoom group stays inside the footer, right edge " +
                            std::to_string(scaledRects->mGroup.mRight) + " of " + std::to_string(content.mRight));
                ++g_ZoomControlChecks;
            }
        }
    }
    imgui.SetScalePercent(100);
}

void CheckCardStyle(HeadlessImGui& imgui)
{
    using rareplay::ui::detail::CardState;
    const double panel = GetLightness(rareplay::ui::detail::PANEL_BACKGROUND_COLOR);
    const auto normal = rareplay::ui::detail::GetCardColors(CardState::Normal);
    const auto hovered = rareplay::ui::detail::GetCardColors(CardState::Hovered);
    const auto selected = rareplay::ui::detail::GetCardColors(CardState::Selected);
    Require(GetLightness(normal.mFill) - panel >= 12.0,
            "a card is clearly lighter than the panel: " + std::to_string(GetLightness(normal.mFill) - panel) + " L*");
    Require(GetLightness(normal.mBorder) - GetLightness(normal.mFill) >= 20.0, "the card border is a light line");
    Require(GetLightness(hovered.mFill) > GetLightness(normal.mFill) && GetLightness(hovered.mBorder) > GetLightness(normal.mBorder),
            "hovering brightens the card");
    Require(selected.mFill != normal.mFill && selected.mBorder != normal.mBorder &&
                GetLightness(selected.mBorder) - GetLightness(normal.mBorder) >= 5.0,
            "a selected card is highlighted in both fill and border");

    float lastGap = 0.0f;
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        ImGui::NewFrame();
        const auto metrics = rareplay::ui::detail::GetCardMetrics();
        Require(metrics.mGap >= 4.0f && metrics.mGap >= lastGap && metrics.mGap == std::floor(metrics.mGap) &&
                    metrics.mBorder >= 1.0f && metrics.mPadding >= 4.0f,
                "card spacing is whole pixels and grows with the scale at " + std::to_string(percent) + "%");
        if (percent == 100)
            Require(metrics.mGap == 6.0f && metrics.mBorder == 1.0f && metrics.mPadding == 5.0f && metrics.mRounding == 3.0f,
                    "the 100% card parameters");
        lastGap = metrics.mGap;
        ImGui::EndFrame();
    }
    imgui.SetScalePercent(100);
}

// ---- 进度条与快进 ----

int g_SeekBarChecks = 0;

constexpr std::uint32_t SEEK_TOTAL_FRAMES = 10'000;
constexpr std::uint32_t SEEK_CURRENT_FRAME = 3'800;
constexpr std::uint32_t SEEK_START_FRAME = 2'000;
constexpr std::uint32_t SEEK_TARGET_FRAME = 6'000;
constexpr int SEEK_FRAME_MS = 33;

std::shared_ptr<const rareplay::viewer::GameTimeline> MakeTimeline(std::uint32_t frames, int frameMs,
                                                                   std::optional<std::uint32_t> firstNominal = std::nullopt)
{
    auto timeline = std::make_shared<rareplay::viewer::GameTimeline>();
    for (std::uint32_t frame = 0; frame < frames; ++frame)
        timeline->mCumulativeMs.push_back(static_cast<std::int64_t>(frame + 1) * frameMs);
    timeline->mFirstNominalFrame = firstNominal;
    return timeline;
}

enum class SeekScenario { ReadOnly, Seekable, Seeking };

const char* Describe(SeekScenario scenario)
{
    switch (scenario)
    {
    case SeekScenario::ReadOnly: return "read-only";
    case SeekScenario::Seekable: return "seekable";
    default: return "seeking";
    }
}

// 总帧数 10 000、停在第 3 800 帧；可跳转时能力标志为真，快进中从第 2 000 帧跳向第 6 000 帧。
ReplayViewSnapshot MakeSeekSnapshot(int canvasWidth, int canvasHeight, SeekScenario scenario)
{
    auto snapshot = MakeSnapshot(canvasWidth, canvasHeight);
    snapshot.mFrameNumber = SEEK_CURRENT_FRAME;
    snapshot.mPlaybackControls.mLastCompletedFrame = SEEK_CURRENT_FRAME;
    snapshot.mPlaybackControls.mCanRestart = true;
    snapshot.mCanChangePlaybackSpeed = true;
    snapshot.mPlaybackSpeedSetting = 3;
    snapshot.mTotalFrames = SEEK_TOTAL_FRAMES;
    snapshot.mTimeline = MakeTimeline(SEEK_TOTAL_FRAMES, SEEK_FRAME_MS);
    snapshot.mBattlefieldZoom = MakeZoomView(50, true, true);
    snapshot.mPlaybackControls.mCanSeek = scenario != SeekScenario::ReadOnly;
    if (scenario == SeekScenario::Seeking)
        snapshot.mPlaybackControls.mSeek = {true, SEEK_START_FRAME, SEEK_TARGET_FRAME};
    return snapshot;
}

// 进度条上某帧所在的像素横坐标（ImGui 把鼠标位置取整到像素），以及该坐标按界面同一算法换回的帧号；
// 一个像素约十帧，测试以换回的帧号为准。
float GetBarX(const UiRect& bar, std::uint32_t frame)
{
    return std::floor(bar.mLeft + (bar.mRight - bar.mLeft) *
                                      rareplay::ui::GetBarFractionOfFrame(frame, SEEK_TOTAL_FRAMES));
}

std::uint32_t GetFrameAtX(const UiRect& bar, float x)
{
    return rareplay::ui::GetFrameAtBarFraction((x - bar.mLeft) / (bar.mRight - bar.mLeft), SEEK_TOTAL_FRAMES);
}

void ReleasePointer()
{
    ImGui::GetIO().AddMouseButtonEvent(0, false);
}

void CheckSeekBarModel()
{
    using namespace rareplay::ui;
    Require(GetFrameAtBarFraction(0.0f, 10'000) == 0 && GetFrameAtBarFraction(1.0f, 10'000) == 9'999,
            "the bar spans frame 0 to the last frame");
    Require(GetFrameAtBarFraction(0.5f, 10'001) == 5'000, "the middle of 10 001 frames is frame 5 000");
    Require(GetFrameAtBarFraction(-1.0f, 10'000) == 0 && GetFrameAtBarFraction(2.0f, 10'000) == 9'999 &&
                GetFrameAtBarFraction(std::nanf(""), 10'000) == 0,
            "fractions outside 0..1 are clamped");
    Require(GetFrameAtBarFraction(0.7f, 0) == 0 && GetFrameAtBarFraction(0.7f, 1) == 0, "tiny replays map to frame 0");
    Require(GetBarFractionOfFrame(0, 10'000) == 0.0f && GetBarFractionOfFrame(9'999, 10'000) == 1.0f &&
                GetBarFractionOfFrame(50'000, 10'000) == 1.0f && GetBarFractionOfFrame(0, 1) == 1.0f,
            "frame fractions are clamped to the bar");
    for (const std::uint32_t frame : {0u, 1u, 17u, 36'000u, 71'999u, 72'000u})
        Require(GetFrameAtBarFraction(GetBarFractionOfFrame(frame, 72'001), 72'001) == frame,
                "frame " + std::to_string(frame) + " survives the round trip through the bar");

    auto snapshot = MakeSeekSnapshot(1600, 900, SeekScenario::Seekable);
    auto model = BuildSeekBarModel(snapshot);
    Require(model.mMode == SeekBarMode::Interactive && model.mTotalFrames == SEEK_TOTAL_FRAMES &&
                model.mCurrentFrame == SEEK_CURRENT_FRAME && model.mClickableAfterFrame == SEEK_CURRENT_FRAME &&
                !model.mIsSeeking && model.mSeekStatus.empty(),
            "a seekable snapshot gives an interactive bar");
    Require(!CanSeekToFrame(model, SEEK_CURRENT_FRAME) && CanSeekToFrame(model, SEEK_CURRENT_FRAME + 1) &&
                CanSeekToFrame(model, SEEK_TOTAL_FRAMES - 1) && !CanSeekToFrame(model, SEEK_TOTAL_FRAMES) &&
                !CanSeekToFrame(model, 0),
            "only frames after the current one and within the replay are targets");

    struct ModeCase
    {
        const char* mName;
        void (*mChange)(ReplayViewSnapshot&);
        SeekBarMode mExpected;
    };
    const ModeCase modeCases[] = {
        {"unknown total", [](ReplayViewSnapshot& s) { s.mTotalFrames.reset(); }, SeekBarMode::Hidden},
        {"zero total", [](ReplayViewSnapshot& s) { s.mTotalFrames = 0u; }, SeekBarMode::Hidden},
        {"no capability", [](ReplayViewSnapshot& s) { s.mPlaybackControls.mCanSeek = false; }, SeekBarMode::ReadOnly},
        {"inactive", [](ReplayViewSnapshot& s) { s.mActive = false; }, SeekBarMode::ReadOnly},
        {"one frame", [](ReplayViewSnapshot& s) { s.mTotalFrames = 1u; }, SeekBarMode::ReadOnly}};
    for (const ModeCase& modeCase : modeCases)
    {
        auto changed = snapshot;
        modeCase.mChange(changed);
        model = BuildSeekBarModel(changed);
        Require(model.mMode == modeCase.mExpected, std::string("bar mode: ") + modeCase.mName);
        Require(!CanSeekToFrame(model, SEEK_CURRENT_FRAME + 100), std::string("no target when ") + modeCase.mName);
    }

    // 快进中：可点区域从目标之后开始；状态文字的时间来自时间轴（第 6 000 帧完成后 = 6 001 × 33 ms = 03:18）。
    snapshot = MakeSeekSnapshot(1600, 900, SeekScenario::Seeking);
    model = BuildSeekBarModel(snapshot);
    Require(model.mIsSeeking && model.mSeekStartFrame == SEEK_START_FRAME && model.mSeekTargetFrame == SEEK_TARGET_FRAME &&
                model.mClickableAfterFrame == SEEK_TARGET_FRAME && model.mSeekPercent == 45,
            "seeking from 2 000 to 6 000 at 3 800 is 45%");
    Require(model.mSeekStatus == "正在跳转到 03:18 … 45%", "seek status text: " + model.mSeekStatus);
    Require(!CanSeekToFrame(model, 5'000) && !CanSeekToFrame(model, SEEK_TARGET_FRAME) &&
                CanSeekToFrame(model, SEEK_TARGET_FRAME + 1),
            "while seeking only frames after the target are targets");
    snapshot.mTimeline.reset();
    Require(BuildSeekBarModel(snapshot).mSeekStatus == "正在跳转到帧 6000 … 45%", "seek status without a timeline");
    snapshot.mTimeline = MakeTimeline(SEEK_TOTAL_FRAMES, SEEK_FRAME_MS, 0);
    Require(BuildSeekBarModel(snapshot).mSeekStatus == "正在跳转到 03:18（名义） … 45%", "nominal seek status");
    snapshot.mFrameNumber = SEEK_TARGET_FRAME;
    Require(BuildSeekBarModel(snapshot).mSeekPercent == 100, "100% at the target");
    snapshot.mActive = false;
    Require(!BuildSeekBarModel(snapshot).mIsSeeking, "an inactive snapshot shows no seek");

    // 悬停说明：时间来自时间轴，名义值附“（名义）”；没有时间轴时只有帧号。
    const auto timeline = MakeTimeline(SEEK_TOTAL_FRAMES, SEEK_FRAME_MS, 5'000);
    Require(FormatSeekPointLabel(timeline.get(), 4'000) == "02:12（帧 4000）", "label before the nominal range");
    Require(FormatSeekPointLabel(timeline.get(), 6'000) == "03:18（帧 6000）（名义）", "label in the nominal range");
    Require(FormatSeekPointLabel(nullptr, 6'000) == "帧 6000", "label without a timeline");
    Require(FormatSeekPointLabel(timeline.get(), 20'000) == "帧 20000", "label outside the timeline");

    model = BuildSeekBarModel(MakeSeekSnapshot(1600, 900, SeekScenario::Seekable));
    Require(BuildSeekBarTooltip(model, timeline.get(), 4'000) == "02:12（帧 4000）", "a target needs no reason");
    Require(BuildSeekBarTooltip(model, timeline.get(), 1'000) ==
                std::string("00:33（帧 1000）\n") + SEEK_BACKWARD_TOOLTIP,
            "the backward region explains the reload");
    Require(BuildSeekBarTooltip(model, timeline.get(), SEEK_CURRENT_FRAME) ==
                FormatSeekPointLabel(timeline.get(), SEEK_CURRENT_FRAME) + "\n" + SEEK_BACKWARD_TOOLTIP,
            "the current frame itself is not a target");
    model = BuildSeekBarModel(MakeSeekSnapshot(1600, 900, SeekScenario::Seeking));
    Require(BuildSeekBarTooltip(model, timeline.get(), 5'000) ==
                FormatSeekPointLabel(timeline.get(), 5'000) + "\n" + SEEK_BEFORE_TARGET_TOOLTIP,
            "between the position and the target the tooltip asks to cancel first");
    Require(BuildSeekBarTooltip(model, timeline.get(), 1'000).ends_with(SEEK_BACKWARD_TOOLTIP),
            "before the position it is still a backward seek");
    model = BuildSeekBarModel(MakeSeekSnapshot(1600, 900, SeekScenario::ReadOnly));
    Require(BuildSeekBarTooltip(model, timeline.get(), 1'000) == "00:33（帧 1000）", "a read-only bar gives no reason");

    // 可以向后跳转：当前位置之前的帧可点并说明需要重新载入，当前位置本身仍不可点（不再说明）；向前照旧。
    snapshot = MakeSeekSnapshot(1600, 900, SeekScenario::Seekable);
    snapshot.mPlaybackControls.mCanSeekBackward = true;
    model = BuildSeekBarModel(snapshot);
    Require(model.mMode == SeekBarMode::Interactive && model.mCanSeekBackward && model.mCanSeekForward,
            "a snapshot that can go back is interactive in both directions");
    Require(CanSeekToFrame(model, 0) && CanSeekToFrame(model, SEEK_CURRENT_FRAME - 1) &&
                !CanSeekToFrame(model, SEEK_CURRENT_FRAME) && CanSeekToFrame(model, SEEK_CURRENT_FRAME + 1),
            "frames before the position are targets, the position itself is not");
    Require(BuildSeekBarTooltip(model, timeline.get(), 1'000) == std::string("00:33（帧 1000）\n") + SEEK_RELOAD_TOOLTIP,
            "a backward target says it needs a reload");
    Require(BuildSeekBarTooltip(model, timeline.get(), SEEK_CURRENT_FRAME) ==
                FormatSeekPointLabel(timeline.get(), SEEK_CURRENT_FRAME),
            "the position itself gives no reason when going back is possible");
    Require(BuildSeekBarTooltip(model, timeline.get(), 4'000) == "02:12（帧 4000）", "a forward target still needs no reason");

    // 回放结束：不能向前，只能向后，进度条仍可交互。
    auto ended = snapshot;
    ended.mPlaybackControls.mStatus = rareplay::viewer::PlaybackStatus::Ended;
    ended.mPlaybackControls.mCanSeek = false;
    model = BuildSeekBarModel(ended);
    Require(model.mMode == SeekBarMode::Interactive && CanSeekToFrame(model, 1'000) &&
                !CanSeekToFrame(model, SEEK_CURRENT_FRAME + 1),
            "an ended replay can only go back");
    Require(BuildSeekBarTooltip(model, timeline.get(), 4'000) == "02:12（帧 4000）", "frames after the end give no reason");
    ended.mPlaybackControls.mCanSeekBackward = false;
    Require(BuildSeekBarModel(ended).mMode == SeekBarMode::ReadOnly, "without going back an ended replay stays read-only");

    // 快进中：当前位置之前可点（向后），当前位置到目标之间仍不可点。
    auto seekingBack = MakeSeekSnapshot(1600, 900, SeekScenario::Seeking);
    seekingBack.mPlaybackControls.mCanSeekBackward = true;
    model = BuildSeekBarModel(seekingBack);
    Require(CanSeekToFrame(model, 1'000) && !CanSeekToFrame(model, SEEK_CURRENT_FRAME) && !CanSeekToFrame(model, 5'000) &&
                CanSeekToFrame(model, SEEK_TARGET_FRAME + 1),
            "while seeking, frames before the position go back and frames after the target retarget");
    Require(BuildSeekBarTooltip(model, timeline.get(), 1'000).ends_with(SEEK_RELOAD_TOOLTIP) &&
                BuildSeekBarTooltip(model, timeline.get(), 5'000).ends_with(SEEK_BEFORE_TARGET_TOOLTIP),
            "while seeking the tooltips tell the reload from the blocked range");

    // 重开已请求、尚未执行：进度按 0，除待执行的目标外任何位置都可改作重开后的目标。
    auto reloading = seekingBack;
    reloading.mPlaybackControls.mSeek = {true, 0, 1'000, true};
    model = BuildSeekBarModel(reloading);
    Require(model.mIsSeeking && model.mIsReloading && model.mSeekPercent == 0 &&
                model.mSeekStatus == "正在跳转到 00:33 … 0%",
            "a pending restart shows 0%: " + model.mSeekStatus);
    Require(!CanSeekToFrame(model, 1'000) && CanSeekToFrame(model, 0) && CanSeekToFrame(model, 5'000) &&
                CanSeekToFrame(model, SEEK_CURRENT_FRAME) && CanSeekToFrame(model, SEEK_TOTAL_FRAMES - 1),
            "a pending restart can be retargeted anywhere but its own target");
    Require(BuildSeekBarTooltip(model, timeline.get(), 5'000).ends_with(SEEK_RELOAD_TOOLTIP) &&
                BuildSeekBarTooltip(model, timeline.get(), 1'000) == FormatSeekPointLabel(timeline.get(), 1'000),
            "a pending restart explains the reload except at its own target");
    ++g_SeekBarChecks;
}

// 可跳转：悬停与拖动不提交，松开时只提交一次；当前位置之前按下或松开都不提交。
void CheckSeekBarInteraction(HeadlessImGui& imgui)
{
    using rareplay::viewer::ViewerCommandKind;
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const UiRect footer{0.0f, 794.0f, 1432.0f, 900.0f};
    auto& exchange = imgui.GetExchange();
    const auto snapshot = MakeSeekSnapshot(1600, 900, SeekScenario::Seekable);
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    const auto rects = hud.GetSeekBarRects();
    Require(rects.mBar && rects.mIsInteractive && !rects.mIsOnOwnRow && !rects.mCancelButton,
            "a seekable snapshot draws an inline interactive bar without a cancel button");
    const UiRect bar = *rects.mBar;
    Require(IsInside(bar, footer) && bar.mTop > footer.mTop + 20.0f && bar.mBottom < footer.mBottom - 20.0f,
            "the bar sits on the second footer row");
    Require(ContainsRect(hud.GetInputRegions(), footer), "the footer keeps its height and input region");
    const float y = CenterY(bar);

    exchange.Drain();
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 7'000), y, 3);
    Require(exchange.Drain().empty(), "hovering the bar submits nothing");

    // 点击：松开时提交一次。
    const float clickX = GetBarX(bar, 7'000);
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, clickX, y);
    auto commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::SeekToFrame &&
                commands[0].mSessionId == snapshot.mSessionId &&
                commands[0].mValue == static_cast<int>(GetFrameAtX(bar, clickX)) &&
                std::abs(commands[0].mValue - 7'000) <= 15,
            "a click submits one seek to the clicked frame");

    // 拖动：途中不提交，松开在最后的位置提交一次。
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 5'000), y);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    for (const std::uint32_t frame : {6'000u, 9'000u, 4'500u, 8'000u})
    {
        MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, frame), y + 30.0f, 1);
        Require(exchange.Drain().empty(), "dragging submits nothing before the release");
    }
    const float releaseX = GetBarX(bar, 8'000);
    ReleasePointer();
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 2);
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::SeekToFrame &&
                commands[0].mValue == static_cast<int>(GetFrameAtX(bar, releaseX)),
            "a drag submits exactly one seek where it is released, even off the bar vertically");

    // 拖出右端松开：最后一帧。
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 6'000), y);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, bar.mRight + 200.0f, y, 1);
    ReleasePointer();
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 2);
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mValue == static_cast<int>(SEEK_TOTAL_FRAMES - 1),
            "releasing past the right end seeks to the last frame");

    // 当前位置之前：点击、从那里按下再拖到后面、从后面拖回来，都不提交。
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 1'000), y);
    Require(exchange.Drain().empty(), "a click before the current frame submits nothing");
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 1'000), y);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 8'000), y, 1);
    ReleasePointer();
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 2);
    Require(exchange.Drain().empty(), "a drag that starts before the current frame submits nothing");
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 8'000), y);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 1);
    MovePointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 1'000), y, 1);
    ReleasePointer();
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image, 2);
    Require(exchange.Drain().empty(), "a drag released before the current frame submits nothing");

    // 进度条在底栏内，点击被底栏拦截，不会漏给游戏。
    rareplay::render::OverlayInputQueue input;
    input.PublishPolicy(true, true, false, hud.GetInputRegions());
    Require(IsConsumedClick(input, static_cast<int>(GetBarX(bar, 7'000)), static_cast<int>(y)),
            "a click on the bar does not reach the game");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ++g_SeekBarChecks;
}

// 不可跳转：总帧数未知、能力标志为假、回放结束、快照未激活时保持只读，点击不提交。
void CheckSeekBarReadOnly(HeadlessImGui& imgui)
{
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const UiRect footer{0.0f, 794.0f, 1432.0f, 900.0f};
    auto& exchange = imgui.GetExchange();

    auto unknown = MakeSeekSnapshot(1600, 900, SeekScenario::Seekable);
    unknown.mTotalFrames.reset();
    unknown.mTimeline.reset();
    ReplayHud unknownHud;
    imgui.Draw(unknownHud, unknown, 1600.0f, 900.0f, image);
    Require(!unknownHud.GetSeekBarRects().mBar && !unknownHud.GetSeekBarRects().mCancelButton,
            "an unknown total draws no bar");
    Require(ContainsRect(unknownHud.GetInputRegions(), footer), "an unknown total keeps the footer height");

    struct ReadOnlyCase
    {
        const char* mName;
        void (*mChange)(ReplayViewSnapshot&);
    };
    const ReadOnlyCase cases[] = {
        {"no capability", [](ReplayViewSnapshot&) {}},
        {"ended", [](ReplayViewSnapshot& s) { s.mPlaybackControls.mStatus = rareplay::viewer::PlaybackStatus::Ended; }},
        {"inactive with capability", [](ReplayViewSnapshot& s) {
             s.mActive = false;
             s.mPlaybackControls.mCanSeek = true;
         }}};
    for (const ReadOnlyCase& item : cases)
    {
        auto snapshot = MakeSeekSnapshot(1600, 900, SeekScenario::ReadOnly);
        item.mChange(snapshot);
        ReplayHud hud;
        imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
        const auto rects = hud.GetSeekBarRects();
        const std::string name = item.mName;
        Require(rects.mBar && !rects.mIsInteractive && !rects.mCancelButton, name + ": a read-only bar is drawn");
        Require(IsInside(*rects.mBar, footer), name + ": the read-only bar stays in the footer");
        Require(ContainsRect(hud.GetInputRegions(), footer), name + ": the footer keeps its height");
        exchange.Drain();
        for (const std::uint32_t frame : {1'000u, 7'000u, 9'999u})
            ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(*rects.mBar, frame),
                         CenterY(*rects.mBar));
        Require(exchange.Drain().empty(), name + ": clicking a read-only bar submits nothing");
    }

    // 窄布局下只读进度条与原来一样不画，底栏不加高。
    imgui.SetScalePercent(150);
    const SizeScenario& narrow = SIZE_SCENARIOS[2];
    const GameImagePlacement narrowImage{{0.0f, 0.0f, narrow.mDisplayWidth, narrow.mDisplayHeight}, true};
    ReplayHud narrowHud;
    imgui.Draw(narrowHud, MakeSeekSnapshot(narrow.mCanvasWidth, narrow.mCanvasHeight, SeekScenario::ReadOnly),
               narrow.mDisplayWidth, narrow.mDisplayHeight, narrowImage);
    const UiRect panel = GetScaledPanel(narrow, 150);
    Require(!narrowHud.GetSeekBarRects().mBar, "a compact read-only footer draws no bar");
    Require(ContainsRect(narrowHud.GetInputRegions(),
                         {0.0f, narrow.mDisplayHeight - std::round(106.0f * 1.5f), panel.mLeft, narrow.mDisplayHeight}),
            "a compact read-only footer keeps its height");
    imgui.SetScalePercent(100);
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ++g_SeekBarChecks;
}

struct FirstRowButtons
{
    UiRect mPause;
    UiRect mRestart;
    UiRect mExit;
};

// 底栏第一行三个按钮的位置：100% 档窗口内边距 12、10，按当前字体与 HUD 样式（Draw 之后保留在上下文中）计算。
FirstRowButtons MeasureFirstRowButtons(const UiRect& footer)
{
    ImGui::NewFrame();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float top = footer.mTop + 10.0f;
    const auto width = [&style](const char* text) { return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f; };
    FirstRowButtons buttons;
    buttons.mPause = {footer.mLeft + 12.0f, top, footer.mLeft + 12.0f + width("暂停"), top + height};
    const float restartLeft = buttons.mPause.mRight + style.ItemSpacing.x;
    buttons.mRestart = {restartLeft, top, restartLeft + width("重新播放"), top + height};
    const float exitLeft = buttons.mRestart.mRight + style.ItemSpacing.x;
    buttons.mExit = {exitLeft, top, exitLeft + width("退出回放"), top + height};
    ImGui::EndFrame();
    return buttons;
}

const ImGuiWindow* FindActiveWindow(const char* name)
{
    for (ImGuiWindow* candidate : ImGui::GetCurrentContext()->Windows)
    {
        if (candidate->Active && std::strcmp(candidate->Name, name) == 0)
            return candidate;
    }
    return nullptr;
}

// “快进中”标签：只在快进中绘制；在地图区（面板左缘、底栏上缘以内）正中、不越界，文字在内边距以内；
// 记入可见区域（遮住原生光标时画 HUD 光标）但不拦截输入。mapArea 为主视口客户区坐标。
void CheckSeekingLabel(const ReplayHud& hud, const std::string& name, bool isSeeking, const UiRect& mapArea)
{
    const auto& label = hud.GetSeekingLabelRect();
    const ImGuiWindow* window = FindActiveWindow("快进中##ReplaySeekingLabel");
    Require(label.has_value() == isSeeking && (window != nullptr) == isSeeking,
            name + ": the seeking label is drawn only while seeking");
    if (!label)
        return;
    Require(IsInside(*label, mapArea),
            name + ": the seeking label stays in the map area, right " + std::to_string(label->mRight) + " of " +
                std::to_string(mapArea.mRight) + ", bottom " + std::to_string(label->mBottom) + " of " +
                std::to_string(mapArea.mBottom));
    Require(std::abs(CenterX(*label) - CenterX(mapArea)) <= 1.0f && std::abs(CenterY(*label) - CenterY(mapArea)) <= 1.0f,
            name + ": the seeking label is centered in the map area");
    Require(SameRect(*label, {window->Pos.x, window->Pos.y, window->Pos.x + window->Size.x, window->Pos.y + window->Size.y}),
            name + ": the label rect is the window rect");
    Require(window->DC.CursorMaxPos.x <= window->Pos.x + window->Size.x - window->WindowPadding.x + 0.5f &&
                window->DC.CursorMaxPos.y <= window->Pos.y + window->Size.y - window->WindowPadding.y + 0.5f,
            name + ": the label text fits inside the padding");
    Require(ContainsRect(hud.GetVisibleRegions(), *label) && !ContainsRect(hud.GetInputRegions(), *label),
            name + ": the label is visible but does not take input");
}

// 可以向后跳转：点在当前位置之前提交一次跳转（由游戏线程按向后跳转处理）；“重新播放”不再要求启动器支持。
void CheckBackwardSeekBar(HeadlessImGui& imgui)
{
    using rareplay::viewer::ViewerCommandKind;
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const UiRect footer{0.0f, 794.0f, 1432.0f, 900.0f};
    auto& exchange = imgui.GetExchange();
    auto snapshot = MakeSeekSnapshot(1600, 900, SeekScenario::Seekable);
    snapshot.mPlaybackControls.mCanSeekBackward = true;
    ReplayHud hud;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    const FirstRowButtons buttons = MeasureFirstRowButtons(footer);
    const UiRect bar = *hud.GetSeekBarRects().mBar;
    const float y = CenterY(bar);
    exchange.Drain();

    const float backX = GetBarX(bar, 1'000);
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, backX, y);
    auto commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::SeekToFrame &&
                commands[0].mValue == static_cast<int>(GetFrameAtX(bar, backX)) &&
                commands[0].mValue < static_cast<int>(SEEK_CURRENT_FRAME),
            "a click before the position submits one backward seek");
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, GetBarX(bar, 0), y);
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mValue == 0, "the left end seeks back to frame 0");

    // 启动器不支持重新播放，但进程内重开可用：按钮可用；两者都不可用时按钮禁用。
    snapshot.mPlaybackControls.mCanRestart = false;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, CenterX(buttons.mRestart), CenterY(buttons.mRestart));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::RestartPlayback,
            "restart works in process without launcher support");
    snapshot.mPlaybackControls.mCanSeekBackward = false;
    imgui.Draw(hud, snapshot, 1600.0f, 900.0f, image);
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, CenterX(buttons.mRestart), CenterY(buttons.mRestart));
    ClickPointer(hud, imgui, snapshot, 1600.0f, 900.0f, image, backX, y);
    Require(exchange.Drain().empty(), "without either path restart and going back are disabled");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ++g_SeekBarChecks;
}

// 快进中：状态文字与“取消”；暂停、速度、半黑幕与缩放禁用；退出与重新播放可用；进度条只接受目标之后的位置。
void CheckSeekingFooter(HeadlessImGui& imgui)
{
    using rareplay::viewer::ViewerCommandKind;
    imgui.SetScalePercent(100);
    const GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const UiRect footer{0.0f, 794.0f, 1432.0f, 900.0f};
    auto& exchange = imgui.GetExchange();

    // 先在可跳转（不在快进中）的状态下核对各控件的位置确实能点到（正对照）。
    const auto idle = MakeSeekSnapshot(1600, 900, SeekScenario::Seekable);
    ReplayHud idleHud;
    imgui.Draw(idleHud, idle, 1600.0f, 900.0f, image);
    const FirstRowButtons buttons = MeasureFirstRowButtons(footer);
    const UiRect idleBar = *idleHud.GetSeekBarRects().mBar;
    const UiRect zoomGroup = idleHud.GetZoomControlRects()->mGroup;
    const UiRect zoomIn = idleHud.GetZoomControlRects()->mZoomIn;
    const float shroudX = footer.mLeft + 12.0f + 8.0f;
    const float shroudY = CenterY(zoomGroup);
    const float comboX = idleBar.mRight + 40.0f;
    exchange.Drain();
    ClickPointer(idleHud, imgui, idle, 1600.0f, 900.0f, image, CenterX(buttons.mPause), CenterY(buttons.mPause));
    auto commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::SetPaused,
            "control: the measured pause button pauses");
    ClickPointer(idleHud, imgui, idle, 1600.0f, 900.0f, image, shroudX, shroudY);
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::SetHalfShroud,
            "control: the half shroud checkbox toggles");
    ClickPointer(idleHud, imgui, idle, 1600.0f, 900.0f, image, CenterX(zoomIn), CenterY(zoomIn));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::AdjustBattlefieldZoomAtCenter,
            "control: the zoom button zooms");
    const std::size_t idleRegions = idleHud.GetInputRegions().size();
    ClickPointer(idleHud, imgui, idle, 1600.0f, 900.0f, image, comboX, CenterY(idleBar));
    Require(idleHud.GetInputRegions().size() == idleRegions + 1, "control: the speed combo opens its popup");
    // 在地图空白处点击，关闭下拉框。
    ClickPointer(idleHud, imgui, idle, 1600.0f, 900.0f, image, 700.0f, 300.0f);
    Require(idleHud.GetInputRegions().size() == idleRegions, "control: the popup closes");
    exchange.Drain();

    Require(!idleHud.GetSeekingLabelRect(), "no seeking label outside a seek");

    const auto seeking = MakeSeekSnapshot(1600, 900, SeekScenario::Seeking);
    ReplayHud hud;
    imgui.Draw(hud, seeking, 1600.0f, 900.0f, image);
    CheckSeekingLabel(hud, "1600x900 seeking", true, {0.0f, 0.0f, footer.mRight, footer.mTop});
    // 标签不拦截输入：点在它正中的鼠标交给游戏，不产生观看命令。
    {
        rareplay::render::OverlayInputQueue input;
        input.PublishPolicy(true, true, false, hud.GetInputRegions());
        const UiRect label = *hud.GetSeekingLabelRect();
        Require(!IsConsumedClick(input, static_cast<int>(CenterX(label)), static_cast<int>(CenterY(label))),
                "a click on the seeking label reaches the game");
    }
    const auto rects = hud.GetSeekBarRects();
    Require(rects.mBar && rects.mIsInteractive && rects.mCancelButton, "seeking shows the bar and a cancel button");
    Require(IsInside(*rects.mCancelButton, footer) && IsInside(*rects.mBar, footer) &&
                rects.mCancelButton->mRight <= rects.mBar->mLeft,
            "the cancel button precedes the bar on the second row");
    Require(ContainsRect(hud.GetInputRegions(), footer), "seeking keeps the footer height");
    const UiRect bar = *rects.mBar;

    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, CenterX(*rects.mCancelButton), CenterY(*rects.mCancelButton));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::CancelSeek &&
                commands[0].mSessionId == seeking.mSessionId,
            "取消 submits one cancel");

    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, CenterX(buttons.mPause), CenterY(buttons.mPause));
    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, shroudX, shroudY);
    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, CenterX(zoomIn), CenterY(zoomIn));
    const std::size_t seekingRegions = hud.GetInputRegions().size();
    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, comboX, CenterY(bar));
    Require(hud.GetInputRegions().size() == seekingRegions, "the speed combo does not open while seeking");
    Require(exchange.Drain().empty(), "pause, half shroud, zoom and speed are disabled while seeking");

    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, CenterX(buttons.mExit), CenterY(buttons.mExit));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::ExitPlayback, "exit stays available");
    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, CenterX(buttons.mRestart), CenterY(buttons.mRestart));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::RestartPlayback, "restart stays available");

    // 当前位置与目标之间不可点；目标之后可以改目标。
    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, GetBarX(bar, 5'000), CenterY(bar));
    Require(exchange.Drain().empty(), "a click between the position and the target submits nothing");
    const float laterX = GetBarX(bar, 8'500);
    ClickPointer(hud, imgui, seeking, 1600.0f, 900.0f, image, laterX, CenterY(bar));
    commands = exchange.Drain();
    Require(commands.size() == 1 && commands[0].mKind == ViewerCommandKind::SeekToFrame &&
                commands[0].mValue == static_cast<int>(GetFrameAtX(bar, laterX)),
            "a click after the target submits a later target");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ++g_SeekBarChecks;
}

// 各档位、各尺寸、三种状态：底栏内容竖直方向放得下，进度条与“取消”在内容区内；窄布局下可交互的进度条单独一行并加高底栏。
void CheckSeekBarLayouts(HeadlessImGui& imgui)
{
    constexpr char FOOTER_WINDOW_NAME[] = "播放状态##ReplayFooter";
    for (const int percent : SCALE_PERCENTS)
    {
        imgui.SetScalePercent(percent);
        const float scale = static_cast<float>(percent) / 100.0f;
        const float padX = std::round(12.0f * scale);
        const float padY = std::round(10.0f * scale);
        for (const SizeScenario& scenario : SIZE_SCENARIOS)
        {
            const GameImagePlacement image{{0.0f, 0.0f, scenario.mDisplayWidth, scenario.mDisplayHeight}, true};
            const UiRect panel = GetScaledPanel(scenario, percent);
            const bool isCompact = panel.mLeft - 2.0f * padX < 730.0f * scale;
            for (const SeekScenario state : {SeekScenario::ReadOnly, SeekScenario::Seekable, SeekScenario::Seeking})
            {
                const std::string name = Describe(scenario, percent) + " " + Describe(state);
                ReplayHud hud;
                imgui.Draw(hud, MakeSeekSnapshot(scenario.mCanvasWidth, scenario.mCanvasHeight, state),
                           scenario.mDisplayWidth, scenario.mDisplayHeight, image);
                const ImGuiWindow* window = nullptr;
                for (ImGuiWindow* candidate : ImGui::GetCurrentContext()->Windows)
                {
                    if (candidate->Active && std::strcmp(candidate->Name, FOOTER_WINDOW_NAME) == 0)
                        window = candidate;
                }
                Require(window != nullptr, name + ": the footer is drawn");
                const float footerTop = window->Pos.y;
                const float footerBottom = window->Pos.y + window->Size.y;
                Require(window->DC.CursorMaxPos.y <= footerBottom - padY + 0.5f,
                        name + ": the footer rows fit, bottom " + std::to_string(window->DC.CursorMaxPos.y) + " of " +
                            std::to_string(footerBottom - padY));
                const auto& rects = hud.GetSeekBarRects();
                const bool hasOwnRow = isCompact && state != SeekScenario::ReadOnly;
                const float baseHeight = std::round(106.0f * scale);
                Require(hasOwnRow ? footerBottom - footerTop > baseHeight + 0.5f
                                  : std::abs(footerBottom - footerTop - baseHeight) < 0.5f,
                        name + ": the footer grows only for a compact interactive bar");
                const UiRect content{padX, footerTop, panel.mLeft - padX, footerBottom};
                if (state == SeekScenario::ReadOnly && isCompact)
                {
                    Require(!rects.mBar, name + ": compact read-only draws no bar");
                }
                else
                {
                    Require(rects.mBar && rects.mIsOnOwnRow == hasOwnRow &&
                                rects.mIsInteractive == (state != SeekScenario::ReadOnly),
                            name + ": bar mode and row");
                    Require(IsInside(*rects.mBar, content),
                            name + ": the bar stays in the footer content, right " + std::to_string(rects.mBar->mRight) +
                                " of " + std::to_string(content.mRight));
                    if (hasOwnRow)
                        Require(rects.mBar->mRight - rects.mBar->mLeft >= content.mRight - content.mLeft - 1.0f,
                                name + ": the own-row bar spans the content width");
                }
                Require(rects.mCancelButton.has_value() == (state == SeekScenario::Seeking),
                        name + ": cancel only while seeking");
                if (rects.mCancelButton)
                    Require(IsInside(*rects.mCancelButton, content), name + ": the cancel button is not clipped");
                CheckSeekingLabel(hud, name, state == SeekScenario::Seeking, {0.0f, 0.0f, panel.mLeft, footerTop});
                const auto zoom = hud.GetZoomControlRects();
                Require(zoom && zoom->mGroup.mTop >= (rects.mBar ? rects.mBar->mBottom : footerTop),
                        name + ": the zoom row stays below the bar");
                ++g_SeekBarChecks;
            }
        }
    }
    imgui.SetScalePercent(100);
}
}

int main()
{
    try
    {
        HeadlessImGui imgui;
        CheckPanelInputRegion(imgui);
        CheckPresenceReport(imgui);
        CheckTabsFitPanel(imgui);
        CheckStatsPagesFit(imgui);
        CheckEconomyHover(imgui);
        CheckFooterCoversCommandBar(imgui);
        CheckUnitGrid(imgui);
        CheckEconomyWindowLayoutAndRegions(imgui);
        CheckEconomyWindowInputRegions(imgui);
        CheckEconomyWindowDragAndClose(imgui);
        CheckEconomyToggleButton(imgui);
        CheckEconomyFilterThroughUi(imgui);
        CheckEconomyWindowHover(imgui);
        CheckEconomyVertexBudget(imgui);
        CheckEconomyWindowSessionState(imgui);
        CheckRadarBarLayoutAndRegions(imgui);
        CheckRadarBarInputRegions(imgui);
        CheckRadarBarInteraction(imgui);
        CheckRadarBarHoverPause(imgui);
        CheckRadarBarVersusOtherWindows(imgui);
        CheckSuperWeaponMarks(imgui);
        CheckCardStyle(imgui);
        CheckZoomControlModel();
        CheckBattlefieldZoomControls(imgui);
        CheckSeekBarModel();
        CheckSeekBarInteraction(imgui);
        CheckSeekBarReadOnly(imgui);
        CheckBackwardSeekBar(imgui);
        CheckSeekingFooter(imgui);
        CheckSeekBarLayouts(imgui);
        std::cout << "Replay HUD panel tests passed (statistics pages: " << g_StatsPageChecks
                  << ", economy hover frames: " << g_EconomyHoverChecks
                  << ", economy window layouts: " << g_EconomyWindowChecks
                  << ", radar bar layouts: " << g_RadarBarChecks
                  << ", zoom control layouts: " << g_ZoomControlChecks
                  << ", seek bar checks: " << g_SeekBarChecks << ")\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
