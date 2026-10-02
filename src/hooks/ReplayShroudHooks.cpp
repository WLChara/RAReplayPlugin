#include "../game_api/native/NativeShroud.h"
#include "../plugin/ReplayRuntime.h"

#include <Syringe.h>

// GetPixels 的专用 SHROUD 绘制返回点。完整 6 字节 MOV EDX,[ViewBounds.Y]
// 不含相对地址，返回 0 由 Syringe 重放；只替换 EAX，不碰原资源或 FOG 的调用点。
DEFINE_HOOK(0x47F136, ReplayShroudPixelsHook, 6)
{
    if (!rareplay::plugin::ShouldUseHalfShroud())
        return 0;
    try
    {
        const auto* pixels = rareplay::game_api::native::GetViewerShroudPixels(R->EBP(),
            reinterpret_cast<const std::uint8_t*>(R->EAX()));
        R->EAX(reinterpret_cast<DWORD>(pixels));
    }
    catch (...) { /* 显示副本分配失败只回退到原生像素。 */ }
    return 0;
}
