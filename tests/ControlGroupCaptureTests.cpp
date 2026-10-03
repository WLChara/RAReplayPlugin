#include "../src/game_api/control_groups/ControlGroupMembership.h"
#include "../src/game_api/control_groups/ControlGroupRecorder.h"
#include "../src/game_api/control_groups/RecordedControlGroupProjection.h"

#include <stdexcept>

namespace
{
using namespace rareplay;

replay::RecordedControlGroupSnapshot g_Snapshot;
bool g_FailSample = false;
int g_SampleCount = 0;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool ReadSnapshot(replay::RecordedControlGroupSnapshot& outSnapshot, std::string& error)
{
    ++g_SampleCount;
    if (g_FailSample)
    {
        error = "采样失败";
        return false;
    }
    outSnapshot = g_Snapshot;
    return true;
}

void TestFinalFrameCaptureAndChanges()
{
    game_api::ControlGroupRecorder recorder(ReadSnapshot);
    g_Snapshot = {};
    g_FailSample = false;
    g_SampleCount = 0;
    std::string error;
    std::optional<replay::RecordedControlGroupSnapshot> update;
    Require(recorder.CopyFrameUpdate(0, update, error) && update && update->mMembers.empty(),
        "initial empty groups must be present");
    Require(recorder.CopyFrameUpdate(1, update, error) && !update,
        "unchanged empty groups produce no update");
    g_Snapshot.mMembers = {{0, 101, 52}, {0, 102, 52}, {9, 202, 52}};
    Require(recorder.CaptureAfterNativeUpdate(error), "native assignment captures");
    g_Snapshot.mMembers = {{1, 101, 52}, {9, 202, 52}};
    Require(recorder.CaptureAfterNativeUpdate(error) &&
        recorder.CopyFrameUpdate(2, update, error) && update == g_Snapshot,
        "same-frame assignments merge to final overwrite and regroup state");
    const auto lastGroups = update;
    Require(recorder.CopyFrameUpdate(3, update, error) && !update,
        "unchanged members including transported ones need no update");
    // 未发生编队按键时，帧尾采样仍记录死亡或失控导致的成员移除。
    g_Snapshot.mMembers = {{9, 202, 52}};
    Require(recorder.CopyFrameUpdate(4, update, error) && update == g_Snapshot,
        "frame-end lifecycle change is captured without hook notification");
    g_FailSample = true;
    const auto beforeFailure = update;
    Require(!recorder.CopyFrameUpdate(5, update, error) && update == beforeFailure && !error.empty(),
        "native sample failure cannot masquerade as clear groups");
    g_FailSample = false;
    g_Snapshot.mMembers.push_back({9, 202, 52});
    Require(!recorder.CaptureAfterNativeUpdate(error), "invalid native snapshot fails unified validation");
    g_Snapshot.mMembers = {{9, 202, 52}};
    Require(recorder.CopyFrameUpdate(5, update, error) && !update,
        "failed sample must not replace last published state");
    g_Snapshot.mMembers.clear();
    Require(recorder.CaptureAfterNativeUpdate(error) &&
        recorder.CopyFrameUpdate(6, update, error) && update && update->mMembers.empty(),
        "clear emits an explicit empty full snapshot");
    Require(g_SampleCount == 12, "hook and frame end use the same sampling path");
    Require(recorder.GetStats().mNativeUpdateCount == 3 &&
        recorder.GetStats().mSnapshotUpdateCount == 4,
        "successful native notifications and emitted changes are counted separately");
    recorder.Reset();
    Require(recorder.GetStats().mNativeUpdateCount == 0 &&
        recorder.GetStats().mSnapshotUpdateCount == 0, "new capture resets diagnostics");
    Require(recorder.CopyFrameUpdate(0, update, error) && update && update->mMembers.empty(),
        "new capture starts with an explicit initial snapshot");
    Require(lastGroups && lastGroups->mMembers.size() == 2, "captured values own their storage");
}

void TestMembershipPolicy()
{
    using game_api::IsRecordedControlGroupMember;
    for (int group = 0; group < 10; ++group)
        Require(IsRecordedControlGroupMember(group, true, 1, true),
            "alive controlled members remain grouped even while off map");
    Require(!IsRecordedControlGroupMember(-1, true, 100, true) &&
        !IsRecordedControlGroupMember(10, true, 100, true) &&
        !IsRecordedControlGroupMember(0, false, 100, true) &&
        !IsRecordedControlGroupMember(0, true, 0, true) &&
        !IsRecordedControlGroupMember(0, true, -1, true) &&
        !IsRecordedControlGroupMember(0, true, 100, false),
        "invalid group, death and lost control remove membership");
}

void TestHistoricalProjection()
{
    viewer::ReplayViewSnapshot view;
    std::unordered_map<std::uint32_t, viewer::UnitSummary> resolved;
    viewer::UnitSummary tank;
    tank.mTypeId = "MTNK";
    tank.mName = "坦克";
    tank.mCategory = viewer::UnitCategory::Vehicle;
    tank.mCount = 1;
    resolved.emplace(101, tank);
    resolved.emplace(102, tank);
    std::optional<replay::RecordedControlGroupSnapshot> groups =
        replay::RecordedControlGroupSnapshot{{{0, 101, 52}, {0, 102, 52}, {9, 202, 52}}};
    game_api::ProjectRecordedControlGroups(groups, resolved, view);
    Require(view.mHasRecordedGroups && view.mRecordedGroups.size() == 2 &&
        view.mRecordedGroups[0].mNumber == 1 && view.mRecordedGroups[0].mUnits.size() == 1 &&
        view.mRecordedGroups[0].mUnits[0].mCount == 2 &&
        view.mRecordedGroups[0].mUnits[0].mName == "坦克" &&
        view.mRecordedGroups[1].mNumber == 10 && view.mRecordedGroups[1].mUnits.empty() &&
        view.mRecordedGroups[1].mUnmatchedMemberCount == 1,
        "projection aggregates matched IDs and preserves unresolved group membership");
    groups->mMembers.clear();
    game_api::ProjectRecordedControlGroups(groups, resolved, view);
    Require(view.mHasRecordedGroups && view.mRecordedGroups.empty(), "recorded empty groups stay distinct");
    groups.reset();
    game_api::ProjectRecordedControlGroups(groups, resolved, view);
    Require(!view.mHasRecordedGroups && view.mRecordedGroups.empty(), "v2 has no historical groups");
}
}

int main()
{
    TestFinalFrameCaptureAndChanges();
    TestMembershipPolicy();
    TestHistoricalProjection();
}
