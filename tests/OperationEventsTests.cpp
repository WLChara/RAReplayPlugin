#include "OperationEventBuilder.h"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::stats;
using namespace rareplay::tests;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

constexpr std::uint8_t MOVE = 2;
constexpr std::uint8_t ATTACK = 1;

// 归类表覆盖全部 47 个 YRpp 事件类型，按类型值有序，且每项都有名称与理由。
void CheckClassificationTable()
{
    const auto& table = GetEventClassifications();
    Require(table.size() == NATIVE_EVENT_TYPE_COUNT && NATIVE_EVENT_TYPE_COUNT == 47, "47 event types");
    std::size_t counted = 0;
    for (std::size_t index = 0; index < table.size(); ++index)
    {
        Require(static_cast<std::size_t>(table[index].mType) == index, "table is ordered by type value");
        Require(table[index].mName != nullptr && table[index].mName[0] != '\0' && table[index].mReason != nullptr &&
                    table[index].mReason[0] != '\0', "every entry has a name and a reason");
        Require(IsOperationEvent(static_cast<std::uint8_t>(index)) == table[index].mIsOperation,
                "IsOperationEvent agrees with the table");
        counted += table[index].mIsOperation ? 1 : 0;
    }
    Require(counted == 24, "24 types count as operations and 23 are excluded");
}

// 合并键表：哪些类型只带执行者、哪些排除执行者、哪些没有载荷，其余按全部载荷；键字段都在载荷范围内。
void CheckMergeRules()
{
    const auto& table = GetEventClassifications();
    const std::set<NativeEventType> executorOnly = {NativeEventType::Idle, NativeEventType::Scatter,
                                                    NativeEventType::Deploy, NativeEventType::Detonate};
    const std::set<NativeEventType> excludingExecutor = {NativeEventType::MegaMission, NativeEventType::MegaMissionF};
    const std::set<NativeEventType> noPayload = {NativeEventType::PlanCommit, NativeEventType::AllCheer};
    for (const EventClassification& entry : table)
    {
        if (!entry.mIsOperation)
        {
            Require(entry.mKeySpanCount == 0, "excluded events have no key");
            continue;
        }
        const MergeBasis expected = executorOnly.contains(entry.mType) ? MergeBasis::ExecutorOnly
            : excludingExecutor.contains(entry.mType) ? MergeBasis::ExcludingExecutor
            : noPayload.contains(entry.mType) ? MergeBasis::NoPayload : MergeBasis::AllPayload;
        Require(entry.mMergeBasis == expected, std::string("merge basis of ") + entry.mName);
        for (std::size_t span = 0; span < entry.mKeySpanCount; ++span)
            Require(entry.mKeySpans[span].mLength > 0 &&
                        entry.mKeySpans[span].mOffset + entry.mKeySpans[span].mLength <= EVENT_PAYLOAD_BYTES,
                    std::string("key span inside the payload: ") + entry.mName);
    }
    // 排除执行者 Whom：MegaMission 的键从任务字节（+5）开始并跳过未初始化的填充（+6）。
    const auto& mega = table[static_cast<std::size_t>(NativeEventType::MegaMission)];
    Require(mega.mKeySpanCount == 2 && mega.mKeySpans[0].mOffset == 5 && mega.mKeySpans[0].mLength == 1 &&
                mega.mKeySpans[1].mOffset == 7 && mega.mKeySpans[1].mLength == 16, "MegaMission key spans");
    const auto& megaF = table[static_cast<std::size_t>(NativeEventType::MegaMissionF)];
    Require(megaF.mKeySpanCount == 1 && megaF.mKeySpans[0].mOffset == 5 && megaF.mKeySpans[0].mLength == 19,
            "MegaMissionF key span");
}

