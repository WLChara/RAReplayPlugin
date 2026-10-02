#include "NativeShroud.h"

#include "../../assets/ShroudPixels.h"
#include "../../target/TargetAbi.h"

using byte = unsigned char;

#include <objidl.h>
#include <FileFormats/SHP.h>
#include <Surface.h>
#include <TacticalClass.h>

namespace rareplay::game_api::native
{
namespace
{
// 仅游戏线程访问；进程只有一个观看会话，退出与资源身份变更明确释放其副本。
assets::ShroudFrameCache g_ShroudCache;

assets::ShroudFramePixels ReadShroudFrame(std::uintptr_t nativeFrame,
    const std::uint8_t* originalPixels)
{
    assets::ShroudFramePixels result;
    result.mPixels = originalPixels;
    // 47F136 时 GetPixels 已返回，原 EBP 帧中 -4 保存资源，+10h 是帧号。
    const auto* source = *reinterpret_cast<SHPStruct* const*>(nativeFrame - 4);
    const int frame = *reinterpret_cast<const int*>(nativeFrame + 0x10);
    if (!source || !originalPixels || source != *reinterpret_cast<SHPStruct* const*>(target::SHROUD_RESOURCE))
        return result;
    result.mResourceIdentity = reinterpret_cast<std::uintptr_t>(source);
    // GetPixels 已解析资源；仅读现有 Data，避免在绘制适配中另行加载或解压。
    const auto* reference = source->AsReference();
    const auto* data = reference ? (reference->Data ? reference->Data->AsFile() : nullptr) : source->AsFile();
    result.mDataIdentity = reinterpret_cast<std::uintptr_t>(data);
    if (!data || frame < 0 || frame >= data->Frames || source->HasCompression(frame))
        return result;
    const auto& header = data->GetFrameHeader(frame);
    if (header.Offset <= 0 || originalPixels != reinterpret_cast<const std::uint8_t*>(data) + header.Offset)
        return result;
    RectangleStruct bounds{};
    source->GetFrameBounds(bounds, frame);
    result.mFrame = frame;
    result.mWidth = bounds.Width;
    result.mHeight = bounds.Height;
    result.mIsPlain = true;
    return result;
}
}

const std::uint8_t* GetViewerShroudPixels(std::uintptr_t nativeFrame,
    const std::uint8_t* originalPixels)
{
    const auto frame = ReadShroudFrame(nativeFrame, originalPixels);
    return frame.mResourceIdentity ? g_ShroudCache.GetPixels(frame, true) : originalPixels;
}

void InvalidateViewerShroud()
{
    g_ShroudCache.Clear();
    // 与原生 ABuffer 使用同一 ViewBounds 原点；true 要求重新生成遮罩显示缓冲。
    if (TacticalClass::Instance && DSurface::ViewBounds.Width > 0 && DSurface::ViewBounds.Height > 0)
        TacticalClass::Instance->RegisterDirtyArea(DSurface::ViewBounds, true);
}

void ClearViewerShroud() noexcept
{
    g_ShroudCache.Clear();
}
}
