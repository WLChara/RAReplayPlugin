#pragma once

#include "../src/viewer/ReplayViewModel.h"
#include "../src/viewer/SuperWeaponMarks.h"

#include <cstdint>
#include <memory>

namespace rareplay::tools
{
struct ReplayHudDemoOptions
{
    bool mMissingData = false;
    /** 示例游戏时间是否带“名义”标记（单机速度 0 的情形）。 */
    bool mNominalTime = false;
    /** 示例游戏画布尺寸；面板区域按原生 SidebarBounds 规则放在画布右侧 y ≥ 158。 */
    int mCanvasWidth = 1600;
    int mCanvasHeight = 900;
    /** 是否附带示例雷达事件历史（提示栏）；默认不附带，使以往的预览截图保持不变。 */
    bool mRadarEvents = false;
    /** 示例“快进中”状态：从第 21 000 帧跳向第 52 000 帧，当前停在第 28 540 帧（约 24%）。 */
    bool mSeeking = false;
};

/** 生成只用于 UI 预览的值数据，不读取游戏资源或运行时对象。 */
std::shared_ptr<viewer::ReplayViewSnapshot> CreateReplayHudDemoSnapshot(const ReplayHudDemoOptions& options);

/**
 * 示例雷达事件历史（超武释放与建筑被渗透，含无图标的超武与中立建筑），按发生顺序、最新在末尾。
 * 事件年龄相对 gameTimeMs：最近 30 秒内有 5 条（最后一条正在淡出），其余更早的用来演示历史列表与滚动。
 * 图标是程序绘制的示例图，不是游戏素材。
 */
std::shared_ptr<const viewer::RadarEventList> CreateReplayHudDemoRadarEvents(std::int64_t gameTimeMs);

/**
 * 示例落点图标帧：战术视图为 {0, 0, 画布宽 - 168, 画布高 - 32}，落点按视图比例摆放，含重叠的一对、
 * 正在淡出的一个、无图标的通用标记与一个视图之外的落点。年龄相对 gameTimeMs。
 */
std::shared_ptr<const viewer::SuperWeaponMarkFrame> CreateReplayHudDemoMarks(const ReplayHudDemoOptions& options,
    std::int64_t gameTimeMs);
}
