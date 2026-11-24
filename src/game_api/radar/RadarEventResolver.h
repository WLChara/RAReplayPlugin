#pragma once

#include "../icons/IconStats.h"
#include "../icons/CameoReader.h"
#include "../../stats/InfiltrationObservation.h"
#include "../../stats/RadarEventLog.h"
#include "../../stats/SuperWeaponLaunch.h"

#include <optional>
#include <unordered_map>

namespace rareplay::game_api
{
/**
 * 把观察点记下的整数（下标、格坐标）解析成雷达事件所需的值：玩家名称与颜色、超武与建筑名称、超武图标、
 * 世界坐标，并应用 SW.ShowCameo 过滤。只在游戏线程的采样点（帧完成时）使用，不在 Hook 回调里使用；
 * 全程只读：不创建游戏对象，不调用 MapClass::GetCellAt（它在越界时会写入）。
 * 一个播放会话持有一个实例；图标缓存随 Reset 清空。
 */
class RadarEventResolver final
{
public:
    void Reset() noexcept;
    /** 每次采样（解析一批事件）开始时调用，刷新新类型的图标读取预算。 */
    void BeginSample() noexcept;

    /**
     * 解析一次已判定的超武释放。该超武规则里 SW.ShowCameo=false（隐藏的辅助超武）、类型或房屋下标已失效时
     * 返回空；PostClick 超武的图标与缺失名称取自前置超武。图标缺失时 mIcon 为空，界面画通用标记。
     */
    std::optional<stats::RadarEventDraft> ResolveLaunch(const stats::SuperWeaponLaunch& launch);

    /** 解析一次间谍渗透。建筑类型下标已失效时返回空；所属房屋缺失或不是参战玩家时记为“中立”。 */
    std::optional<stats::RadarEventDraft> ResolveInfiltration(const stats::BuildingInfiltrationObservation& observation);

    const IconStats& GetIconStats() const noexcept { return mIcons.GetStats(); }

private:
    bool IsShownInSidebar(int typeIndex);

    CameoReader mIcons;
    /** 每个超武类型的 SW.ShowCameo 取值缓存；规则在一局内不变。 */
    std::unordered_map<int, bool> mShowCameo;
};
}
