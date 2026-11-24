#include "../src/ui/ReplayHudLayout.h"
#include "../src/ui/ReplayHudMarkModel.h"
#include "../src/ui/ReplayHudRadarModel.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool IsNear(float left, float right, float tolerance = 0.001f) { return std::abs(left - right) <= tolerance; }

viewer::RadarEvent MakeEvent(std::uint64_t id, std::int64_t timeMs, viewer::RadarEventKind kind =
    viewer::RadarEventKind::SuperWeaponLaunched)
{
    viewer::RadarEvent event;
    event.mId = id;
    event.mKind = kind;
    event.mGameTimeMs = timeMs;
    event.mActor = {1, "玩家A", 0x4E9A78};
    event.mSubjectName = "闪电风暴";
    if (kind == viewer::RadarEventKind::BuildingInfiltrated)
    {
        event.mActor = {3, "玩家B", 0x8B68A8};
        event.mVictim = {1, "玩家A", 0x4E9A78};
        event.mSubjectName = "兵营";
    }
    return event;
}

std::shared_ptr<viewer::RadarEventList> MakeList(std::initializer_list<viewer::RadarEvent> events)
{
    auto list = std::make_shared<viewer::RadarEventList>();
    list->mEvents.assign(events.begin(), events.end());
    return list;
}

void CheckEventText()
{
    const auto launch = ui::BuildRadarEventText(MakeEvent(1, 0));
    Require(ui::JoinRadarEventText(launch) == "玩家A 释放了 闪电风暴", "launch text: " + ui::JoinRadarEventText(launch));
    Require(launch.size() == 3 && launch[0].mRole == ui::RadarTextRole::Actor && launch[2].mRole == ui::RadarTextRole::Plain,
            "only the player name is colored in a launch");
    auto infiltration = MakeEvent(2, 0, viewer::RadarEventKind::BuildingInfiltrated);
    const auto spy = ui::BuildRadarEventText(infiltration);
    Require(ui::JoinRadarEventText(spy) == "玩家B 的间谍渗透了 玩家A 的 兵营", "infiltration text: " + ui::JoinRadarEventText(spy));
    Require(spy[0].mRole == ui::RadarTextRole::Actor && spy[2].mRole == ui::RadarTextRole::Victim,
            "the spy owner and the victim are colored separately");
    infiltration.mVictim = {-1, "中立", 0x8A8F86};
    infiltration.mSubjectName = "科技医院";
    Require(ui::JoinRadarEventText(ui::BuildRadarEventText(infiltration)) == "玩家B 的间谍渗透了 中立 的 科技医院",
            "an unowned building reads as neutral");
}

