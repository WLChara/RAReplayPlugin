#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rareplay::assets
{
/** 只读帧描述。身份是不透明资源键，像素由调用者持有并保证为未压缩的连续数据。 */
struct ShroudFramePixels
{
    std::uintptr_t mResourceIdentity = 0;
    std::uintptr_t mDataIdentity = 0;
    const std::uint8_t* mPixels = nullptr;
    int mFrame = -1;
    int mWidth = 0;
    int mHeight = 0;
    bool mIsPlain = false;
};

/** ABuffer 中 127 为中性强度；向中性插值一半，保留 254/255 特殊像素。 */
std::uint8_t AdjustHalfShroudPixel(std::uint8_t value) noexcept;

/** 游戏线程独占的有界像素副本；返回指针只供本次同步绘制，不得跨帧持有。 */
class ShroudFrameCache final
{
public:
    static constexpr std::size_t MAX_FRAME_PIXELS = 1024 * 1024;
    static constexpr std::size_t MAX_CACHE_BYTES = 4 * 1024 * 1024;
    static constexpr std::size_t MAX_CACHE_FRAMES = 128;

    /** 关闭、压缩或尺寸异常时返回原像素；预算耗尽时同样回退，不修改源资源。 */
    const std::uint8_t* GetPixels(const ShroudFramePixels& frame, bool enabled);
    void Clear() noexcept;
    std::size_t GetCachedBytes() const noexcept { return mCachedBytes; }
    std::size_t GetCachedFrames() const noexcept { return mFrames.size(); }

private:
    struct CachedFrame
    {
        const std::uint8_t* mOriginalPixels = nullptr;
        int mFrame = -1;
        int mWidth = 0;
        int mHeight = 0;
        std::vector<std::uint8_t> mPixels;
    };

    std::uintptr_t mResourceIdentity = 0;
    std::uintptr_t mDataIdentity = 0;
    std::size_t mCachedBytes = 0;
    std::vector<CachedFrame> mFrames;
};
}
