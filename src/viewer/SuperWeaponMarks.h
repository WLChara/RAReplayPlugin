#pragma once

#include "ReplayViewModel.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rareplay::viewer
{
/**
 * 一个超武落点图标的不变部分：释放时确定，之后只被引用。mId 与对应的雷达事件编号相同。
 * 图标缺失（MO 中可见超武约有 27 个没有图标）时 mIcon 为空，界面画通用标记并显示 mLabel。
 */
struct SuperWeaponMarkInfo
{
    std::uint64_t mId = 0;
    /** 释放那一帧完成时的游戏时间；年龄 = 当前游戏时间 − 它。 */
    std::int64_t mReleasedAtMs = 0;
    /** 释放方的颜色，用于图标下方的底标。 */
    std::uint32_t mOwnerColorRgb = 0x8A8F86;
    std::string mLabel;
    std::shared_ptr<const IconImage> mIcon;
};

/** 落点在游戏画布中的位置，原点为 WindowBounds 左上角；可以在战术视图之外。 */
struct CanvasPoint
{
    int mX = 0;
    int mY = 0;
};

struct SuperWeaponMark
{
    std::shared_ptr<const SuperWeaponMarkInfo> mInfo;
    CanvasPoint mPosition;
};

/**
 * 游戏线程在每个主循环迭代发布的落点屏幕位置。它独立于约 100 ms 一次的快照：观看者滚动地图时
 * 图标要贴住地面，所以位置必须与镜头同步更新。位置来自游戏自己的 TacticalClass::CoordsToClient，
 * 渲染线程不重复实现投影公式，只把画布坐标换算到客户区。发布后不可修改。
 */
struct SuperWeaponMarkFrame
{
    std::uint64_t mSessionId = 0;
    /** 投影当时的游戏时间；渲染线程以它计算淡出，暂停时它不前进，图标也就不淡出。 */
    std::int64_t mGameTimeMs = 0;
    int mCanvasWidth = 0;
    int mCanvasHeight = 0;
    /** 战术视图（地图区）在画布中的范围；落点图标只在此范围内绘制，超出部分被裁剪。 */
    NativeRect mView;
    /** 按释放先后排列，最旧的在前。 */
    std::vector<SuperWeaponMark> mMarks;
};
}
