#include "../src/stats/RadarEventLog.h"
#include "../src/stats/SuperWeaponMarkTracker.h"
#include "../src/viewer/RadarDisplayRules.h"

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

bool IsNear(float left, float right) { return std::abs(left - right) < 0.0005f; }

viewer::RadarPlayerRef Player(int houseIndex, const char* name, std::uint32_t color)
{
    viewer::RadarPlayerRef player;
    player.mHouseIndex = houseIndex;
    player.mName = name;
    player.mColorRgb = color;
    return player;
}

stats::RadarEventDraft LaunchDraft(const char* name)
{
    return stats::MakeSuperWeaponLaunchDraft(Player(1, "玩家A", 0x4E9A78), name, nullptr, {1000, 2000, 300});
}

void CheckDisplayRules()
{
    using namespace viewer;
    // 落点图标：显示 15 秒游戏时间，最后 3 秒线性淡出；未淡出时约 60% 不透明。
    Require(IsNear(GetSuperWeaponMarkOpacity(0), 0.6f), "a fresh mark is 60% opaque");
    Require(IsNear(GetSuperWeaponMarkOpacity(12'000), 0.6f), "the mark stays at full strength until the fade starts");
    Require(IsNear(GetSuperWeaponMarkOpacity(13'500), 0.3f), "the middle of the fade is half of 60%");
    Require(IsNear(GetSuperWeaponMarkOpacity(14'999), 0.0002f), "the mark is almost gone just before 15 s");
    Require(GetSuperWeaponMarkOpacity(15'000) == 0.0f && GetSuperWeaponMarkOpacity(40'000) == 0.0f,
            "a mark has no opacity at or after 15 s");
    Require(IsNear(GetSuperWeaponMarkOpacity(-500), 0.6f), "a negative age is treated as fresh");
    Require(!IsSuperWeaponMarkExpired(14'999) && IsSuperWeaponMarkExpired(15'000), "a mark expires at exactly 15 s");
    float previous = 1.0f;
    for (std::int64_t age = 0; age <= 16'000; age += 250)
    {
        const float opacity = GetSuperWeaponMarkOpacity(age);
        Require(opacity <= previous + 1e-6f, "the mark opacity never increases with age");
        previous = opacity;
    }

    // 提示栏：每条显示 30 秒，最后 5 秒淡出。
    Require(GetRadarEventOpacity(0) == 1.0f && GetRadarEventOpacity(25'000) == 1.0f, "an event is opaque for 25 s");
    Require(IsNear(GetRadarEventOpacity(27'500), 0.5f), "the middle of the event fade is half opaque");
    Require(GetRadarEventOpacity(30'000) == 0.0f, "an event is gone at 30 s");
    Require(IsRadarEventRecent(29'999) && !IsRadarEventRecent(30'000), "recent events are younger than 30 s");
    Require(RADAR_EVENT_RECENT_LIMIT == 5 && RADAR_EVENT_HISTORY_LIMIT == 200, "5 recent events and 200 in history");
}

void CheckDrafts()
{
    // 超武释放：释放方、名称、落点；没有图标时图标为空（界面画通用标记）。
    const auto launch = stats::MakeSuperWeaponLaunchDraft(Player(2, "玩家B", 0xB45044), "闪电风暴", nullptr, {5, 6, 7});
    Require(launch.mKind == viewer::RadarEventKind::SuperWeaponLaunched && launch.mActor.mHouseIndex == 2 &&
                launch.mActor.mName == "玩家B" && launch.mSubjectName == "闪电风暴" && !launch.mIcon &&
                launch.mLocation && launch.mLocation->mX == 5 && launch.mLocation->mZ == 7,
            "a launch draft carries the actor, the weapon name and the impact point");
    Require(launch.mVictim.mHouseIndex == -1, "a launch has no victim");

    // 渗透：间谍所属方、被渗透方与建筑；无主建筑记为中立。
    const auto infiltration = stats::MakeInfiltrationDraft(Player(3, "玩家B", 0x8B68A8), Player(1, "玩家A", 0x4E9A78),
                                                            "兵营", {100, 200, 0});
    Require(infiltration.mKind == viewer::RadarEventKind::BuildingInfiltrated && infiltration.mActor.mName == "玩家B" &&
                infiltration.mVictim.mName == "玩家A" && infiltration.mSubjectName == "兵营" && !infiltration.mIcon &&
                infiltration.mLocation && infiltration.mLocation->mX == 100,
            "an infiltration draft names the spy owner, the victim and the building");
    const auto neutral = stats::MakeInfiltrationDraft(Player(3, "玩家B", 0x8B68A8), stats::MakeNeutralPlayerRef(),
                                                       "科技医院", {1, 2, 3});
    Require(neutral.mVictim.mHouseIndex == -1 && neutral.mVictim.mName == "中立", "an unowned building belongs to the neutral side");
}

void CheckLog()
{
    stats::RadarEventLog log;
    Require(log.GetCount() == 0 && log.GetSnapshot()->mEvents.empty(), "an empty log has an empty snapshot");
    const auto first = log.Add(LaunchDraft("核弹"), 1000);
    const auto second = log.Add(stats::MakeInfiltrationDraft(Player(3, "玩家B", 0x8B68A8), Player(1, "玩家A", 0x4E9A78),
                                                             "兵营", {1, 2, 3}), 2500);
    Require(first == 1 && second == 2, "event ids start at 1 and increase");
    const auto* found = log.Find(second);
    Require(found && found->mKind == viewer::RadarEventKind::BuildingInfiltrated && found->mGameTimeMs == 2500 &&
                found->mActor.mName == "玩家B" && found->mVictim.mName == "玩家A",
            "an event keeps the time stamped by the log");
    Require(log.Find(99) == nullptr && log.Find(0) == nullptr, "unknown ids are not found");

    // 快照：没有新事件时共享同一个对象；已发布的快照不受后续追加影响；事件按发生顺序、最新在末尾。
    const auto snapshotA = log.GetSnapshot();
    const auto snapshotB = log.GetSnapshot();
    Require(snapshotA == snapshotB && snapshotA->mEvents.size() == 2, "snapshots share one object until the next event");
    Require(snapshotA->mEvents.front().mId == 1 && snapshotA->mEvents.back().mId == 2, "the newest event is last");
    log.Add(LaunchDraft("闪电风暴"), 3000);
    const auto snapshotC = log.GetSnapshot();
    Require(snapshotC != snapshotA && snapshotC->mEvents.size() == 3 && snapshotA->mEvents.size() == 2,
            "a published snapshot is immutable");
    log.Reset();
    Require(log.GetCount() == 0 && log.Add(LaunchDraft("x"), 0) == 1, "Reset restarts the numbering");
}

void CheckHistoryLimit()
{
    stats::RadarEventLog log;
    constexpr int TOTAL = 230;
    for (int index = 0; index < TOTAL; ++index)
        log.Add(LaunchDraft("超级武器"), static_cast<std::int64_t>(index) * 1000);
    Require(log.GetCount() == viewer::RADAR_EVENT_HISTORY_LIMIT, "history keeps at most 200 events");
    const auto snapshot = log.GetSnapshot();
    Require(snapshot->mEvents.size() == 200 && snapshot->mEvents.front().mId == 31 &&
                snapshot->mEvents.back().mId == 230,
            "the oldest events are dropped and ids are never reused");
    Require(log.Find(30) == nullptr && log.Find(31) != nullptr && log.Find(230) != nullptr,
            "lookup follows the retained window");
    for (std::size_t index = 1; index < snapshot->mEvents.size(); ++index)
        Require(snapshot->mEvents[index - 1].mGameTimeMs <= snapshot->mEvents[index].mGameTimeMs,
                "events stay ordered by time");
}

std::shared_ptr<viewer::SuperWeaponMarkInfo> Mark(std::uint64_t id, std::int64_t releasedAtMs)
{
    auto info = std::make_shared<viewer::SuperWeaponMarkInfo>();
    info->mId = id;
    info->mReleasedAtMs = releasedAtMs;
    return info;
}

void CheckMarkTracker()
{
    stats::SuperWeaponMarkTracker marks;
    marks.Add(Mark(1, 1000), {1, 2, 3});
    marks.Add(Mark(2, 4000), {4, 5, 6});
    marks.Add(nullptr, {0, 0, 0});
    Require(marks.GetActive().size() == 2 && marks.GetActive().front().mInfo->mId == 1, "marks are kept oldest first");

    // 从释放帧起显示 15 秒：14 999 ms 仍在，15 000 ms 过期。
    Require(!marks.Prune(15'999), "a mark younger than 15 s stays");
    Require(marks.Prune(16'000) && marks.GetActive().size() == 1 && marks.GetActive().front().mInfo->mId == 2,
            "the first mark expires exactly 15 s after its release");
    Require(!marks.Prune(16'000), "pruning twice changes nothing");
    Require(marks.Prune(19'000) && marks.GetActive().empty(), "the second mark expires later");

    // 暂停时游戏时间不前进：不过期。
    marks.Add(Mark(3, 100'000), {0, 0, 0});
    for (int round = 0; round < 5; ++round)
        Require(!marks.Prune(100'000) && marks.GetActive().size() == 1, "a paused game never expires a mark");

    // 同时存在的落点有上限，超出时丢弃最旧的。
    marks.Reset();
    for (std::uint64_t id = 1; id <= viewer::SUPERWEAPON_MARK_LIMIT + 5; ++id)
        marks.Add(Mark(id, 0), {0, 0, 0});
    Require(marks.GetActive().size() == viewer::SUPERWEAPON_MARK_LIMIT && marks.GetActive().front().mInfo->mId == 6,
            "the mark count is bounded and the oldest are dropped");
}
}

int main()
{
    try
    {
        CheckDisplayRules();
        CheckDrafts();
        CheckLog();
        CheckHistoryLimit();
        CheckMarkTracker();
        std::cout << "Radar event log tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
