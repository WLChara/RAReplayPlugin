#include "ReplayHudDemoData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rareplay::tools
{
namespace
{
using Rgba = std::array<std::uint8_t, 4>;

void SetPixel(viewer::IconImage& image, int x, int y, const Rgba& color)
{
    if (x < 0 || y < 0 || x >= image.mWidth || y >= image.mHeight)
    {
        return;
    }
    const std::size_t offset = (static_cast<std::size_t>(y) * image.mWidth + x) * 4;
    std::copy(color.begin(), color.end(), image.mRgba.begin() + offset);
}

void FillRect(viewer::IconImage& image, int left, int top, int right, int bottom, const Rgba& color)
{
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            SetPixel(image, x, y, color);
        }
    }
}

std::shared_ptr<const viewer::IconImage> CreateNeutralIcon(const std::string& key, viewer::UnitCategory category,
                                                           const Rgba& accent)
{
    // 与原生 cameo 相同的 60×48 画布，用来检查卡片按原始比例显示图标；图形画在中间 36×36 区域。
    auto image = std::make_shared<viewer::IconImage>();
    image->mKey = key;
    image->mWidth = 60;
    image->mHeight = 48;
    image->mRgba.resize(static_cast<std::size_t>(image->mWidth * image->mHeight * 4));
    const Rgba background{22, 30, 27, 255};
    const Rgba border{100, 118, 104, 255};
    const Rgba light{224, 213, 178, 255};
    for (int y = 0; y < image->mHeight; ++y)
    {
        for (int x = 0; x < image->mWidth; ++x)
        {
            SetPixel(*image, x, y, background);
        }
    }
    FillRect(*image, 1, 1, 59, 2, border);
    FillRect(*image, 1, 46, 59, 47, border);
    FillRect(*image, 1, 2, 2, 46, border);
    FillRect(*image, 58, 2, 59, 46, border);

    constexpr int OFFSET_X = 12;
    constexpr int OFFSET_Y = 6;
    const auto fill = [&image](int left, int top, int right, int bottom, const Rgba& color)
    {
        FillRect(*image, left + OFFSET_X, top + OFFSET_Y, right + OFFSET_X, bottom + OFFSET_Y, color);
    };
    switch (category)
    {
    case viewer::UnitCategory::Infantry:
        fill(15, 7, 21, 14, light);
        fill(11, 15, 25, 27, accent);
        fill(9, 27, 16, 31, light);
        fill(20, 27, 27, 31, light);
        break;
    case viewer::UnitCategory::Vehicle:
        fill(7, 14, 29, 25, accent);
        fill(12, 9, 24, 14, light);
        fill(9, 25, 15, 30, light);
        fill(22, 25, 28, 30, light);
        break;
    case viewer::UnitCategory::Aircraft:
        fill(16, 5, 20, 30, light);
        fill(5, 13, 31, 18, accent);
        fill(11, 23, 25, 27, accent);
        break;
    case viewer::UnitCategory::Building:
        fill(7, 12, 29, 29, accent);
        fill(12, 17, 16, 22, light);
        fill(20, 17, 24, 22, light);
        fill(15, 24, 21, 29, light);
        break;
    default:
        fill(9, 9, 27, 27, accent);
        fill(14, 14, 22, 22, light);
        break;
    }
    return image;
}

// 示例资金曲线：1500 个点（发布上限）铺满 durationMs，终点等于 finalCredits（与总览里的资金一致）。
// 走势由缓升的基线与一个随时间衰减的波动叠加，defeatedAt 之后资金归零（示例中的战败玩家）。
std::shared_ptr<const viewer::EconomyCurve> MakeCurve(std::int64_t durationMs, std::int64_t finalCredits,
                                                     double wavePeriod, double phase,
                                                     std::optional<std::int64_t> defeatedAtMs = std::nullopt)
{
    constexpr int POINTS = 1500;
    auto curve = std::make_shared<viewer::EconomyCurve>();
    curve->mPoints.reserve(POINTS);
    for (int index = 0; index < POINTS; ++index)
    {
        const double progress = static_cast<double>(index) / (POINTS - 1);
        const std::int64_t timeMs = static_cast<std::int64_t>(progress * static_cast<double>(durationMs));
        double credits = static_cast<double>(finalCredits) * (0.30 + 0.70 * progress) +
                         static_cast<double>(finalCredits) * 0.55 * std::sin(index * wavePeriod + phase) * (1.0 - progress);
        if (defeatedAtMs && timeMs >= *defeatedAtMs)
            credits = 0.0;
        curve->mPoints.push_back({timeMs, std::max<std::int64_t>(0, static_cast<std::int64_t>(credits))});
    }
    return curve;
}

