#include "../src/stats/SuperWeaponLaunch.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::stats::CHARGE_DRAIN_DRAINING;
using rareplay::stats::CHARGE_DRAIN_READY;
using rareplay::stats::SuperWeaponClickMode;
using rareplay::stats::SuperWeaponFireBegin;
using rareplay::stats::SuperWeaponFireEnd;
using rareplay::stats::SuperWeaponFireTracker;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

SuperWeaponFireBegin MakeBegin(std::uint64_t token, SuperWeaponClickMode mode, bool isReady)
{
    SuperWeaponFireBegin begin;
    begin.mSuperToken = token;
    begin.mFrame = 1000;
    begin.mHouseIndex = 2;
    begin.mTypeIndex = 7;
    begin.mMode = mode;
    begin.mIsReady = isReady;
    begin.mChargeDrainState = isReady ? CHARGE_DRAIN_READY : 0;
    begin.mTargetCell = {120, 85};
    return begin;
}

SuperWeaponFireEnd MakeEnd(std::uint64_t token, bool isReady, int chargeDrainState = -1)
{
    return {token, isReady, chargeDrainState};
}

void CheckNormalSuperWeapon()
{
    using namespace rareplay::stats;
    // 就绪的普通超武发射后 IsReady 变 0：释放。
    Require(IsSuperWeaponLaunched(MakeBegin(1, SuperWeaponClickMode::Normal, true), MakeEnd(1, false)),
            "a ready weapon that is no longer ready was launched");
    // 发射被中止（资金不足、AbortFire）时 IsReady 保持：不算。
    Require(!IsSuperWeaponLaunched(MakeBegin(1, SuperWeaponClickMode::Normal, true), MakeEnd(1, true)),
            "an aborted fire leaves the weapon ready and is not a launch");
    // 本来就没就绪（无效点击）：不算。
    Require(!IsSuperWeaponLaunched(MakeBegin(1, SuperWeaponClickMode::Normal, false), MakeEnd(1, false)),
            "a weapon that was not ready cannot launch");
    // 状态不能由 0 变 1 来触发。
    Require(!IsSuperWeaponLaunched(MakeBegin(1, SuperWeaponClickMode::Normal, false), MakeEnd(1, true)),
            "becoming ready is not a launch");
    // 配对的两次观察必须来自同一个超武对象。
    Require(!IsSuperWeaponLaunched(MakeBegin(1, SuperWeaponClickMode::Normal, true), MakeEnd(2, false)),
            "observations of different super weapons are not paired");
}

void CheckChargeDrain()
{
    using namespace rareplay::stats;
    // 开启（Ready -> Draining）算一次释放。
    auto begin = MakeBegin(5, SuperWeaponClickMode::ChargeDrain, false);
    begin.mChargeDrainState = CHARGE_DRAIN_READY;
    Require(IsSuperWeaponLaunched(begin, MakeEnd(5, true, CHARGE_DRAIN_DRAINING)),
            "turning a charge-drain weapon on is a launch even though IsReady stays set");
    // 关闭（Draining -> Ready）不提示。
    begin.mChargeDrainState = CHARGE_DRAIN_DRAINING;
    Require(!IsSuperWeaponLaunched(begin, MakeEnd(5, true, CHARGE_DRAIN_READY)),
            "turning a charge-drain weapon off is not a launch");
    // 充能中点击没有任何变化。
    begin.mChargeDrainState = 0;
    Require(!IsSuperWeaponLaunched(begin, MakeEnd(5, false, 0)), "clicking while charging is not a launch");
    // 开启时 IsReady 的取值不影响判定。
    begin.mChargeDrainState = CHARGE_DRAIN_READY;
    begin.mIsReady = false;
    Require(IsSuperWeaponLaunched(begin, MakeEnd(5, false, CHARGE_DRAIN_DRAINING)),
            "the charge-drain rule only looks at the drain state");
}

void CheckTwoStepSuperWeapon()
{
    using namespace rareplay::stats;
    // PreClick：第一段选源格，不算释放，即使 IsReady 变化。
    Require(!IsSuperWeaponLaunched(MakeBegin(8, SuperWeaponClickMode::PreClick, true), MakeEnd(8, false)),
            "the first click of a two-step weapon is not a launch");
    // PostClick：C1 时前置超武就绪即为释放。
    auto post = MakeBegin(9, SuperWeaponClickMode::PostClick, false);
    post.mHasPrerequisite = true;
    post.mPrerequisiteTypeIndex = 3;
    post.mPrerequisiteIsReady = true;
    Require(IsSuperWeaponLaunched(post, MakeEnd(9, false)), "the second click with a ready prerequisite is a launch");
    post.mPrerequisiteIsReady = false;
    Require(!IsSuperWeaponLaunched(post, MakeEnd(9, false)), "a prerequisite that was not ready does not launch");
    post.mHasPrerequisite = false;
    post.mPrerequisiteIsReady = true;
    Require(!IsSuperWeaponLaunched(post, MakeEnd(9, false)), "no prerequisite weapon means no launch");
}

