#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rareplay::stats
{
/**
 * 超武释放的判定逻辑：只含值与纯逻辑，不访问引擎。判定依据：
 * 不存在既必经又不与 Ares 重叠的单一点，所以在 HouseClass::Fire_SW 调用 ClickFire 的前后各观察一次（Hook C1、C2），
 * 以两次观察到的超武状态变化判定“这一次是否真的释放”。
 */

/** 超武的点击方式，由适配层按 SuperWeaponTypeClass 的标志推出；同时设置多个标志时 ChargeDrain 优先，其次 PostClick。 */
enum class SuperWeaponClickMode
{
    /** 普通超武：发射后 IsReady 由 1 变 0。 */
    Normal,
    /** 持续放能（例如防火墙）：开启时 ChargeDrainState 由 Ready 变 Draining，关闭时不算释放。 */
    ChargeDrain,
    /** 两段式超武的第一段（超时空传送选源格）：不算释放。 */
    PreClick,
    /** 两段式超武的第二段（真正传送）：以此帧为释放帧，图标改用前置超武。 */
    PostClick
};

/** 游戏 ChargeDrainState 枚举的数值（None=-1、Charging=0、Ready=1、Draining=2）。 */
constexpr int CHARGE_DRAIN_READY = 1;
constexpr int CHARGE_DRAIN_DRAINING = 2;

struct CellPoint
{
    std::int32_t mX = 0;
    std::int32_t mY = 0;
};

/** Fire_SW 调用 ClickFire 之前（Hook C1）读到的值。 */
struct SuperWeaponFireBegin
{
    /** 超武对象地址，只用于把 C1 与 C2 配对，绝不解引用。 */
    std::uint64_t mSuperToken = 0;
    std::uint32_t mFrame = 0;
    int mHouseIndex = -1;
    /** 超武类型在 SuperWeaponTypeClass::Array 中的下标。 */
    int mTypeIndex = -1;
    SuperWeaponClickMode mMode = SuperWeaponClickMode::Normal;
    bool mIsReady = false;
    int mChargeDrainState = -1;
    /** 前置依赖超武（Ares 记录的最近一次 PreClick 超武）；没有时为假。 */
    bool mHasPrerequisite = false;
    int mPrerequisiteTypeIndex = -1;
    bool mPrerequisiteIsReady = false;
    /** 玩家点击或 AI 选定的目标格；适配层在 C1 立即复制（AI 路径的格位于 Ares 栈上）。 */
    CellPoint mTargetCell;
};

/** Fire_SW 调用 ClickFire 返回之后（Hook C2）读到的值。 */
struct SuperWeaponFireEnd
{
    std::uint64_t mSuperToken = 0;
    bool mIsReady = false;
    int mChargeDrainState = -1;
};

/** 判定为已释放的一次超武发射。 */
struct SuperWeaponLaunch
{
    std::uint32_t mFrame = 0;
    int mHouseIndex = -1;
    int mTypeIndex = -1;
    /**
     * 提供图标与名称兜底的类型：PostClick 超武自己没有图标，改用前置超武；其余为 mTypeIndex。
     * PostClick 但没有前置超武时回退为 mTypeIndex。
     */
    int mIconTypeIndex = -1;
    SuperWeaponClickMode mMode = SuperWeaponClickMode::Normal;
    CellPoint mTargetCell;
};

/**
 * 按观察到的前后状态判定这一次是否真的释放：
 * - ChargeDrain：C1 时状态为 Ready 且 C2 时为 Draining；
 * - PostClick：C1 时前置超武存在且已就绪（推断：无法区分之后资金不足或 AbortFire 中止）；
 * - PreClick：不算；
 * - 普通：C1 时 IsReady，C2 时不再 IsReady。
 * 两次观察的超武对象不同时不算。
 */
bool IsSuperWeaponLaunched(const SuperWeaponFireBegin& before, const SuperWeaponFireEnd& after) noexcept;

/**
 * 超武发射观察的快照栈。触发器可能在 Fire_SW 内同步再次调用 Fire_SW（推测情形），
 * 所以用栈而不是单个槽位：C1 压入、C2 弹出，配对按超武对象。栈有上限，C2 缺失造成的残留由 Clear 清理。
 */
class SuperWeaponFireTracker final
{
public:
    static constexpr std::size_t MAX_DEPTH = 16;

    /** 压入一次 C1 观察；栈满时丢弃最旧的一项并计入 GetDiscardedCount。 */
    void Begin(const SuperWeaponFireBegin& before);

    /**
     * 处理一次 C2 观察：从栈顶向下找到同一超武对象的最近一次 C1 并弹出它；其上的残留项视为丢失并丢弃。
     * 找不到配对项时计入丢弃。判定为已释放时返回该次发射。
     */
    std::optional<SuperWeaponLaunch> End(const SuperWeaponFireEnd& after);

    /** 帧边界调用：Fire_SW 是同步调用，帧之间栈必须为空；残留项计入丢弃。 */
    void Clear() noexcept;

    std::size_t GetDepth() const noexcept { return mStack.size(); }
    /** 因栈满、缺少配对或帧边界残留而丢弃的观察次数；供诊断日志使用。 */
    std::uint32_t GetDiscardedCount() const noexcept { return mDiscarded; }

private:
    std::vector<SuperWeaponFireBegin> mStack;
    std::uint32_t mDiscarded = 0;
};

/**
 * 只显示侧边栏可见的超武：规则中 SW.ShowCameo=false 的隐藏辅助超武不报告。
 * 适配层读到的键缺失时按显示处理，所以参数已是最终取值。
 */
constexpr bool IsSuperWeaponReportable(bool showCameo) noexcept
{
    return showCameo;
}
}