std::shared_ptr<const viewer::StallHistory> MakeHistory(std::initializer_list<viewer::StallRecord> records)
{
    return std::make_shared<const viewer::StallHistory>(records);
}

// 总停产时间 = 历史区间之和 + 进行中且已满 2 秒的停产；停产率 = 总停产时间 ÷ 当前游戏时间（口径同统计层）。
viewer::ProductionLineStatus MakeLineStatus(viewer::ProductionLineState state, std::int64_t gameTimeMs,
                                            std::shared_ptr<const viewer::StallHistory> history = nullptr,
                                            std::int64_t stalledSinceMs = 0, bool stallHadOutage = false)
{
    viewer::ProductionLineStatus status;
    status.mState = state;
    status.mStalledSinceMs = stalledSinceMs;
    status.mStallHadPowerOutage = stallHadOutage;
    status.mHistory = std::move(history);
    if (status.mHistory)
    {
        for (const viewer::StallRecord& record : *status.mHistory)
            status.mTotalStalledMs += record.mEndMs - record.mStartMs;
    }
    if (state == viewer::ProductionLineState::Stalled && gameTimeMs - stalledSinceMs >= 2000)
        status.mTotalStalledMs += gameTimeMs - stalledSinceMs;
    if (gameTimeMs > 0)
        status.mStallRatePercent = static_cast<float>(static_cast<double>(status.mTotalStalledMs) * 100.0 /
                                                      static_cast<double>(gameTimeMs));
    return status;
}

viewer::UnitSummary MakeUnit(std::string typeId, std::string name, viewer::UnitCategory category,
                             std::uint32_t count, const std::shared_ptr<const viewer::IconImage>& icon)
{
    viewer::UnitSummary unit;
    unit.mTypeId = std::move(typeId);
    unit.mName = std::move(name);
    unit.mCategory = category;
    unit.mCount = count;
    unit.mIcon = icon;
    return unit;
}

viewer::ProductionLine MakeLine(std::uint32_t id, std::string label, std::string productName,
                                const std::shared_ptr<const viewer::IconImage>& icon, float progress,
                                std::uint32_t queuedCount = 0, bool ready = false, bool onHold = false)
{
    viewer::ProductionLine line;
    line.mId = id;
    line.mLabel = std::move(label);
    line.mProductName = std::move(productName);
    line.mIcon = icon;
    line.mProgress = progress;
    line.mQueuedCount = queuedCount;
    line.mReady = ready;
    line.mOnHold = onHold;
    return line;
}

enum class DemoWeaponGlyph { Storm, Nuke, Chrono };