void CheckExclusions()
{
    for (const NativeEventType type : {NativeEventType::FrameInfo, NativeEventType::Timing,
                                       NativeEventType::ProcessTime, NativeEventType::Options, NativeEventType::Exit,
                                       NativeEventType::Empty, NativeEventType::FrameSync, NativeEventType::Message,
                                       NativeEventType::ResponseTime, NativeEventType::MegaFrameInfo,
                                       NativeEventType::PacketTiming, NativeEventType::GameSpeed,
                                       NativeEventType::SaveGame, NativeEventType::Animation, NativeEventType::Special,
                                       NativeEventType::Destruct, NativeEventType::AddPlayer,
                                       NativeEventType::RemovePlayer, NativeEventType::PageUser})
        Require(!IsOperationEvent(static_cast<std::uint8_t>(type)),
                std::string("excluded: ") + GetEventClassifications()[static_cast<std::size_t>(type)].mName);
    for (const NativeEventType type : {NativeEventType::MegaMission, NativeEventType::MegaMissionF,
                                       NativeEventType::Produce, NativeEventType::Place, NativeEventType::Sell,
                                       NativeEventType::SpecialPlace, NativeEventType::Deploy, NativeEventType::PowerOn,
                                       NativeEventType::Ally, NativeEventType::Archive, NativeEventType::PlanCommit,
                                       NativeEventType::AllCheer, NativeEventType::AbandonAll})
        Require(IsOperationEvent(static_cast<std::uint8_t>(type)),
                std::string("counted: ") + GetEventClassifications()[static_cast<std::size_t>(type)].mName);
    for (int type = 47; type <= 255; ++type)
        Require(!IsOperationEvent(static_cast<std::uint8_t>(type)), "unknown type values are not operations");
}

// 系统与设置类事件不计入，也不进入原始事件数。
void CheckExcludedEventsAreNotCounted()
{
    OperationCounter counter;
    for (const NativeEventType type : {NativeEventType::FrameInfo, NativeEventType::Timing, NativeEventType::Exit,
                                       NativeEventType::ProcessTime, NativeEventType::Options})
        Require(!counter.Count(5, MakeSimpleEvent(type, 0)), "system events never count");
    Require(counter.GetOperationCount(0) == 0 && counter.GetRawEventCount(0) == 0, "nothing was recorded");
}

// 同一帧、同一玩家的群体命令：执行者不同、其余参数相同，只算一次操作；原始事件条数照实记录。
void CheckGroupCommandMerges()
{
    OperationCounter counter;
    bool first = false;
    int merged = 0;
    for (std::uint32_t whom = 1; whom <= 20; ++whom)
    {
        const bool isNew = counter.Count(10, MakeMegaMission(0, whom, MOVE, 5000, 6000));
        if (whom == 1)
            first = isNew;
        else
            merged += isNew ? 0 : 1;
    }
    Require(first && merged == 19, "only the first unit's event is a new operation");
    Require(counter.GetOperationCount(0) == 1 && counter.GetRawEventCount(0) == 20,
            "twenty units with one target are one operation and twenty raw events");

    // 同帧不同目标分开计：一次框选两次点击是两次操作；任一参数不同都不合并。
    counter.Count(10, MakeMegaMission(0, 1, MOVE, 5001, 6000));
    counter.Count(10, MakeMegaMission(0, 2, MOVE, 5001, 6000));
    Require(counter.GetOperationCount(0) == 2, "a different target is another operation");
    counter.Count(10, MakeMegaMission(0, 3, ATTACK, 5000, 6000));
    Require(counter.GetOperationCount(0) == 3, "a different mission is another operation");
    counter.Count(10, MakeMegaMission(0, 4, MOVE, 5000, 6001));
    Require(counter.GetOperationCount(0) == 4, "a different destination is another operation");
    counter.Count(10, MakeMegaMission(0, 5, MOVE, 5000, 6000, 77));
    Require(counter.GetOperationCount(0) == 5, "a different follow target is another operation");
    counter.Count(10, MakeMegaMission(0, 6, MOVE, 5000, 6000, 0, true));
    Require(counter.GetOperationCount(0) == 6, "a planning event is another operation");
    Require(counter.GetRawEventCount(0) == 20 + 2 + 4, "raw events keep counting every record");

    // 交错到达也合并：A、B、A 只算两次。
    OperationCounter interleaved;
    interleaved.Count(3, MakeMegaMission(0, 1, MOVE, 1));
    interleaved.Count(3, MakeMegaMission(0, 2, MOVE, 2));
    interleaved.Count(3, MakeMegaMission(0, 3, MOVE, 1));
    Require(interleaved.GetOperationCount(0) == 2, "non-adjacent identical commands still merge");

    // MegaMissionF：速度与最大速度属于键。
    OperationCounter speeds;
    speeds.Count(1, MakeMegaMissionF(0, 1, MOVE, 10, 20, 3, 100));
    speeds.Count(1, MakeMegaMissionF(0, 2, MOVE, 10, 20, 3, 100));
    Require(speeds.GetOperationCount(0) == 1, "MegaMissionF group merges");
    speeds.Count(1, MakeMegaMissionF(0, 3, MOVE, 10, 20, 4, 100));
    speeds.Count(1, MakeMegaMissionF(0, 4, MOVE, 10, 20, 3, 101));
    Require(speeds.GetOperationCount(0) == 3, "speed and max speed separate MegaMissionF commands");
    speeds.Count(1, MakeMegaMission(0, 5, MOVE, 10, 20));
    Require(speeds.GetOperationCount(0) == 4, "MegaMission and MegaMissionF never merge with each other");
}

