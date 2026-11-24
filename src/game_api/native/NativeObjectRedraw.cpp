#include "NativeObjectRedraw.h"

// 此版 YRpp 的若干声明仍使用 Win32 的小写 byte 基类型。
using byte = unsigned char;

#include <objidl.h>
// ObjectClass.h 中的 generic_cast 特化需要 TechnoClass、FootClass 的完整定义，故包含派生类头。
#include <FootClass.h>

#include <cstddef>
#include <cstdint>

namespace rareplay::game_api::native
{
namespace
{
// 原生 SetHeight（0x5F5FBB 的 mov al,[esi+74h]）与 ObjectClass::Mark（0x5F5850）按这两个字节判定；YRpp 声明漂移时编译期失败。
static_assert(offsetof(ObjectClass, IsOnMap) == 0x74);
static_assert(offsetof(ObjectClass, InLimbo) == 0x81);

// 原生以 test al,al 按字节判非零；按字节读取，避免非 0/1 值的 bool 语义。
bool IsFlagSet(const bool& flag) noexcept
{
    return reinterpret_cast<const std::uint8_t&>(flag) != 0;
}
}

void MarkPlacedObjectForRedraw(const void* object) noexcept
{
    // Hook 以 const 指针交出寄存器中的对象；MarkForRedraw 只改渲染状态，去掉 const 不改变模拟。
    auto* placed = static_cast<ObjectClass*>(const_cast<void*>(object));
    // 与原生 Mark(Down) 调用 MarkForRedraw 的条件相同：对象在地图上且不在 Limbo。
    if (!placed || !IsFlagSet(placed->IsOnMap) || IsFlagSet(placed->InLimbo))
        return;
    // 虚调用（虚表 +0x134，ObjectClass 实现为 0x5F4D10），与原生 Mark(Down) 的调用方式相同。
    placed->MarkForRedraw();
}
}