// 60×48 的示例超武图标（与原生侧栏图标同尺寸）：深色底、浅色边框、按类别画的简单图形；不是游戏素材。
std::shared_ptr<const viewer::IconImage> CreateSuperWeaponIcon(const std::string& key, DemoWeaponGlyph glyph,
                                                               const Rgba& accent)
{
    auto image = std::make_shared<viewer::IconImage>();
    image->mKey = key;
    image->mWidth = 60;
    image->mHeight = 48;
    image->mRgba.assign(static_cast<std::size_t>(image->mWidth * image->mHeight * 4), 0);
    const Rgba background{26, 22, 38, 255};
    const Rgba border{150, 140, 190, 255};
    const Rgba light{250, 244, 214, 255};
    FillRect(*image, 0, 0, 60, 48, background);
    FillRect(*image, 0, 0, 60, 1, border);
    FillRect(*image, 0, 47, 60, 48, border);
    FillRect(*image, 0, 0, 1, 48, border);
    FillRect(*image, 59, 0, 60, 48, border);
    for (int y = 3; y < 45; ++y)
    {
        for (int x = 3; x < 57; ++x)
        {
            const double dx = x - 29.5;
            const double dy = y - 23.5;
            const double distance = std::sqrt(dx * dx + dy * dy);
            bool lit = false;
            Rgba color = accent;
            switch (glyph)
            {
            case DemoWeaponGlyph::Storm:
                // 锯齿闪电：随 y 在两个 x 位置之间折返。
                lit = std::abs(x - (y < 24 ? 35 - (y - 4) * 0.5 : 24 + (y - 24) * 0.6)) < 3.5 && y > 4 && y < 44;
                color = y % 9 < 3 ? light : accent;
                break;
            case DemoWeaponGlyph::Nuke:
                // 圆形火球加放射线。
                lit = distance < 11.0 || (distance < 20.0 && (static_cast<int>(std::atan2(dy, dx) * 8.0 / 3.14159 + 16.0) % 2 == 0));
                color = distance < 6.0 ? light : accent;
                break;
            case DemoWeaponGlyph::Chrono:
                // 同心圆环。
                lit = std::abs(distance - 9.0) < 1.8 || std::abs(distance - 17.0) < 1.8;
                color = distance < 12.0 ? light : accent;
                break;
            }
            if (lit)
                SetPixel(*image, x, y, color);
        }
    }
    return image;
}

viewer::RadarEvent MakeDemoEvent(std::uint64_t id, viewer::RadarEventKind kind, std::int64_t timeMs,
                                 viewer::RadarPlayerRef actor, viewer::RadarPlayerRef victim, std::string subject,
                                 std::shared_ptr<const viewer::IconImage> icon, std::optional<viewer::WorldPoint> where)
{
    viewer::RadarEvent event;
    event.mId = id;
    event.mKind = kind;
    event.mGameTimeMs = timeMs;
    event.mActor = std::move(actor);
    event.mVictim = std::move(victim);
    event.mSubjectName = std::move(subject);
    event.mIcon = std::move(icon);
    event.mLocation = where;
    return event;
}

const viewer::RadarPlayerRef DEMO_NORTH{1, "北岸指挥部", 0x4E9A78};
const viewer::RadarPlayerRef DEMO_AI{2, "电脑对手", 0xB45044};
const viewer::RadarPlayerRef DEMO_EAST{3, "东部基地", 0x8B68A8};
}

