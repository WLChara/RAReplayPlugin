#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"

#include <Syringe.h>

// 超武释放与建筑被渗透的只读观察点（回放界面第二版第 4 阶段）。三个回调只把寄存器与栈上的指针交给插件层，
// 返回 0 由 Syringe 原样执行被覆盖的指令；录制时插件层立即返回，不读任何游戏字段。

// Fire_SW 调用 ClickFire 之前：EBX = 房屋，EDI = 超武，ESI = 前置依赖超武（可为 0），[ESP+0x18] = 目标格指针。
DEFINE_HOOK(0x4FAE85, ReplaySuperWeaponBeforeClickHook, 5)
{
    rareplay::plugin::OnSuperWeaponFireBegin(reinterpret_cast<const void*>(R->EBX()),
        reinterpret_cast<const void*>(R->EDI()), reinterpret_cast<const void*>(R->ESI()),
        reinterpret_cast<const void*>(R->Stack<DWORD>(rareplay::target::SUPERWEAPON_FIRE_STACK_CELL)));
    return 0;
}

// ClickFire 返回之后：EDI 与 EBX 不变，栈布局与上一个点相同。
DEFINE_HOOK(0x4FAE9C, ReplaySuperWeaponAfterClickHook, 9)
{
    rareplay::plugin::OnSuperWeaponFireEnd(reinterpret_cast<const void*>(R->EDI()));
    return 0;
}

// 间谍进入敌方建筑、调用 BuildingClass::Infiltrate 之前：ESI = 间谍步兵，EDI = 被渗透的建筑。
DEFINE_HOOK(0x51A002, ReplayInfiltrateBuildingHook, 6)
{
    rareplay::plugin::OnBuildingInfiltrated(reinterpret_cast<const void*>(R->ESI()),
        reinterpret_cast<const void*>(R->EDI()));
    return 0;
}
