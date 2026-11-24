#include "../game_api/native/NativeObjectRedraw.h"
#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"

#include <Syringe.h>

// ObjectClass::SetHeight 内的 IsOnMap 测试（5 字节 mov al,[esi+74h]; test al,al，无相对位移）。
// AircraftClass::Draw 画影子时临时改高度，原生 Mark 对会把飞机插入或移出格内链表并调用 DiscoveredBy，
// 使模拟取决于观看者画到了哪些飞机（R1）。中和时补做原生 Mark(Down) 的重画请求，再跳到只写 Z 的原生分支；
// 不复制 Z 的公式，不补做 Tether 的 RequestRedraw，不碰 FrozenStill。其余调用者返回 0，由 Syringe 重放这两条指令。
DEFINE_HOOK(0x5F5FBB, ReplayAircraftShadowSetHeightHook, 5)
{
    // SetHeight 被模拟大量调用：先按栈上的返回地址排除其他调用者，只读一次栈、比较三个常量。
    if (!rareplay::target::IsAircraftShadowSetHeightReturn(
            R->Stack<DWORD>(rareplay::target::SET_HEIGHT_RETURN_ADDRESS_STACK)) ||
        !rareplay::plugin::ShouldNeutralizeAircraftShadowSideEffects())
        return 0;
    // ESI = this（被画的飞机）。
    rareplay::game_api::native::MarkPlacedObjectForRedraw(reinterpret_cast<const void*>(R->ESI()));
    return static_cast<DWORD>(rareplay::target::SET_HEIGHT_Z_ONLY);
}