std::shared_ptr<const viewer::RadarEventList> CreateReplayHudDemoRadarEvents(std::int64_t gameTimeMs)
{
    const auto storm = CreateSuperWeaponIcon("preview-sw-storm", DemoWeaponGlyph::Storm, {138, 196, 255, 255});
    const auto nuke = CreateSuperWeaponIcon("preview-sw-nuke", DemoWeaponGlyph::Nuke, {255, 150, 70, 255});
    const auto chrono = CreateSuperWeaponIcon("preview-sw-chrono", DemoWeaponGlyph::Chrono, {120, 220, 200, 255});
    const viewer::RadarPlayerRef neutral{-1, "中立", 0x8A8F86};
    const viewer::WorldPoint where{128 + 256 * 40, 128 + 256 * 52, 0};
    const auto launch = viewer::RadarEventKind::SuperWeaponLaunched;
    const auto spy = viewer::RadarEventKind::BuildingInfiltrated;

    // 按发生顺序（最旧的在前）；年龄 = 当前游戏时间 − 事件时间。前 7 条较旧（历史列表演示），后 5 条在 30 秒内。
    auto list = std::make_shared<viewer::RadarEventList>();
    std::uint64_t id = 1;
    const auto add = [&](std::int64_t ageMs, viewer::RadarEventKind kind, const viewer::RadarPlayerRef& actor,
                         const viewer::RadarPlayerRef& victim, const char* subject,
                         const std::shared_ptr<const viewer::IconImage>& icon, bool hasLocation = true)
    {
        list->mEvents.push_back(MakeDemoEvent(id++, kind, gameTimeMs - ageMs, actor, victim, subject, icon,
            hasLocation ? std::optional<viewer::WorldPoint>(where) : std::nullopt));
    };
    add(1'210'000, launch, DEMO_AI, {}, "闪电风暴", storm);
    add(980'000, spy, DEMO_EAST, DEMO_NORTH, "兵营", nullptr);
    add(760'000, launch, DEMO_NORTH, {}, "核弹", nuke);
    add(600'000, spy, DEMO_AI, DEMO_NORTH, "战车工厂", nullptr);
    add(420'000, launch, DEMO_NORTH, {}, "超时空传送", chrono);
    add(300'000, spy, DEMO_EAST, neutral, "科技医院", nullptr);
    add(95'000, launch, DEMO_AI, {}, "铁幕装置（示例长名称）", nullptr);
    // 最近 30 秒：最新的在末尾。
    add(27'500, spy, DEMO_EAST, neutral, "科技机场", nullptr);
    add(23'000, launch, DEMO_NORTH, {}, "超时空传送", chrono);
    add(16'800, launch, DEMO_AI, {}, "核弹", nuke);
    add(9'400, spy, DEMO_AI, DEMO_NORTH, "兵营", nullptr);
    add(3'900, launch, DEMO_NORTH, {}, "闪电风暴", storm);
    return list;
}

std::shared_ptr<const viewer::SuperWeaponMarkFrame> CreateReplayHudDemoMarks(const ReplayHudDemoOptions& options,
    std::int64_t gameTimeMs)
{
    const auto storm = CreateSuperWeaponIcon("preview-sw-storm", DemoWeaponGlyph::Storm, {138, 196, 255, 255});
    const auto nuke = CreateSuperWeaponIcon("preview-sw-nuke", DemoWeaponGlyph::Nuke, {255, 150, 70, 255});
    const auto chrono = CreateSuperWeaponIcon("preview-sw-chrono", DemoWeaponGlyph::Chrono, {120, 220, 200, 255});
    auto frame = std::make_shared<viewer::SuperWeaponMarkFrame>();
    frame->mSessionId = 1;
    frame->mGameTimeMs = gameTimeMs;
    frame->mCanvasWidth = options.mCanvasWidth;
    frame->mCanvasHeight = options.mCanvasHeight;
    const int viewWidth = options.mCanvasWidth - 168;
    const int viewHeight = options.mCanvasHeight - 32;
    frame->mView = {0, 0, viewWidth, viewHeight};
    const auto add = [&](std::uint64_t id, std::int64_t ageMs, std::uint32_t color, const char* label,
                         const std::shared_ptr<const viewer::IconImage>& icon, double fractionX, double fractionY)
    {
        auto info = std::make_shared<viewer::SuperWeaponMarkInfo>();
        info->mId = id;
        info->mReleasedAtMs = gameTimeMs - ageMs;
        info->mOwnerColorRgb = color;
        info->mLabel = label;
        info->mIcon = icon;
        frame->mMarks.push_back({info, {static_cast<int>(fractionX * viewWidth), static_cast<int>(fractionY * viewHeight)}});
    };
    // 按释放先后（最旧的在前）：一个正在淡出的无图标标记、三个不同图标、一对重叠的落点、一个视图之外的落点。
    add(1, 13'800, 0xB45044, "铁幕装置", nullptr, 0.30, 0.22);
    add(2, 11'000, 0x8B68A8, "超时空传送", chrono, 0.72, 0.30);
    add(3, 7'200, 0xB45044, "核弹", nuke, 0.52, 0.58);
    add(4, 4'300, 0x4E9A78, "超时空传送", chrono, 0.72, 0.31);
    add(5, 2'400, 0x4E9A78, "闪电风暴", storm, 0.20, 0.52);
    add(6, 1'200, 0xB45044, "核弹", nuke, -0.15, 0.40);
    return frame;
}

std::shared_ptr<viewer::ReplayViewSnapshot> CreateReplayHudDemoSnapshot(const ReplayHudDemoOptions& options)
{
    const auto infantryIcon = CreateNeutralIcon("preview-infantry", viewer::UnitCategory::Infantry, {172, 169, 120, 255});
    const auto vehicleIcon = CreateNeutralIcon("preview-vehicle", viewer::UnitCategory::Vehicle, {167, 127, 70, 255});
    const auto aircraftIcon = CreateNeutralIcon("preview-aircraft", viewer::UnitCategory::Aircraft, {112, 157, 163, 255});
    const auto buildingIcon = CreateNeutralIcon("preview-building", viewer::UnitCategory::Building, {116, 143, 98, 255});

    auto snapshot = std::make_shared<viewer::ReplayViewSnapshot>();
    snapshot->mSessionId = 1;
    snapshot->mActive = true;
    snapshot->mIsPreview = true;
    snapshot->mReplayName = "示例回放_01.yrreplay";
    snapshot->mMapName = "Crossroads · 示例地图";
    snapshot->mMapHash = "preview-only";
    snapshot->mStartedUtc = 1'767'226'800;
    snapshot->mWasNetworkGame = true;
    snapshot->mFrameNumber = 28'540;
    snapshot->mPlaybackControls.mLastCompletedFrame = snapshot->mFrameNumber;
    snapshot->mPlaybackControls.mCanRestart = true;
    snapshot->mPlaybackControls.mIsHalfShroudEnabled = true;
    snapshot->mTotalFrames = 64'800;
    snapshot->mRecordedLocalHouseIndex = 1;
    snapshot->mPlaybackSpeedSetting = 1;
    snapshot->mCanChangePlaybackSpeed = true;
    // 示例战局缩放：已缩小到 50%，两个方向都还能继续。
    snapshot->mBattlefieldZoom.mIsAvailable = true;
    snapshot->mBattlefieldZoom.mIsActive = true;
    snapshot->mBattlefieldZoom.mPercent = 50;
    snapshot->mBattlefieldZoom.mCanZoomIn = true;
    snapshot->mBattlefieldZoom.mCanZoomOut = true;
    snapshot->mHasPlayerVision = false;
    snapshot->mHasRecordedGroups = true;
    // 示例画布按原生 SidebarBounds 规则推出面板区：{画布宽 - 168, 158, 168, 画布高 - 158}；
    // 不是假装从运行中游戏取得的数据。
    constexpr int NATIVE_SIDEBAR_WIDTH = 168;
    constexpr int NATIVE_SIDEBAR_TOP = 158;
    // 原生命令栏按同一规则为画面底部 32 行、宽同战术区：{0, 画布高 - 32, 画布宽 - 168, 32}。
    constexpr int NATIVE_COMMAND_BAR_HEIGHT = 32;
    snapshot->mNativeUiLayout = viewer::NativeUiLayout{options.mCanvasWidth, options.mCanvasHeight,
        {options.mCanvasWidth - NATIVE_SIDEBAR_WIDTH, NATIVE_SIDEBAR_TOP, NATIVE_SIDEBAR_WIDTH,
         options.mCanvasHeight - NATIVE_SIDEBAR_TOP},
        viewer::NativeRect{0, options.mCanvasHeight - NATIVE_COMMAND_BAR_HEIGHT,
                           options.mCanvasWidth - NATIVE_SIDEBAR_WIDTH, NATIVE_COMMAND_BAR_HEIGHT}};
    snapshot->mIsNativeRadarAvailable = true;

    viewer::PlayerSummary localPlayer;
    localPlayer.mHouseIndex = 1;
    localPlayer.mName = "北岸指挥部";
    localPlayer.mFaction = "盟军";
    localPlayer.mColorRgb = 0x4E9A78;
    localPlayer.mIsHuman = true;
    localPlayer.mCredits = 6'450;
    localPlayer.mPowerOutput = 1'480;
    localPlayer.mPowerDrain = 1'120;
    localPlayer.mHasProductionData = true;
    localPlayer.mHasUnitData = true;
    localPlayer.mProductionLines = {
        MakeLine(1, "战车工厂", "灰熊坦克", vehicleIcon, 0.72f, 2),
        MakeLine(2, "兵营", "火箭飞行兵", infantryIcon, 1.0f, 0, true),
        MakeLine(3, "空军指挥部", "入侵者战机", aircraftIcon, 0.18f, 1, false, true)};
    // 类别与数量足够多，用于检查两列卡片、类别子页签和长名称截断。
    localPlayer.mUnits = {
        MakeUnit("E1", "美国大兵", viewer::UnitCategory::Infantry, 18, infantryIcon),
        MakeUnit("ENGINEER", "工程师", viewer::UnitCategory::Infantry, 2, infantryIcon),
        MakeUnit("ADOG", "军犬", viewer::UnitCategory::Infantry, 3, infantryIcon),
        MakeUnit("SNIPE", "狙击手", viewer::UnitCategory::Infantry, 4, infantryIcon),
        MakeUnit("CLEG", "超时空军团兵（示例长名称）", viewer::UnitCategory::Infantry, 6, infantryIcon),
        MakeUnit("SPY", "间谍", viewer::UnitCategory::Infantry, 1, infantryIcon),
        MakeUnit("TANY", "谭雅", viewer::UnitCategory::Infantry, 1, infantryIcon),
        MakeUnit("FV", "多功能步兵车", viewer::UnitCategory::Vehicle, 5, vehicleIcon),
        MakeUnit("MTNK", "灰熊坦克", viewer::UnitCategory::Vehicle, 7, vehicleIcon),
        MakeUnit("SREF", "光棱坦克", viewer::UnitCategory::Vehicle, 4, vehicleIcon),
        MakeUnit("CMIN", "超时空采矿车", viewer::UnitCategory::Vehicle, 3, vehicleIcon),
        MakeUnit("MGTK", "幻影坦克", viewer::UnitCategory::Vehicle, 2, vehicleIcon),
        MakeUnit("ORCA", "入侵者战机", viewer::UnitCategory::Aircraft, 3, aircraftIcon),
        MakeUnit("BEAG", "黑鹰战机", viewer::UnitCategory::Aircraft, 2, aircraftIcon),
        MakeUnit("GACNST", "盟军建造厂", viewer::UnitCategory::Building, 1, buildingIcon),
        MakeUnit("GAPOWR", "发电厂", viewer::UnitCategory::Building, 4, buildingIcon),
        MakeUnit("GAREFN", "矿石精炼厂", viewer::UnitCategory::Building, 2, buildingIcon),
        MakeUnit("GAPILE", "兵营", viewer::UnitCategory::Building, 2, buildingIcon),
        MakeUnit("GAWEAP", "战车工厂", viewer::UnitCategory::Building, 1, buildingIcon),
        MakeUnit("GAPILL", "机枪碉堡", viewer::UnitCategory::Building, 3, buildingIcon)};

    viewer::PlayerSummary aiPlayer;
    aiPlayer.mHouseIndex = 2;
    aiPlayer.mName = "电脑对手";
    aiPlayer.mFaction = "苏联";
    aiPlayer.mColorRgb = 0xB45044;
    aiPlayer.mIsHuman = false;
    aiPlayer.mCredits = 4'200;
    aiPlayer.mPowerOutput = 0;
    aiPlayer.mPowerDrain = 860;
    aiPlayer.mHasProductionData = true;
    aiPlayer.mHasUnitData = true;
    aiPlayer.mProductionLines = {
        MakeLine(11, "战车工厂（北）", "犀牛坦克", vehicleIcon, 0.91f, 3),
        MakeLine(12, "兵营（东）", "动员兵", infantryIcon, 0.43f, 8),
        MakeLine(13, "战车工厂（南）", "犀牛坦克", vehicleIcon, 0.12f, 1),
        MakeLine(14, "兵营（西）", "磁暴步兵", infantryIcon, 1.0f, 0, true)};
    aiPlayer.mUnits = {
        MakeUnit("E2", "动员兵", viewer::UnitCategory::Infantry, 27, infantryIcon),
        MakeUnit("HTNK", "犀牛坦克", viewer::UnitCategory::Vehicle, 11, vehicleIcon),
        MakeUnit("Kirov", "基洛夫飞艇", viewer::UnitCategory::Aircraft, 1, aircraftIcon),
        MakeUnit("NARADR", "雷达站", viewer::UnitCategory::Building, 1, buildingIcon)};

    viewer::PlayerSummary thirdPlayer;
    thirdPlayer.mHouseIndex = 3;
    thirdPlayer.mName = "东部基地";
    thirdPlayer.mFaction = "尤里";
    thirdPlayer.mColorRgb = 0x8B68A8;
    thirdPlayer.mIsHuman = true;
    thirdPlayer.mDefeated = true;
    thirdPlayer.mCredits = 0;
    thirdPlayer.mPowerOutput = 760;
    thirdPlayer.mPowerDrain = 690;
    thirdPlayer.mHasProductionData = false;
    thirdPlayer.mHasUnitData = false;

    // 统计示例：游戏时间 48:12；曲线、停产历史与各项统计都是示例值，数值之间大体自洽（战损比 = 摧毁 ÷ 损失）。
    constexpr std::int64_t DEMO_TIME_MS = 2'892'000;
    constexpr std::int64_t CURVE_END_MS = 2'880'000;
    snapshot->mGameTime = viewer::GameTime{DEMO_TIME_MS, options.mNominalTime};

    // 示例时间轴：到当前帧均匀累计到示例游戏时间（第 28 540 帧完成时 48:12），之后每帧 100 ms（联机 10 帧/秒）。
    // 只用来让进度条的悬停提示与快进状态有时间可显示，不是从录像算出的。
    {
        auto timeline = std::make_shared<viewer::GameTimeline>();
        const std::uint32_t totalFrames = *snapshot->mTotalFrames;
        const std::uint32_t currentFrame = snapshot->mFrameNumber;
        timeline->mCumulativeMs.reserve(totalFrames);
        for (std::uint32_t frame = 0; frame < totalFrames; ++frame)
        {
            const std::int64_t timeMs = frame <= currentFrame
                ? DEMO_TIME_MS * (frame + 1) / (currentFrame + 1)
                : DEMO_TIME_MS + static_cast<std::int64_t>(frame - currentFrame) * 100;
            timeline->mCumulativeMs.push_back(timeMs);
        }
        if (options.mNominalTime)
            timeline->mFirstNominalFrame = 0;
        snapshot->mTimeline = std::move(timeline);
        snapshot->mPlaybackControls.mCanSeek = true;
        if (options.mSeeking)
            snapshot->mPlaybackControls.mSeek = viewer::PlaybackSeekView{true, 21'000, 52'000};
    }

    viewer::PlayerStatistics localStats;
    localStats.mCombat = viewer::CombatStatistics{142, 17, 96, 9, 159.0f / 105.0f};
    localStats.mBuiltBuildings = 41;
    localStats.mBuiltVehicles = 96;
    localStats.mBuiltInfantry = 160;
    localStats.mBuiltAircraft = 15;
    localStats.mScore = 18'420;
    localStats.mOperationCount = 3'560;
    localStats.mOperationEventCount = 14'820;
    localStats.mApm = 74.0f;
    localStats.mBarracksUpgrade = {true, 765'000};
    localPlayer.mUnitTotal = 83;
    localPlayer.mStatistics = localStats;
    localPlayer.mBarracksLine = MakeLineStatus(viewer::ProductionLineState::Producing, DEMO_TIME_MS,
        MakeHistory({{600'000, 655'000, false}, {1'320'000, 1'401'000, true}, {2'100'000, 2'160'000, false}}));
    localPlayer.mWarFactoryLine = MakeLineStatus(viewer::ProductionLineState::Stalled, DEMO_TIME_MS,
        MakeHistory({{1'020'000, 1'064'000, false}, {1'900'000, 1'987'000, true}}), 2'792'000, true);
    localPlayer.mEconomy = MakeCurve(CURVE_END_MS, 6'450, 0.035, 0.0);

    viewer::PlayerStatistics aiStats;
    aiStats.mCombat = viewer::CombatStatistics{96, 9, 142, 17, 105.0f / 159.0f};
    aiStats.mBuiltBuildings = 36;
    aiStats.mBuiltVehicles = 88;
    aiStats.mBuiltInfantry = 84;
    aiStats.mBuiltAircraft = 12;
    aiStats.mScore = 12'100;
    aiStats.mWarFactoryUpgrade = {true, 1'290'000};
    aiPlayer.mUnitTotal = 40;
    aiPlayer.mStatistics = aiStats;
    aiPlayer.mBarracksLine = MakeLineStatus(viewer::ProductionLineState::Paused, DEMO_TIME_MS,
        MakeHistory({{900'000, 930'000, true}}));
    aiPlayer.mWarFactoryLine = MakeLineStatus(viewer::ProductionLineState::PowerLoss, DEMO_TIME_MS);
    aiPlayer.mEconomy = MakeCurve(CURVE_END_MS, 4'200, 0.052, 1.3);

    viewer::PlayerStatistics thirdStats;
    thirdStats.mCombat = viewer::CombatStatistics{12, 1, 60, 12, 13.0f / 72.0f};
    thirdStats.mBuiltBuildings = 20;
    thirdStats.mBuiltVehicles = 11;
    thirdStats.mBuiltInfantry = 14;
    thirdStats.mBuiltAircraft = 0;
    thirdStats.mScore = 2'100;
    thirdStats.mOperationCount = 410;
    thirdStats.mOperationEventCount = 1'350;
    thirdStats.mApm = 9.0f;
    thirdPlayer.mUnitTotal = 0;
    thirdPlayer.mStatistics = thirdStats;
    thirdPlayer.mBarracksLine = MakeLineStatus(viewer::ProductionLineState::NoFunds, DEMO_TIME_MS);
    thirdPlayer.mWarFactoryLine = MakeLineStatus(viewer::ProductionLineState::NoFactory, DEMO_TIME_MS,
        MakeHistory({{300'000, 411'000, false}}));
    thirdPlayer.mEconomy = MakeCurve(CURVE_END_MS, 3'800, 0.071, 2.4, 2'040'000);

    snapshot->mPlayers = {std::move(localPlayer), std::move(aiPlayer), std::move(thirdPlayer)};
    snapshot->mRecordedGroups = {
        {1, {MakeUnit("E1", "美国大兵", viewer::UnitCategory::Infantry, 6, infantryIcon),
             MakeUnit("FV", "多功能步兵车", viewer::UnitCategory::Vehicle, 2, vehicleIcon)}},
        {2, {MakeUnit("HTNK", "灰熊坦克", viewer::UnitCategory::Vehicle, 4, vehicleIcon)}},
        {3, {MakeUnit("ORCA", "入侵者战机", viewer::UnitCategory::Aircraft, 3, aircraftIcon)}}};
    snapshot->mRecordedGroups.front().mUnmatchedMemberCount = 1;

    snapshot->mRecordedFps.reserve(90);
    for (std::uint32_t index = 0; index < 90; ++index)
    {
        const float wave = static_cast<float>((index * 7u) % 19u);
        snapshot->mRecordedFps.push_back({snapshot->mFrameNumber - 89u + index, 54.0f + wave});
    }

    if (options.mRadarEvents)
        snapshot->mRadarEvents = CreateReplayHudDemoRadarEvents(DEMO_TIME_MS);

    if (options.mMissingData)
    {
        snapshot->mStartedUtc.reset();
        snapshot->mMapHash.clear();
        snapshot->mHasRecordedGroups = false;
        snapshot->mRecordedGroups.clear();
        snapshot->mRecordedFps.clear();
        snapshot->mTotalFrames.reset();
        snapshot->mTimeline.reset();
        snapshot->mPlaybackControls.mCanSeek = false;
        snapshot->mPlayers.front().mCredits.reset();
        snapshot->mPlayers.front().mHasProductionData = false;
        snapshot->mPlayers.front().mHasUnitData = false;
        snapshot->mPlayers.front().mProductionLines.clear();
        snapshot->mPlayers.front().mUnits.clear();
        snapshot->mGameTime.reset();
        snapshot->mPlayers.front().mUnitTotal.reset();
        snapshot->mPlayers.front().mStatistics.reset();
        snapshot->mPlayers.front().mBarracksLine.reset();
        snapshot->mPlayers.front().mWarFactoryLine.reset();
        snapshot->mPlayers.front().mEconomy.reset();
        // 演示录制方失去雷达时的面板提示。
        snapshot->mIsNativeRadarAvailable = false;
    }

    return snapshot;
}
}
