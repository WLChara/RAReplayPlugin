#pragma once

#include "ScenarioSessionSnapshot.h"

#include <cstdint>
#include <optional>
#include <string>

// 进程内重开（同进程重新播放与向后跳转）的原生入口。本头文件不含 YRpp；plugin 经 GameApi 使用。
namespace rareplay::game_api::native
{
/** 一次原生重开的结果与诊断，由 plugin 写成技术日志。 */
struct ScenarioRestartReport
{
    /** 失败的步骤与原因（技术日志，英文，以“step N”开头）；成功时为空。 */
    std::string mError;
    /** 第 5 步删除的辐射光束数。 */
    std::int32_t mDeletedRadBeams = 0;
    /** 第 6 步在 StartScenario 之前读到的 ScenarioInit（应为 0；不为 0 时已置 0）。 */
    std::int32_t mScenarioInitBeforeStart = 0;
    /** 第 8 步是否重建了联机连接（快照 GameMode 为 3 或 4）。 */
    bool mHasRebuiltConnections = false;
    /** 第 8 步删除的连接数，以及删除停止后仍剩的连接数（应为 0）。 */
    std::int32_t mDeletedConnections = 0;
    std::int32_t mRemainingConnections = 0;
};

/**
 * 读取 spawner 已设定的会话标量：四个节奏字段、SpecialRulesFlagsCache、GameMode、Session+0x300 与场景文件名副本。
 * 只在开局载入的预世界等待点（StartScenario 内、等待其他玩家之前）由游戏线程调用，不要求世界就绪；只读。
 * 场景不存在或文件名数组未终止时返回空。不抛异常。
 */
std::optional<ScenarioSessionSnapshot> ReadScenarioSessionSnapshot() noexcept;

/**
 * 进程内重开开始拆除之前的原生只读检查：g_GameActive 不为 0（为 0 时 GAME_OVER_CALL 之后的原生处理会结束会话，
 * 进入对局状态也不会生效）、场景对象存在，且 snapshot 可用于重开（不是战役、文件名长度合法）。
 * 不满足时返回 false 并写 outReason（技术日志，英文）。只在游戏线程调用；不写任何游戏状态。不抛异常。
 */
bool CheckScenarioRestartReady(const ScenarioSessionSnapshot& snapshot, std::string& outReason) noexcept;

/**
 * 进程内重开的原生序列，十步依次为：
 * 1 忙碌计数加一（作用域守卫保证任何返回路径都减一）；2 场景已开始、游戏激活标志置 0；3 FinalizeSessionCleanupIfPending；
 * 4 清空 DoList 与 OutList，Timing 时间戳与同步目标帧清零，用 snapshot 还原四个节奏字段、SpecialRulesFlagsCache 与 Session+0x300，
 *   全局与局部变量的值及变化标志直接写 0；5 删除全部辐射光束；6 InitRandom，GameMode 置快照值，确认 ScenarioInit 为 0；
 * 7 StartScenario(文件名副本, 不播影片, −1)；8 ScenarioInit 与闪电风暴结束标志置 0，快照 GameMode 为 3 或 4 时 GameMode 置 3、
 *   删除全部 IPX 连接并 CreateConnections；9 按开局前奏写特殊对话状态、输入锁快照与 0x822CFC，然后进入对局状态；
 * 10 清零四个退出与重开请求字节。不调用 ClearScenario、EvadeClass::Do，也不切换分辨率。
 *
 * 前置条件：只在游戏线程、GAME_OVER_CALL 执行点（主循环两次迭代之间）调用；CheckScenarioRestartReady 已通过；
 * 调用方已完成插件收尾（快进已结束，战局缩放已停用并回到物理状态，统计与雷达已停，HUD 快照已撤回，运行时已回到“尚未开始”）。
 * 成功返回 true：新场景已载入、停在第 0 帧之前，下一次主循环由插件开始播放。
 * 失败返回 false 并在 outReport.mError 写明哪一步：世界可能已经半拆，调用方必须按播放失败处理并退出回放。不抛异常。
 */
bool RestartScenarioInProcess(const ScenarioSessionSnapshot& snapshot, ScenarioRestartReport& outReport) noexcept;
}