void CheckExecutorOnlyEventsMerge()
{
    OperationCounter counter;
    for (const NativeEventType type : {NativeEventType::Idle, NativeEventType::Scatter, NativeEventType::Deploy,
                                       NativeEventType::Detonate})
    {
        for (std::uint32_t whom = 1; whom <= 5; ++whom)
            counter.Count(8, MakeTargetEvent(type, 0, whom));
    }
    Require(counter.GetOperationCount(0) == 4 && counter.GetRawEventCount(0) == 20,
            "each executor-only type merges to one operation per frame; types stay separate");
    counter.Count(9, MakeTargetEvent(NativeEventType::Idle, 0, 1));
    Require(counter.GetOperationCount(0) == 5, "the next frame is a new operation");
}

// 按住 Shift 一次排 5 个同类单位：同一帧 5 条相同的 Produce 计为 1 次。
void CheckProductionMerges()
{
    OperationCounter counter;
    for (int index = 0; index < 5; ++index)
        counter.Count(100, MakeProduce(NativeEventType::Produce, 0, 40, 12, 0));
    Require(counter.GetOperationCount(0) == 1 && counter.GetRawEventCount(0) == 5, "shift-queued units are one operation");
    counter.Count(100, MakeProduce(NativeEventType::Produce, 0, 40, 13, 0));
    counter.Count(100, MakeProduce(NativeEventType::Produce, 0, 40, 12, 1));
    counter.Count(100, MakeProduce(NativeEventType::Suspend, 0, 40, 12, 0));
    counter.Count(100, MakeProduce(NativeEventType::Abandon, 0, 40, 12, 0));
    counter.Count(100, MakeProduce(NativeEventType::AbandonAll, 0, 40, 12, 0));
    Require(counter.GetOperationCount(0) == 6, "another product, naval flag or event type is another operation");

    OperationCounter placing;
    placing.Count(7, MakePlace(0, 3, 40, 50));
    placing.Count(7, MakePlace(0, 3, 40, 50));
    Require(placing.GetOperationCount(0) == 1, "an identical placement merges");
    placing.Count(7, MakePlace(0, 3, 41, 50));
    placing.Count(7, MakePlace(0, 4, 40, 50));
    Require(placing.GetOperationCount(0) == 3, "another cell or building is another operation");

    // 出售与修理的 Whom 是被点击的建筑，每个建筑是独立操作；同一建筑的重复才合并。
    OperationCounter buildings;
    for (const NativeEventType type : {NativeEventType::Sell, NativeEventType::Repair, NativeEventType::PowerOn,
                                       NativeEventType::PowerOff, NativeEventType::Primary})
    {
        buildings.Count(2, MakeTargetEvent(type, 0, 11));
        buildings.Count(2, MakeTargetEvent(type, 0, 11));
        buildings.Count(2, MakeTargetEvent(type, 0, 12));
    }
    Require(buildings.GetOperationCount(0) == 10 && buildings.GetRawEventCount(0) == 15,
            "different buildings are separate operations, repeats of one building merge");
}

void CheckHousesAreIndependent()
{
    OperationCounter counter;
    for (std::uint32_t whom = 1; whom <= 3; ++whom)
    {
        counter.Count(4, MakeMegaMission(0, whom, MOVE, 99));
        counter.Count(4, MakeMegaMission(1, whom + 10, MOVE, 99));
    }
    counter.Count(4, MakeMegaMission(127, 1, MOVE, 99));
    Require(counter.GetOperationCount(0) == 1 && counter.GetOperationCount(1) == 1 && counter.GetOperationCount(127) == 1 &&
                counter.GetOperationCount(2) == 0, "the same command from two players is two operations");
    Require(counter.GetRawEventCount(0) == 3 && counter.GetRawEventCount(1) == 3, "raw events stay per house");
}