void CheckAgesAndFade()
{
    // 没有悬停：年龄 = 游戏时间 − 事件时间。
    auto list = MakeList({MakeEvent(1, 10'000), MakeEvent(2, 20'000)});
    ui::RadarEventAges ages;
    ages.Update(list.get(), 25'000, false);
    Require(ages.GetAgeMs(list->mEvents[0]) == 15'000 && ages.GetAgeMs(list->mEvents[1]) == 5'000,
            "without hovering the age is simply the elapsed game time");
    Require(ages.GetTrackedCount() == 0, "nothing is tracked without hovering");

    // 悬停：已有事件的年龄冻结在悬停开始前一帧的值，游戏时间照常前进。
    ages.Update(list.get(), 26'000, true);
    ages.Update(list.get(), 36'000, true);
    Require(ages.GetAgeMs(list->mEvents[0]) == 15'000 && ages.GetAgeMs(list->mEvents[1]) == 5'000,
            "hovering freezes the age of the events already shown");
    Require(ages.GetTrackedCount() == 2, "hovered events are tracked");
}

void CheckHoverPause()
{
    auto list = MakeList({MakeEvent(1, 10'000)});
    ui::RadarEventAges ages;
    ages.Update(list.get(), 12'000, false);
    Require(ages.GetAgeMs(list->mEvents[0]) == 2'000, "initial age");
    // 悬停 20 秒游戏时间：年龄停在 2 000 ms，事件不会在此期间到期。
    for (std::int64_t now = 13'000; now <= 32'000; now += 1'000)
        ages.Update(list.get(), now, true);
    Require(ages.GetAgeMs(list->mEvents[0]) == 2'000, "the age stays put while hovering");
    Require(viewer::IsRadarEventRecent(ages.GetAgeMs(list->mEvents[0])), "a hovered event does not expire");
    // 移开后继续淡出。
    ages.Update(list.get(), 42'000, false);
    Require(ages.GetAgeMs(list->mEvents[0]) == 2'000 + 10'000, "after the hover ends the age resumes from where it stopped");
}

void CheckNewEventsDuringHover()
{
    // 悬停期间到达的事件从满不透明开始，移开后才开始计时，不会因为整体平移时钟而多显示一段时间。
    auto list = MakeList({MakeEvent(1, 10'000)});
    ui::RadarEventAges ages;
    ages.Update(list.get(), 11'000, false);
    ages.Update(list.get(), 12'000, true);
    list->mEvents.push_back(MakeEvent(2, 40'000));
    for (std::int64_t now = 13'000; now <= 60'000; now += 1'000)
        ages.Update(list.get(), now, true);
    const std::int64_t newAge = ages.GetAgeMs(list->mEvents[1]);
    Require(newAge <= 1'000, "an event that arrives while hovering is shown fresh: " + std::to_string(newAge));
    ages.Update(list.get(), 70'000, false);
    Require(ages.GetAgeMs(list->mEvents[1]) <= 1'000 + 10'000, "it ages normally once the hover ends");
    Require(ages.GetAgeMs(list->mEvents[0]) > 1'000 && ages.GetAgeMs(list->mEvents[0]) <= 3'000 + 10'000,
            "the older event resumes from its frozen age");
}

void CheckPausedClockAndExpiry()
{
    auto list = MakeList({MakeEvent(1, 0)});
    ui::RadarEventAges ages;
    // 游戏时间不前进（暂停）：悬停与否都不累计，也不改变年龄。
    ages.Update(list.get(), 5'000, false);
    for (int index = 0; index < 5; ++index)
        ages.Update(list.get(), 5'000, true);
    Require(ages.GetAgeMs(list->mEvents[0]) == 5'000, "a paused clock does not accumulate");
    // 时间回退不产生负的年龄。
    ages.Update(list.get(), 1'000, false);
    Require(ages.GetAgeMs(list->mEvents[0]) == 1'000, "age follows the clock");
    ages.Update(list.get(), 200, false);
    Require(ages.GetAgeMs(list->mEvents[0]) == 200, "age is never negative");
    list->mEvents[0].mGameTimeMs = 10'000;
    ages.Update(list.get(), 5'000, false);
    Require(ages.GetAgeMs(list->mEvents[0]) == 0, "an event from the future has age 0");

    // 到期的事件不再保留状态。
    auto expiring = MakeList({MakeEvent(7, 0)});
    ui::RadarEventAges tracked;
    tracked.Update(expiring.get(), 1'000, false);
    tracked.Update(expiring.get(), 2'000, true);
    Require(tracked.GetTrackedCount() == 1, "a hovered event is tracked");
    tracked.Update(expiring.get(), 60'000, false);
    Require(tracked.GetTrackedCount() == 0 || !viewer::IsRadarEventRecent(tracked.GetAgeMs(expiring->mEvents[0])),
            "an expired event is no longer recent");
    tracked.Update(nullptr, 61'000, true);
    Require(tracked.GetTrackedCount() == 0, "no event list clears the state");
}

void CheckRowSelection()
{
    // 最近模式：最新的在上，最多 5 条，超过 30 秒的不显示，不透明度随年龄淡出。
    auto list = std::make_shared<viewer::RadarEventList>();
    for (std::uint64_t id = 1; id <= 9; ++id)
        list->mEvents.push_back(MakeEvent(id, static_cast<std::int64_t>(id) * 3'000));
    ui::RadarEventAges ages;
    ages.Update(list.get(), 30'000, false);
    const auto recent = ui::SelectRecentRadarRows(list.get(), ages);
    Require(recent.size() == 5, "at most five recent rows: " + std::to_string(recent.size()));
    Require(recent[0].mEvent->mId == 9 && recent[4].mEvent->mId == 5, "newest first");
    Require(recent[0].mOpacity == 1.0f && recent[0].mAgeMs == 3'000, "a young event is opaque");

    ui::RadarEventAges lateAges;
    lateAges.Update(list.get(), 27'000 + 27'500, false);
    const auto fading = ui::SelectRecentRadarRows(list.get(), lateAges);
    Require(fading.size() == 1 && fading[0].mEvent->mId == 9, "only events younger than 30 s remain");
    Require(IsNear(fading[0].mOpacity, 0.5f), "the last seconds fade linearly");

    ui::RadarEventAges expiredAges;
    expiredAges.Update(list.get(), 200'000, false);
    Require(ui::SelectRecentRadarRows(list.get(), expiredAges).empty(), "old events disappear from the recent view");
    Require(ui::SelectRecentRadarRows(nullptr, expiredAges).empty() && ui::SelectHistoryRadarRows(nullptr).empty(),
            "no list means no rows");

    // 历史模式：全部事件，最新的在上，不淡出。
    const auto history = ui::SelectHistoryRadarRows(list.get());
    Require(history.size() == 9 && history.front().mEvent->mId == 9 && history.back().mEvent->mId == 1 &&
                history.front().mOpacity == 1.0f && history.back().mOpacity == 1.0f,
            "history lists everything newest first without fading");
}

ui::RadarBarLayoutInput BarLayout(float width, float height, float scale)
{
    ui::RadarBarLayoutInput input;
    input.mDisplayWidth = width;
    input.mDisplayHeight = height;
    input.mScale = scale;
    // 底栏 106 × 缩放；面板按原生 168 宽加宽。
    input.mFooterTop = height - std::round(106.0f * scale);
    input.mPanelLeft = width - std::floor(168.0f * scale);
    return input;
}

void CheckBarLayout()
{
    // 2560×1440、100%：FPS 卡下沿 14 + 180，可用区从 208 到 1320，5 条整块高 256，居中后上沿为 636。
    auto input = BarLayout(2560.0f, 1440.0f, 1.0f);
    input.mRowCount = 5;
    const ui::UiRect five = ui::GetRadarBarRect(input);
    Require(five.mLeft == 14.0f && five.mRight - five.mLeft == 260.0f && five.mTop == 636.0f &&
                five.mBottom - five.mTop == 30.0f + 5.0f * 44.0f + 6.0f,
            "the five-row bar is 260 wide at the left edge and vertically centered: top " + std::to_string(five.mTop));

    // 上沿与条数、折叠无关：标题行不会随事件增减跳动。
    for (const std::size_t rows : {std::size_t(0), std::size_t(1), std::size_t(3), std::size_t(5)})
    {
        input.mRowCount = rows;
        Require(ui::GetRadarBarRect(input).mTop == five.mTop, "the title row stays put when rows come and go");
    }
    input.mRowCount = 0;
    Require(ui::GetRadarBarRect(input).mBottom - five.mTop == 30.0f + 26.0f, "no events shows one hint row");
    input.mIsCollapsed = true;
    const ui::UiRect collapsed = ui::GetRadarBarRect(input);
    Require(collapsed.mTop == five.mTop && collapsed.mBottom - collapsed.mTop == 30.0f,
            "a collapsed bar is only the title row");
    input.mIsCollapsed = false;
    input.mShowHistory = true;
    const ui::UiRect history = ui::GetRadarBarRect(input);
    Require(history.mTop == five.mTop && history.mBottom - history.mTop == 30.0f + 10.0f * 44.0f + 6.0f,
            "history mode shows ten rows and scrolls the rest");

    // 各档位、各画面尺寸：不盖住 FPS 卡与底栏、不越出画面、不越过地图区右缘，整数像素。
    const float sizes[][2] = {{2560, 1440}, {1920, 1080}, {1600, 900}, {1280, 720}, {1024, 768}, {800, 600}};
    for (const auto& size : sizes)
    {
        for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f})
        {
            for (int mode = 0; mode < 4; ++mode)
            {
                auto layout = BarLayout(size[0], size[1], scale);
                layout.mIsCollapsed = mode == 0;
                layout.mShowHistory = mode == 3;
                layout.mRowCount = mode == 1 ? 0 : 5;
                const ui::UiRect rect = ui::GetRadarBarRect(layout);
                const std::string name = std::to_string(static_cast<int>(size[0])) + "x" +
                    std::to_string(static_cast<int>(size[1])) + " @" + std::to_string(static_cast<int>(scale * 100)) +
                    "% mode " + std::to_string(mode);
                Require(rect.mLeft >= 0.0f && rect.mTop >= 0.0f && rect.mRight <= size[0] && rect.mBottom <= size[1],
                        name + ": bar stays inside the display");
                Require(rect.mRight <= *layout.mPanelLeft, name + ": bar stays left of the sidebar panel");
                Require(rect.mLeft == std::floor(rect.mLeft) && rect.mTop == std::floor(rect.mTop) &&
                            rect.mRight == std::floor(rect.mRight) && rect.mBottom == std::floor(rect.mBottom),
                        name + ": whole pixels");
                const float fpsBottom = std::round(14.0f * scale) + std::round(180.0f * scale);
                const float height = rect.mBottom - rect.mTop;
                const ui::RadarBarMetrics metrics = ui::GetRadarBarMetrics(scale);
                Require(rect.mTop >= fpsBottom, name + ": bar does not cover the FPS card");
                // 画面小到 FPS 卡与底栏之间放不下标题行时（例如 800×600 的 200%），重叠无法避免，不在检查范围。
                const float freeHeight = layout.mFooterTop - std::round(14.0f * scale) - (fpsBottom + std::round(14.0f * scale));
                if (freeHeight >= metrics.mTitleHeight)
                    Require(rect.mBottom <= layout.mFooterTop - std::round(14.0f * scale) + 0.5f,
                            name + ": bar does not cover the footer");
                Require(height >= metrics.mTitleHeight, name + ": the title row is always there");
            }
        }
    }

    // 放得下的条数：大画面是 5 条；画面小到放不下时只显示最新的几条，窗口内容不越出窗口下缘。
    Require(ui::GetRadarBarRowCapacity(BarLayout(2560.0f, 1440.0f, 1.0f)) == 5, "a large display fits five rows");
    Require(ui::GetRadarBarRowCapacity(BarLayout(1920.0f, 1080.0f, 2.0f)) < 5 ||
                ui::GetRadarBarRect([] { auto input = BarLayout(1920.0f, 1080.0f, 2.0f); input.mRowCount = 5; return input; }())
                        .mBottom <= 1080.0f,
            "a cramped display keeps the bar on screen");
    for (const auto& size : sizes)
    {
        for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f})
        {
            auto layout = BarLayout(size[0], size[1], scale);
            const std::size_t capacity = ui::GetRadarBarRowCapacity(layout);
            Require(capacity <= 5, "at most five rows");
            if (capacity > 0)
            {
                layout.mRowCount = capacity;
                const ui::UiRect rect = ui::GetRadarBarRect(layout);
                const ui::RadarBarMetrics metrics = ui::GetRadarBarMetrics(scale);
                Require(rect.mBottom - rect.mTop >= metrics.mTitleHeight + static_cast<float>(capacity) * metrics.mRowHeight,
                        "the rows that fit are not clipped by the window height");
            }
        }
    }

    // 地图区很窄时宽度被夹住，仍在画面内。
    auto narrow = BarLayout(800.0f, 600.0f, 2.0f);
    const ui::UiRect narrowRect = ui::GetRadarBarRect(narrow);
    Require(narrowRect.mRight - narrowRect.mLeft <= *narrow.mPanelLeft - 2.0f * 28.0f + 0.5f, "the bar narrows with the map area");
}

void CheckPointMapper()
{
    // 宿主确认的图像区域：画布坐标线性映射。
    const ui::GameImagePlacement exact{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const auto mapper = ui::MakeNativePointMapper(1024, 768, 1600.0f, 900.0f, exact);
    Require(mapper && mapper->mIsExact, "a host-confirmed image rect is exact");
    const ui::UiPoint center = mapper->MapPoint(512.0f, 384.0f);
    Require(IsNear(center.mX, 800.0f) && IsNear(center.mY, 450.0f), "the canvas center maps to the display center");
    const ui::UiRect view = mapper->MapRect({0, 0, 856, 736});
    Require(IsNear(view.mLeft, 0.0f) && IsNear(view.mRight, 856.0f * 1600.0f / 1024.0f) &&
                IsNear(view.mBottom, 736.0f * 900.0f / 768.0f), "rects map with independent axis scales");

    // 留边的图像区域（maintas）：映射到该区域内。
    const ui::GameImagePlacement boxed{{200.0f, 0.0f, 1400.0f, 900.0f}, true};
    const auto boxedMapper = ui::MakeNativePointMapper(1600, 900, 1600.0f, 900.0f, boxed);
    Require(boxedMapper && IsNear(boxedMapper->MapPoint(0.0f, 0.0f).mX, 200.0f) &&
                IsNear(boxedMapper->MapPoint(1600.0f, 900.0f).mX, 1400.0f), "a letterboxed image maps inside its rect");

    // 位置不确定（D3D9 顶点留边）：拉伸与等比留边两种解释不同，标记为不确定，取参考矩形或整块拉伸。
    const auto ambiguous = ui::MakeNativePointMapper(1024, 768, 1600.0f, 900.0f, {{0.0f, 0.0f, 1600.0f, 900.0f}, false});
    Require(ambiguous && !ambiguous->mIsExact && IsNear(ambiguous->MapPoint(1024.0f, 768.0f).mX, 1600.0f),
            "an ambiguous mapping is flagged and falls back to the reference rect");
    const auto sameAspect = ui::MakeNativePointMapper(800, 450, 1600.0f, 900.0f, {});
    Require(sameAspect && sameAspect->mIsExact, "stretch and fit agree when the aspect ratios match");
    Require(!ui::MakeNativePointMapper(0, 768, 1600.0f, 900.0f, exact) && !ui::MakeNativePointMapper(1024, 768, 0.0f, 900.0f, exact),
            "invalid sizes have no mapping");
}

viewer::SuperWeaponMarkFrame MakeMarkFrame(std::int64_t gameTimeMs)
{
    viewer::SuperWeaponMarkFrame frame;
    frame.mSessionId = 1;
    frame.mGameTimeMs = gameTimeMs;
    frame.mCanvasWidth = 1600;
    frame.mCanvasHeight = 900;
    frame.mView = {0, 0, 1432, 868};
    return frame;
}

std::shared_ptr<viewer::SuperWeaponMarkInfo> MakeMarkInfo(std::uint64_t id, std::int64_t releasedAt, int iconWidth = 0,
                                                          int iconHeight = 0)
{
    auto info = std::make_shared<viewer::SuperWeaponMarkInfo>();
    info->mId = id;
    info->mReleasedAtMs = releasedAt;
    info->mLabel = "闪电风暴";
    if (iconWidth > 0)
    {
        auto icon = std::make_shared<viewer::IconImage>();
        icon->mWidth = iconWidth;
        icon->mHeight = iconHeight;
        icon->mRgba.resize(static_cast<std::size_t>(iconWidth) * iconHeight * 4);
        info->mIcon = icon;
    }
    return info;
}

void CheckMarkLayout()
{
    const ui::GameImagePlacement image{{0.0f, 0.0f, 1600.0f, 900.0f}, true};
    const auto mapper = *ui::MakeNativePointMapper(1600, 900, 1600.0f, 900.0f, image);

    auto frame = MakeMarkFrame(10'000);
    frame.mMarks.push_back({MakeMarkInfo(1, 9'000, 60, 48), {400, 300}});
    ui::MarkLayout layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    Require(layout.mPlacements.size() == 1 && layout.mPlacements[0].mIsVisible, "a mark inside the view is drawn");
    const ui::UiRect icon = layout.mPlacements[0].mIconRect;
    Require(icon.mRight - icon.mLeft == 60.0f && icon.mBottom - icon.mTop == 48.0f && icon.mLeft == 370.0f &&
                icon.mTop == 276.0f, "the icon keeps its pixel size and is centered on the impact point");
    Require(IsNear(layout.mPlacements[0].mOpacity, 0.6f), "a young mark is 60% opaque");
    Require(layout.mClip.mRight == 1432.0f && layout.mClip.mBottom == 868.0f, "marks are clipped to the tactical view");

    // 大小随 HUD 缩放，宽高比不变。
    layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.5f);
    Require(layout.mPlacements[0].mIconRect.mRight - layout.mPlacements[0].mIconRect.mLeft == 90.0f &&
                layout.mPlacements[0].mIconRect.mBottom - layout.mPlacements[0].mIconRect.mTop == 72.0f,
            "the icon scales with the HUD");

    // 大图标按比例缩小到上限，宽高比保持；没有图标时用 60×48 的通用标记。
    frame.mMarks[0].mInfo = MakeMarkInfo(1, 9'000, 192, 96);
    layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    Require(layout.mPlacements[0].mIconRect.mRight - layout.mPlacements[0].mIconRect.mLeft == 96.0f &&
                layout.mPlacements[0].mIconRect.mBottom - layout.mPlacements[0].mIconRect.mTop == 48.0f,
            "an oversized icon keeps its aspect ratio");
    frame.mMarks[0].mInfo = MakeMarkInfo(1, 9'000);
    layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    Require(layout.mPlacements[0].mIconRect.mRight - layout.mPlacements[0].mIconRect.mLeft == 60.0f &&
                layout.mPlacements[0].mIconRect.mBottom - layout.mPlacements[0].mIconRect.mTop == 48.0f,
            "a missing icon uses the generic 60x48 marker");

    // 屏幕外的落点不绘制：完全在战术视图之外；被侧栏一侧裁掉一半的仍绘制。
    frame.mMarks.clear();
    frame.mMarks.push_back({MakeMarkInfo(1, 9'000, 60, 48), {-200, 300}});
    frame.mMarks.push_back({MakeMarkInfo(2, 9'000, 60, 48), {2000, 300}});
    frame.mMarks.push_back({MakeMarkInfo(3, 9'000, 60, 48), {400, 2000}});
    frame.mMarks.push_back({MakeMarkInfo(4, 9'000, 60, 48), {1420, 300}});
    frame.mMarks.push_back({MakeMarkInfo(5, 9'000, 60, 48), {1440, 880}});
    layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    Require(!layout.mPlacements[0].mIsVisible && !layout.mPlacements[1].mIsVisible && !layout.mPlacements[2].mIsVisible,
            "marks outside the view are not drawn");
    Require(layout.mPlacements[3].mIsVisible, "a mark straddling the view edge is drawn (clipped)");
    Require(!layout.mPlacements[4].mIsVisible || layout.mPlacements[4].mIconRect.mLeft < 1432.0f,
            "a mark beyond the corner is clipped away");

    // 淡出：13.5 秒时一半，15 秒完全消失（不绘制）。
    frame.mMarks.clear();
    frame.mMarks.push_back({MakeMarkInfo(1, 0, 60, 48), {400, 300}});
    frame.mGameTimeMs = 13'500;
    layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    Require(IsNear(layout.mPlacements[0].mOpacity, 0.3f) && layout.mPlacements[0].mIsVisible, "the fade is linear in the last 3 s");
    frame.mGameTimeMs = 15'000;
    layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    Require(layout.mPlacements[0].mOpacity == 0.0f && !layout.mPlacements[0].mIsVisible, "a faded-out mark is not drawn");

    // 同一位置的多个落点错开少许，最多 4 步；相隔很远的不错开。
    frame.mGameTimeMs = 1'000;
    frame.mMarks.clear();
    for (std::uint64_t id = 1; id <= 7; ++id)
        frame.mMarks.push_back({MakeMarkInfo(id, 0, 60, 48), {500, 400}});
    frame.mMarks.push_back({MakeMarkInfo(8, 0, 60, 48), {900, 400}});
    layout = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    Require(layout.mPlacements[0].mIconRect.mLeft == 470.0f, "the first of a pile stays on the impact point");
    Require(layout.mPlacements[1].mIconRect.mLeft == 492.0f && layout.mPlacements[1].mIconRect.mTop == 358.0f,
            "the second is shifted a little up and to the right");
    Require(layout.mPlacements[4].mIconRect.mLeft == 558.0f && layout.mPlacements[6].mIconRect.mLeft == 558.0f,
            "the shift stops after four steps");
    Require(layout.mPlacements[7].mIconRect.mLeft == 870.0f, "a distant mark is not shifted");
    for (const auto& placement : layout.mPlacements)
        Require(placement.mAnchor.mX == (placement.mMark->mInfo->mId == 8 ? 900.0f : 500.0f), "the anchor is never shifted");

    // 镜头滚动只平移整体：错开结果与滚动无关。
    frame.mMarks.clear();
    for (std::uint64_t id = 1; id <= 3; ++id)
        frame.mMarks.push_back({MakeMarkInfo(id, 0, 60, 48), {500, 400}});
    const auto before = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    for (auto& mark : frame.mMarks)
    {
        mark.mPosition.mX += 123;
        mark.mPosition.mY -= 77;
    }
    const auto after = ui::LayoutSuperWeaponMarks(frame, mapper, 1.0f);
    for (std::size_t index = 0; index < 3; ++index)
        Require(after.mPlacements[index].mIconRect.mLeft - before.mPlacements[index].mIconRect.mLeft == 123.0f &&
                    after.mPlacements[index].mIconRect.mTop - before.mPlacements[index].mIconRect.mTop == -77.0f,
                "scrolling the map translates the whole pile");
}
}

int main()
{
    try
    {
        CheckEventText();
        CheckAgesAndFade();
        CheckHoverPause();
        CheckNewEventsDuringHover();
        CheckPausedClockAndExpiry();
        CheckRowSelection();
        CheckBarLayout();
        CheckPointMapper();
        CheckMarkLayout();
        std::cout << "Replay HUD radar model tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