void CheckTrackerAndIconSource()
{
    using namespace rareplay::stats;
    SuperWeaponFireTracker tracker;
    // 普通超武：图标取自自己的类型。
    tracker.Begin(MakeBegin(1, SuperWeaponClickMode::Normal, true));
    Require(tracker.GetDepth() == 1, "Begin pushes one snapshot");
    auto launch = tracker.End(MakeEnd(1, false));
    Require(launch && launch->mIconTypeIndex == 7 && launch->mTypeIndex == 7 && launch->mHouseIndex == 2 &&
                launch->mFrame == 1000 && launch->mTargetCell.mX == 120 && launch->mTargetCell.mY == 85,
            "a normal launch keeps house, type, frame, target cell and its own icon");
    Require(tracker.GetDepth() == 0 && tracker.GetDiscardedCount() == 0, "End pops the matching snapshot");

    // PostClick：图标改用前置超武的类型。
    auto post = MakeBegin(2, SuperWeaponClickMode::PostClick, false);
    post.mHasPrerequisite = true;
    post.mPrerequisiteTypeIndex = 3;
    post.mPrerequisiteIsReady = true;
    tracker.Begin(post);
    launch = tracker.End(MakeEnd(2, false));
    Require(launch && launch->mTypeIndex == 7 && launch->mIconTypeIndex == 3,
            "a post-click launch borrows the prerequisite weapon's icon");

    // 前置超武下标无效时回退到自己的类型。
    post.mPrerequisiteTypeIndex = -1;
    tracker.Begin(post);
    launch = tracker.End(MakeEnd(2, false));
    Require(launch && launch->mIconTypeIndex == 7, "a missing prerequisite type falls back to the weapon's own icon");

    // 不是释放时返回空，栈仍然弹出。
    tracker.Begin(MakeBegin(3, SuperWeaponClickMode::Normal, true));
    Require(!tracker.End(MakeEnd(3, true)) && tracker.GetDepth() == 0, "a non-launch pops without a result");
}

void CheckRecursion()
{
    using namespace rareplay::stats;
    // 触发器在 Fire_SW 内同步再次发射：A 开始，B 开始并结束，A 结束；按后进先出配对。
    SuperWeaponFireTracker tracker;
    auto first = MakeBegin(10, SuperWeaponClickMode::Normal, true);
    auto nested = MakeBegin(20, SuperWeaponClickMode::Normal, true);
    nested.mTypeIndex = 9;
    nested.mHouseIndex = 4;
    tracker.Begin(first);
    tracker.Begin(nested);
    Require(tracker.GetDepth() == 2, "nested fire stacks two snapshots");
    const auto inner = tracker.End(MakeEnd(20, false));
    Require(inner && inner->mTypeIndex == 9 && inner->mHouseIndex == 4, "the inner launch is paired with its own snapshot");
    Require(tracker.GetDepth() == 1, "the outer snapshot is still waiting");
    const auto outer = tracker.End(MakeEnd(10, false));
    Require(outer && outer->mTypeIndex == 7 && outer->mHouseIndex == 2, "the outer launch is paired afterwards");
    Require(tracker.GetDepth() == 0 && tracker.GetDiscardedCount() == 0, "balanced nesting loses nothing");

    // 同一个超武对象递归：最近的一次 C1 先配对。
    auto recursive = MakeBegin(30, SuperWeaponClickMode::Normal, true);
    recursive.mFrame = 1;
    tracker.Begin(recursive);
    recursive.mFrame = 2;
    tracker.Begin(recursive);
    const auto latest = tracker.End(MakeEnd(30, false));
    Require(latest && latest->mFrame == 2, "the most recent snapshot of the same weapon pairs first");
    tracker.Clear();
}

void CheckLossHandling()
{
    using namespace rareplay::stats;
    SuperWeaponFireTracker tracker;
    // C2 没有对应的 C1：计入丢弃，不产生事件。
    Require(!tracker.End(MakeEnd(99, false)) && tracker.GetDiscardedCount() == 1, "an unpaired end is counted as discarded");

    // 栈顶项的 C2 丢失：下面一项的 C2 到来时，上面的残留被丢弃，下面这项仍可判定。
    tracker.Begin(MakeBegin(1, SuperWeaponClickMode::Normal, true));
    tracker.Begin(MakeBegin(2, SuperWeaponClickMode::Normal, true));
    const auto launch = tracker.End(MakeEnd(1, false));
    Require(launch && tracker.GetDepth() == 0 && tracker.GetDiscardedCount() == 2,
            "a stale top entry is dropped when a lower entry completes");

    // 帧边界清理残留。
    tracker.Begin(MakeBegin(3, SuperWeaponClickMode::Normal, true));
    tracker.Begin(MakeBegin(4, SuperWeaponClickMode::Normal, true));
    tracker.Clear();
    Require(tracker.GetDepth() == 0 && tracker.GetDiscardedCount() == 4, "Clear drops and counts the leftovers");

    // 栈满时丢弃最旧的。
    for (std::uint64_t token = 0; token < SuperWeaponFireTracker::MAX_DEPTH + 3; ++token)
        tracker.Begin(MakeBegin(100 + token, SuperWeaponClickMode::Normal, true));
    Require(tracker.GetDepth() == SuperWeaponFireTracker::MAX_DEPTH && tracker.GetDiscardedCount() == 7,
            "the stack is bounded and counts overflow");
    Require(!tracker.End(MakeEnd(100, false)), "the oldest snapshot was dropped by the overflow");
}

void CheckHiddenWeaponFilter()
{
    using namespace rareplay::stats;
    // SW.ShowCameo=false 的隐藏辅助超武不报告；缺省与 true 都显示。
    Require(!IsSuperWeaponReportable(false), "a weapon with SW.ShowCameo=false is filtered out");
    Require(IsSuperWeaponReportable(true), "a visible weapon is reported");
}
}

int main()
{
    try
    {
        CheckNormalSuperWeapon();
        CheckChargeDrain();
        CheckTwoStepSuperWeapon();
        CheckTrackerAndIconSource();
        CheckRecursion();
        CheckLossHandling();
        CheckHiddenWeaponFilter();
        std::cout << "Super weapon launch tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
