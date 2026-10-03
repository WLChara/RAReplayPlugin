#include "../plugin/ReplayRuntime.h"

#include <Syringe.h>

// 两个出口均位于原生写入之后；返回 0 让 Syringe 重放 pop edi/pop ebp/pop ebx/ret/nop。
DEFINE_HOOK(0x7310CB, ReplayControlGroupAssignedHook, 5)
{
    rareplay::plugin::OnControlGroupUpdated();
    return 0;
}

DEFINE_HOOK(0x73112B, ReplayControlGroupClearedHook, 5)
{
    rareplay::plugin::OnControlGroupUpdated();
    return 0;
}
