#pragma once

#include "IconStats.h"
#include "../../assets/IconLoadCache.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class SuperWeaponTypeClass;
class TechnoTypeClass;

namespace rareplay::game_api
{
/** 仅游戏线程调用；只缓存 RGBA 副本，原生资源由引擎自己持有。 */
class CameoReader final
{
public:
    void Clear() noexcept;
    void BeginSample() noexcept;
    std::shared_ptr<const viewer::IconImage> Resolve(const TechnoTypeClass* type);

    /**
     * 超武侧边栏图标，缓存键 sw:<ID>。来源依次为：
     * 1. 规则（INI_Rules）中 [ID] 的 SidebarPCX，转小写后走 PCX 缓存（Ares 已预载入，通常直接命中）；
     * 2. SidebarImage 的 SHP 第一帧；若它就是读取失败后回退的 XXICON.SHP 则视为缺失。
     * 两者都没有时返回空指针，调用方画通用标记。type 可为空。
     * 只在游戏线程的采样点调用，不在 Hook 回调里做图像处理；新类型的读取次数受 BeginSample 起的预算约束。
     */
    std::shared_ptr<const viewer::IconImage> ResolveSuperWeapon(const SuperWeaponTypeClass* type);
    const IconStats& GetStats() const noexcept { return mStats; }

private:
    struct PcxEntry
    {
        std::weak_ptr<const viewer::IconImage> mImage;
        bool mMissing = false;
        bool mInvalidSurface = false;
    };
    std::shared_ptr<const viewer::IconImage> Load(const TechnoTypeClass* type, const std::string& typeId);
    std::shared_ptr<const viewer::IconImage> LoadSuperWeapon(const SuperWeaponTypeClass* type,
        const std::string& typeId);
    assets::IconLoadCache mTypes{1024, 4, 32U * 1024U * 1024U};
    // 超武类型只有一百多个且几乎不变；每次释放最多解析几个，预算放宽到不会在采样点被推迟。
    assets::IconLoadCache mSuperWeapons{256, 64, 16U * 1024U * 1024U};
    std::unordered_map<std::string, PcxEntry> mPcxFiles;
    IconStats mStats;
};
}
