#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rareplay::stats
{
/**
 * 游戏适配层解析好、尚未编号与打时间戳的雷达事件。玩家名称、颜色、超武与建筑名称、图标都已复制成值，
 * 日志不再访问引擎。
 */
struct RadarEventDraft
{
    viewer::RadarEventKind mKind = viewer::RadarEventKind::SuperWeaponLaunched;
    viewer::RadarPlayerRef mActor;
    viewer::RadarPlayerRef mVictim;
    std::string mSubjectName;
    std::shared_ptr<const viewer::IconImage> mIcon;
    std::optional<viewer::WorldPoint> mLocation;
};

/** 中立或无主房屋的显示名称。 */
constexpr const char NEUTRAL_PLAYER_NAME[] = "中立";

/** 没有所属房屋（或房屋读不到）时使用：下标 -1、名称“中立”、灰色。 */
viewer::RadarPlayerRef MakeNeutralPlayerRef();

/** 超武释放事件：actor 为释放方，location 为落点的世界坐标；icon 缺失时界面画通用标记。 */
RadarEventDraft MakeSuperWeaponLaunchDraft(viewer::RadarPlayerRef actor, std::string superWeaponName,
    std::shared_ptr<const viewer::IconImage> icon, const viewer::WorldPoint& location);

/**
 * 建筑被间谍渗透事件：spyOwner 为间谍所属方，buildingOwner 为被渗透建筑的所属方（无主传 MakeNeutralPlayerRef()），
 * location 为建筑位置。工程师占领与 C4 破坏者不会走到这里（观察点已排除）。
 */
RadarEventDraft MakeInfiltrationDraft(viewer::RadarPlayerRef spyOwner, viewer::RadarPlayerRef buildingOwner,
    std::string buildingName, const viewer::WorldPoint& location);

/**
 * 一次回放播放会话的雷达事件历史唯一所有者；只在游戏线程使用，录制时不创建。
 * 事件按发生顺序追加，至多保留 viewer::RADAR_EVENT_HISTORY_LIMIT 条，超出时丢弃最旧的。
 */
class RadarEventLog final
{
public:
    void Reset();

    /**
     * 追加一条事件并返回它的编号（从 1 开始、会话内单调递增，丢弃旧事件后也不复用）。
     * gameTimeMs 取事件所在帧完成时的游戏时钟；调用方保证它不小于上一条的时间。
     */
    std::uint64_t Add(RadarEventDraft draft, std::int64_t gameTimeMs);

    /** 按编号查找仍在历史中的事件；已被丢弃或不存在时返回空。指针在下一次 Add 或 Reset 之前有效。 */
    const viewer::RadarEvent* Find(std::uint64_t id) const noexcept;

    std::size_t GetCount() const noexcept { return mEvents.size(); }

    /**
     * 不可变的事件列表，发布到快照。没有新事件时多次调用共享同一个对象，
     * 所以每 100 ms 一次的发布不会复制 200 条事件。
     */
    std::shared_ptr<const viewer::RadarEventList> GetSnapshot();

private:
    std::vector<viewer::RadarEvent> mEvents;
    std::uint64_t mNextId = 1;
    std::shared_ptr<const viewer::RadarEventList> mCached;
};
}
