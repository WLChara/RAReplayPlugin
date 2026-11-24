#include "../plugin/ReplayRuntime.h"
#include "../target/TargetAbi.h"

#include <Syringe.h>

// 原生侧栏绘制的跳过点。跳过时直达 Strip 绘制之后，雷达仍由 PowerClass::Draw 尾调绘制。
// 返回 0 由 Syringe 重放被覆盖的完整 cmp（只有绝对位移），其标志供 0x6A6CD2 的 jz 使用。
DEFINE_HOOK(0x6A6CCC, ReplaySidebarChromeHook, 6)
{
    using rareplay::plugin::NativeSidebarAction;
    const auto action = rareplay::plugin::OnNativeSidebarDraw();
    if (action == NativeSidebarAction::SkipChrome)
        return static_cast<DWORD>(rareplay::target::SIDEBAR_CHROME_SKIP_CONTINUE);
    // 插件已置原生重画标志；本次 Draw 的 forceRedraw 字节早已算好，需同步置 1 才会完整重画。
    if (action == NativeSidebarAction::RedrawNative)
        R->Stack8(rareplay::target::SIDEBAR_DRAW_FORCE_REDRAW_STACK, 1);
    return 0;
}