// 合并键只取有定义的载荷字段：填充字节、未用字节的内容不同不影响合并。
void CheckUndefinedBytesAreIgnored()
{
    OperationCounter counter;
    counter.Count(6, MakeMegaMission(0, 1, MOVE, 50, 60, 0, false, 0x00, 0x00));
    counter.Count(6, MakeMegaMission(0, 2, MOVE, 50, 60, 0, false, 0xA5, 0x3C));
    counter.Count(6, MakeMegaMission(0, 3, MOVE, 50, 60, 0, false, 0x5A, 0xFF));
    Require(counter.GetOperationCount(0) == 1, "padding and unused bytes do not split a group");
    counter.Count(6, MakeProduce(NativeEventType::Produce, 0, 40, 12, 0, 0x00));
    counter.Count(6, MakeProduce(NativeEventType::Produce, 0, 40, 12, 0, 0xEE));
    Require(counter.GetOperationCount(0) == 2, "bytes after the produce fields are ignored");
    counter.Count(6, MakeSimpleEvent(NativeEventType::AllCheer, 0, 0x00));
    counter.Count(6, MakeSimpleEvent(NativeEventType::AllCheer, 0, 0x77));
    counter.Count(6, MakeSimpleEvent(NativeEventType::PlanCommit, 0, 0x00));
    counter.Count(6, MakeSimpleEvent(NativeEventType::PlanCommit, 0, 0x33));
    Require(counter.GetOperationCount(0) == 4, "payload-less events merge by type");
}

void CheckCrossFrameAndInvalidInput()
{
    OperationCounter counter;
    for (std::uint32_t frame = 20; frame < 23; ++frame)
    {
        counter.Count(frame, MakeMegaMission(0, 1, MOVE, 5));
        counter.Count(frame, MakeMegaMission(0, 2, MOVE, 5));
    }
    Require(counter.GetOperationCount(0) == 3 && counter.GetRawEventCount(0) == 6, "frames never merge with each other");

    // 无效房屋下标（-1、-128）、过短的事件不计入。
    Require(!counter.Count(30, MakeMegaMission(-1, 1, MOVE, 5)) && !counter.Count(30, MakeMegaMission(-128, 1, MOVE, 5)),
            "negative house indices are ignored");
    const auto shortEvent = MakeMegaMission(0, 1, MOVE, 5);
    Require(!counter.Count(30, std::span<const std::uint8_t>(shortEvent.data(), shortEvent.size() - 1)),
            "a truncated event is ignored");
    Require(counter.GetOperationCount(0) == 3 && counter.GetRawEventCount(0) == 6, "ignored input changes nothing");
    Require(counter.GetOperationCount(-1) == 0 && counter.GetOperationCount(500) == 0 && counter.GetRawEventCount(500) == 0,
            "out-of-range lookup is zero");
    counter.Reset();
    Require(counter.GetOperationCount(0) == 0 && counter.GetRawEventCount(0) == 0, "reset");
    counter.Count(20, MakeMegaMission(0, 1, MOVE, 5));
    Require(counter.GetOperationCount(0) == 1, "reset also clears the per-frame merge state");
}

void CheckApm()
{
    Require(!ComputeApm(50, 0) && !ComputeApm(50, APM_MIN_GAME_TIME_MS - 1), "no APM before the minimum game time");
    const auto atMinimum = ComputeApm(10, APM_MIN_GAME_TIME_MS);
    Require(atMinimum && std::abs(*atMinimum - 60.0f) < 1e-4f, "10 operations in 10 s is 60 APM");
    const auto oneMinute = ComputeApm(120, 60'000);
    Require(oneMinute && std::abs(*oneMinute - 120.0f) < 1e-4f, "operations divided by game minutes");
    const auto fractional = ComputeApm(90, 90'000);
    Require(fractional && std::abs(*fractional - 60.0f) < 1e-4f, "fractional minutes");
    const auto idle = ComputeApm(0, 120'000);
    Require(idle && *idle == 0.0f, "no operations is zero APM, not missing");
}
}

int main()
{
    try
    {
        CheckClassificationTable();
        CheckMergeRules();
        CheckExclusions();
        CheckExcludedEventsAreNotCounted();
        CheckGroupCommandMerges();
        CheckExecutorOnlyEventsMerge();
        CheckProductionMerges();
        CheckHousesAreIndependent();
        CheckUndefinedBytesAreIgnored();
        CheckCrossFrameAndInvalidInput();
        CheckApm();
        std::cout << "Operation event tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
