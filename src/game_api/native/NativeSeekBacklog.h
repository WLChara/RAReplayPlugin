#pragma once

#include <cstdint>

// 快进（不调用 DrawOnTop）期间只在绘制路径中消费、因而会持续积压的渲染簿记。都只涉及画面，不影响模拟与帧 CRC。
// 本头文件只含值类型与声明，不含 YRpp。函数只在游戏线程、世界存在时调用，plugin 经 GameApi 使用；
// 值类型也供 plugin 的快进纯逻辑（清理时机与阈值、诊断累计）直接包含，离线测试不链接 game_api。
namespace rareplay::game_api::native
{
/** 各积压结构的当前条数；读取本身不改动游戏状态。 */
struct SeekBacklogCounts
{
    /** 战术区脏矩形。 */
    std::int32_t mDirtyAreas = 0;
    /** 电弧（EBolt）。 */
    std::int32_t mEBolts = 0;
    /** 辐射与地裂光束（RadBeam）。 */
    std::int32_t mRadBeams = 0;
    /** 物体尾迹（LineTrail），含仍挂在物体上的。 */
    std::int32_t mLineTrails = 0;
    /** 雷达事件。 */
    std::int32_t mRadarEvents = 0;
};

/** 一次清理要做的事；各项互相独立，按声明顺序执行。 */
struct SeekBacklogCleanup
{
    /** 脏矩形计数写 0（保留 Items 与容量）；不完整重绘时之后由完整重绘覆盖整个战术区。 */
    bool mClearDirtyAreas = false;
    /** 调用原生 EBolt_DeleteAll：清除拥有者的反向指针后全部删除。 */
    bool mDeleteEBolts = false;
    /** 按原生过期的做法逐个清除反向指针并删除全部光束，最后计数写 0（保留 Items 与容量）。 */
    bool mDeleteRadBeams = false;
    /** 删除拥有者已解绑的尾迹（+4 为空），其余记录保持原顺序。不调用原生 LineTrail::DeleteAll（它会断开物体的尾迹指针）。 */
    bool mDeleteDetachedLineTrails = false;
    /** 仍挂在物体上的尾迹：32 个点的透明度全部写 0，物体再移动时原生从当前位置重新画起。 */
    bool mHideAttachedLineTrails = false;
    /** 调用原生 ClearRadarEvents 删除全部雷达事件。 */
    bool mClearRadarEvents = false;
    /** 屏震 ScreenShakeX/Y 写 0。 */
    bool mClearScreenShake = false;
};

/** 一次清理实际删除的条数。 */
struct SeekBacklogCleanupResult
{
    std::int32_t mEBoltsDeleted = 0;
    std::int32_t mRadBeamsDeleted = 0;
    std::int32_t mLineTrailsDeleted = 0;
    std::int32_t mRadarEventsCleared = 0;
};

SeekBacklogCounts ReadSeekBacklogCounts() noexcept;

/**
 * 按 cleanup 执行清理。只在快进期间、绘制之外、模拟之外的游戏线程调用（帧完成点，或兜底重绘前的绘制门控）：
 * 与原生过期一样只释放渲染簿记，不消耗场景随机数，不分配引擎对象 ID。
 */
SeekBacklogCleanupResult CleanSeekBacklog(const SeekBacklogCleanup& cleanup) noexcept;
}
