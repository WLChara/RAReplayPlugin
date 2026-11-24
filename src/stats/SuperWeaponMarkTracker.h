#pragma once

#include "../viewer/SuperWeaponMarks.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rareplay::stats
{
/** 一个仍在显示的超武落点：不变信息与落点的世界坐标（投影由适配层每个迭代重新做）。 */
struct ActiveSuperWeaponMark
{
    std::shared_ptr<const viewer::SuperWeaponMarkInfo> mInfo;
    viewer::WorldPoint mWorld;
};

/**
 * 仍在显示的超武落点集合；只在游戏线程使用，录制时不创建。落点从释放帧完成时起显示
 * viewer::SUPERWEAPON_MARK_LIFETIME_MS 游戏时间，之后由 Prune 丢弃。它不创建任何游戏对象。
 */
class SuperWeaponMarkTracker final
{
public:
    void Reset() noexcept { mMarks.clear(); }

    /** 新增落点；同时存在的落点超过 viewer::SUPERWEAPON_MARK_LIMIT 时丢弃最旧的。 */
    void Add(std::shared_ptr<const viewer::SuperWeaponMarkInfo> info, const viewer::WorldPoint& world);

    /** 丢弃年龄已达显示时长的落点；返回是否丢弃了任何一个。gameTimeMs 为当前游戏时间。 */
    bool Prune(std::int64_t gameTimeMs);

    /** 按释放先后排列，最旧的在前。 */
    const std::vector<ActiveSuperWeaponMark>& GetActive() const noexcept { return mMarks; }

private:
    std::vector<ActiveSuperWeaponMark> mMarks;
};
}
