#include "NativeTooltips.h"

using byte = unsigned char;

// YRpp 的绘图头依赖 COM 接口声明，必须先于 CCToolTip.h 引入。
#include <objidl.h>
#include <CCToolTip.h>

#include <cstddef>

namespace rareplay::game_api
{
namespace
{
// IDA 只读核对 0x7241A0（YRpp ToolTipManager::SetState）：__thiscall，单个 bool 栈参数，retn 4；
// 与 [ecx+0Ch] 比较并写入新状态，关闭时 KillTimer 并经虚表 +8 收起当前提示。IsActive 偏移必须与此一致。
static_assert(offsetof(ToolTipManager, IsActive) == 0x0C);
}

std::optional<bool> ReadNativeTooltipState() noexcept
{
    const CCToolTip* tooltip = CCToolTip::Instance;
    if (tooltip == nullptr)
        return std::nullopt;
    return tooltip->IsActive;
}

void SetNativeTooltipState(bool isActive) noexcept
{
    if (CCToolTip* tooltip = CCToolTip::Instance)
        tooltip->SetState(isActive);
}
}
