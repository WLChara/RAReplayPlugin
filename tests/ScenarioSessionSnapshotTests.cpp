#include "../src/game_api/native/ScenarioSessionSnapshot.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::game_api::native::CanRestartFromSnapshot;
using rareplay::game_api::native::CopyScenarioFileName;
using rareplay::game_api::native::DescribeSnapshotDifferences;
using rareplay::game_api::native::ScenarioSessionSnapshot;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckCopy()
{
    // 引擎的 FileName 是定长字符数组，后面常留有上一场的残余字节；只取到第一个 NUL 为止。
    char buffer[16] = "TOURN1.MAP";
    std::memcpy(buffer + 11, "OLD", 3);
    const auto name = CopyScenarioFileName(buffer, sizeof(buffer));
    Require(name && *name == "TOURN1.MAP", "the name stops at the first terminator");

    // 结尾恰在最后一个字节：仍算终止。
    char exact[6] = {'A', 'B', 'C', 'D', 'E', '\0'};
    Require(CopyScenarioFileName(exact, sizeof(exact)) == std::string("ABCDE"), "a terminator in the last byte is found");

    // 非 ASCII 字节原样复制（不假定编码）。
    const char bytes[] = {'\xC4', '\xE3', '.', 'm', 'a', 'p', '\0'};
    Require(CopyScenarioFileName(bytes, sizeof(bytes)) == std::string(bytes), "bytes are copied unchanged");
}

void CheckRejects()
{
    Require(!CopyScenarioFileName(nullptr, 16), "a null buffer has no name");
    char buffer[8] = "ABC";
    Require(!CopyScenarioFileName(buffer, 0), "a zero capacity has no name");
    char empty[8] = {};
    Require(!CopyScenarioFileName(empty, sizeof(empty)), "an empty name is rejected");
    // 前 capacity 个字节内没有 NUL：未终止的数组不能越界读，也不能截断成部分名称。
    char unterminated[4] = {'A', 'B', 'C', 'D'};
    Require(!CopyScenarioFileName(unterminated, sizeof(unterminated)), "an unterminated array is rejected");
    Require(!CopyScenarioFileName(buffer, 3), "a terminator beyond the capacity is not used");
    Require(CopyScenarioFileName(buffer, 4) == std::string("ABC"), "the capacity includes the terminator");
}

// 一份联机开局的快照：GameMode 4，节奏字段为 spawner 设定的值，场景名已被 StartScenario 转为大写。
ScenarioSessionSnapshot MakeOnlineSnapshot()
{
    ScenarioSessionSnapshot snapshot;
    snapshot.mMaxAhead = 16;
    snapshot.mFrameSendRate = 4;
    snapshot.mMaxMaxAhead = 16;
    snapshot.mDesiredFrameRate = 60;
    snapshot.mSpecialRulesFlags = 0x00001040u;
    snapshot.mGameMode = 4;
    snapshot.mMultiplayerObserver = 0;
    snapshot.mScenarioFileName = "SPAWNMAP.INI";
    return snapshot;
}

// 前提的纯逻辑部分：战役（GameMode 0）不能重开，其余模式可以；场景名为空时也不能。
void CheckRestartableModes()
{
    auto snapshot = MakeOnlineSnapshot();
    for (const std::uint32_t gameMode : {3u, 4u, 5u})
    {
        snapshot.mGameMode = gameMode;
        Require(CanRestartFromSnapshot(snapshot), "game mode " + std::to_string(gameMode) + " can restart");
    }
    snapshot.mGameMode = 0;
    Require(!CanRestartFromSnapshot(snapshot), "campaign mode cannot restart");
    snapshot = MakeOnlineSnapshot();
    snapshot.mScenarioFileName.clear();
    Require(!CanRestartFromSnapshot(snapshot), "a snapshot without a scenario name cannot restart");
}

// 重开后的自检：完全一致为空串；每一项不同都被列出，格式为“名称=期望/实际”。
void CheckDifferences()
{
    const auto expected = MakeOnlineSnapshot();
    Require(DescribeSnapshotDifferences(expected, expected).empty(), "identical snapshots have no differences");

    struct FieldCase
    {
        const char* mText;
        void (*mChange)(ScenarioSessionSnapshot&);
    };
    const FieldCase cases[] = {
        {"gameMode=4/3", [](ScenarioSessionSnapshot& s) { s.mGameMode = 3; }},
        {"maxAhead=16/24", [](ScenarioSessionSnapshot& s) { s.mMaxAhead = 24; }},
        {"frameSendRate=4/2", [](ScenarioSessionSnapshot& s) { s.mFrameSendRate = 2; }},
        {"maxMaxAhead=16/32", [](ScenarioSessionSnapshot& s) { s.mMaxMaxAhead = 32; }},
        {"desiredFrameRate=60/30", [](ScenarioSessionSnapshot& s) { s.mDesiredFrameRate = 30; }},
        {"specialFlags=4160/4164", [](ScenarioSessionSnapshot& s) { s.mSpecialRulesFlags = 0x00001044u; }},
        {"multiplayerObserver=0/1", [](ScenarioSessionSnapshot& s) { s.mMultiplayerObserver = 1; }},
        {"scenario=\"SPAWNMAP.INI\"/\"spawnmap.ini\"",
            [](ScenarioSessionSnapshot& s) { s.mScenarioFileName = "spawnmap.ini"; }}};
    for (const FieldCase& fieldCase : cases)
    {
        auto actual = expected;
        fieldCase.mChange(actual);
        const auto text = DescribeSnapshotDifferences(expected, actual);
        Require(text == fieldCase.mText, std::string("one difference is reported: ") + text);
    }

    auto actual = expected;
    actual.mMaxAhead = 24;
    actual.mMultiplayerObserver = 1;
    Require(DescribeSnapshotDifferences(expected, actual) == "maxAhead=16/24 multiplayerObserver=0/1",
        "several differences are separated by spaces in field order");
}
}

int main()
{
    try
    {
        CheckCopy();
        CheckRejects();
        CheckRestartableModes();
        CheckDifferences();
        std::cout << "Scenario session snapshot tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
