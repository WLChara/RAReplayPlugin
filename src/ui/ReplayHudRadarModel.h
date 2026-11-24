#pragma once

#include "../viewer/RadarDisplayRules.h"
#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * 雷达事件提示栏的界面本地状态与纯逻辑（不含 ImGui）：事件显示年龄（含悬停暂停淡出）、最近与历史两种模式的行选择、
 * 事件文字。状态只影响渲染线程的呈现，游戏线程不需要读取，所以不进入 viewer 快照。
 */
namespace rareplay::ui
{
/**
 * 每条事件的显示年龄。年龄 = 当前游戏时间 − 事件时间 − 该事件被悬停暂停过的时长：
 * 鼠标悬停在提示栏上时，仍在显示的每条事件的年龄冻结（游戏时间照常前进），悬停期间到达的事件也从满不透明开始，
 * 移开后一起继续淡出。暂停时长按事件各自累计，且只累计事件已经存在的那一段，而不是整体平移时钟，
 * 所以悬停期间新到的事件不会多显示一段时间。
 */
class RadarEventAges final
{
public:
    void Reset() noexcept;

    /**
     * 每帧调用一次。gameNowMs 为快照里的游戏时间；isHovered 为鼠标是否在提示栏上。
     * 游戏时间不前进（暂停）或回退时不累计。只对仍在“最近”显示期内的事件累计，年龄已超期的事件不再保留状态。
     */
    void Update(const viewer::RadarEventList* events, std::int64_t gameNowMs, bool isHovered);

    /** 事件的显示年龄（毫秒，不小于 0）；没有被暂停过的事件即为 游戏时间 − 事件时间。 */
    std::int64_t GetAgeMs(const viewer::RadarEvent& event) const noexcept;

    /** 供测试核对：仍保留暂停状态的事件数。 */
    std::size_t GetTrackedCount() const noexcept { return mFrozenMs.size(); }

private:
    std::int64_t mNowMs = 0;
    bool mHasNow = false;
    /** 事件编号 → 已被悬停暂停的游戏时间。 */
    std::unordered_map<std::uint64_t, std::int64_t> mFrozenMs;
};

/** 提示栏的界面本地状态。 */
struct RadarBarState
{
    /** 折叠后提示栏只占标题行。 */
    bool mIsCollapsed = false;
    /** 历史模式：标题行下是可滚动的全部历史（至多 200 条），不淡出；否则只显示最近 5 条。 */
    bool mShowHistory = false;
    /** 上一帧鼠标是否在提示栏上；本帧开头据此决定是否暂停淡出（窗口矩形要在绘制之后才知道悬停）。 */
    bool mWasHovered = false;
    RadarEventAges mAges;
};

/** 提示栏的一行：指向快照里的事件（快照存活期间有效）、显示年龄与不透明度。 */
struct RadarRow
{
    const viewer::RadarEvent* mEvent = nullptr;
    std::int64_t mAgeMs = 0;
    float mOpacity = 1.0f;
};

/**
 * 最近模式：从最新往旧取至多 RADAR_EVENT_RECENT_LIMIT 条仍在 30 秒显示期内的事件，最新的在前；
 * 不透明度随显示年龄线性淡出。没有事件列表时返回空。
 */
std::vector<RadarRow> SelectRecentRadarRows(const viewer::RadarEventList* events, const RadarEventAges& ages);

/** 历史模式：全部事件，最新的在前，不透明度恒为 1（不淡出）。 */
std::vector<RadarRow> SelectHistoryRadarRows(const viewer::RadarEventList* events);

/** 事件文字中的角色，界面据此着色：玩家名称用玩家颜色，其余为普通文字。 */
enum class RadarTextRole { Plain, Actor, Victim };

struct RadarTextSegment
{
    std::string mText;
    RadarTextRole mRole = RadarTextRole::Plain;
};

/**
 * 事件的文字分段：
 * - 超武：“{释放方} 释放了 {超武名称}”；
 * - 渗透：“{间谍所属方} 的间谍渗透了 {建筑所属方} 的 {建筑名称}”，无主建筑的所属方为“中立”。
 * 名称两侧的空格与示例“12:34 玩家A 释放了 闪电风暴”一致；时间由界面另列一栏。
 */
std::vector<RadarTextSegment> BuildRadarEventText(const viewer::RadarEvent& event);

/** 分段拼成的完整文字，用于悬停提示与测试。 */
std::string JoinRadarEventText(const std::vector<RadarTextSegment>& segments);
}
