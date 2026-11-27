#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// 进程内重开要还原的首次开局会话标量。本头文件只含值类型与纯逻辑，不含 YRpp；
// plugin 经 GameApi 取得并持有它，离线测试只链接 ReplayGameApi 中不含 YRpp 的目标文件。
namespace rareplay::game_api::native
{
/**
 * 首次载入时（预世界等待点）spawner 已设定的会话标量。进程内重开时只用它还原这些标量，不复制、不恢复完整 Session。
 * 场景文件名是复制出的字符串，不引用引擎内存，可在世界拆除后继续使用。
 */
struct ScenarioSessionSnapshot
{
    // SessionClass 的四个节奏字段（+0x318、+0x31C、+0x330、+0x320）；Timing 事件会在对局中改写它们。
    std::int32_t mMaxAhead = 0;
    std::int32_t mFrameSendRate = 0;
    std::int32_t mMaxMaxAhead = 0;
    std::int32_t mDesiredFrameRate = 0;
    /** SpecialRulesFlagsCache 原值；Special 事件会在对局中改写它。 */
    std::uint32_t mSpecialRulesFlags = 0;
    /** 当时的 SessionClass::GameMode 原值（联机开局期间为 4 的，重开时先置 4，载入后改回 3）。 */
    std::uint32_t mGameMode = 0;
    /** SessionClass::MultiplayerObserver（Session+0x300）原值；本方被击败时原生置 1，只有原生新开局复位。 */
    std::int32_t mMultiplayerObserver = 0;
    /**
     * Scenario 的 FileName 副本（不含结尾的 NUL），非空、不超过 259 字节。快照点在 StartScenario 内部，
     * 此时 StartScenario 已把传入的名字复制进来并转为大写。
     */
    std::string mScenarioFileName;
};

/**
 * 从引擎的定长字符数组复制场景文件名。text 为空、capacity 为 0、前 capacity 个字节内没有 NUL（数组未终止），
 * 或名称为空时返回空；否则返回 NUL 之前的全部字节。只读 text 的前 capacity 个字节，不假定其编码。
 * 内存不足时向调用方抛出 std::bad_alloc。
 */
std::optional<std::string> CopyScenarioFileName(const char* text, std::size_t capacity);

/**
 * 快照能否用于进程内重开。GameMode 为 0（战役）时不能：战役模式下读场景不调用 ClearScenario，旧世界不会被拆除，
 * 首版不支持。场景文件名为空时也不能。
 */
bool CanRestartFromSnapshot(const ScenarioSessionSnapshot& snapshot) noexcept;

/**
 * 逐项比对两份快照，返回不同的项，格式为“名称=期望/实际”，以空格分隔（文件名的两侧加引号）；完全一致时为空串。
 * 用于进程内重开的载入经过快照点时自检：还原正确时，载入期间的变换对快照值幂等，两份应当相等。技术日志用英文名称。
 */
std::string DescribeSnapshotDifferences(const ScenarioSessionSnapshot& expected, const ScenarioSessionSnapshot& actual);
}
