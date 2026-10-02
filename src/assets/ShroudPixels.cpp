#include "ShroudPixels.h"

#include <algorithm>
#include <utility>

namespace rareplay::assets
{
std::uint8_t AdjustHalfShroudPixel(std::uint8_t value) noexcept
{
    if (value >= 0xFE)
        return value;
    constexpr int NEUTRAL_INTENSITY = 127;
    const int delta = static_cast<int>(value) - NEUTRAL_INTENSITY;
    // 与原显示强度插值一致：半步在相对中性方向上取远离零的整数。
    return static_cast<std::uint8_t>(NEUTRAL_INTENSITY + (delta + (delta >= 0 ? 1 : -1)) / 2);
}

const std::uint8_t* ShroudFrameCache::GetPixels(const ShroudFramePixels& frame, bool enabled)
{
    if (!enabled)
    {
        Clear();
        return frame.mPixels;
    }
    if (mResourceIdentity != frame.mResourceIdentity || mDataIdentity != frame.mDataIdentity)
    {
        Clear();
        mResourceIdentity = frame.mResourceIdentity;
        mDataIdentity = frame.mDataIdentity;
    }
    if (!frame.mPixels || !frame.mIsPlain || !frame.mResourceIdentity || !frame.mDataIdentity ||
        frame.mFrame < 0 || frame.mWidth <= 0 || frame.mHeight <= 0 ||
        static_cast<std::size_t>(frame.mWidth) > MAX_FRAME_PIXELS / static_cast<std::size_t>(frame.mHeight))
        return frame.mPixels;
    const auto pixelCount = static_cast<std::size_t>(frame.mWidth) * frame.mHeight;
    for (auto iterator = mFrames.begin(); iterator != mFrames.end(); ++iterator)
    {
        if (iterator->mFrame != frame.mFrame)
            continue;
        if (iterator->mOriginalPixels == frame.mPixels && iterator->mWidth == frame.mWidth &&
            iterator->mHeight == frame.mHeight)
            return iterator->mPixels.data();
        // 同一帧重新加载或尺寸变化不能留下旧副本，也不能无限追加身份变体。
        mCachedBytes -= iterator->mPixels.size();
        mFrames.erase(iterator);
        break;
    }
    if (mFrames.size() >= MAX_CACHE_FRAMES || pixelCount > MAX_CACHE_BYTES - mCachedBytes)
        return frame.mPixels;
    CachedFrame cached;
    cached.mOriginalPixels = frame.mPixels;
    cached.mFrame = frame.mFrame;
    cached.mWidth = frame.mWidth;
    cached.mHeight = frame.mHeight;
    cached.mPixels.resize(pixelCount);
    std::transform(frame.mPixels, frame.mPixels + pixelCount, cached.mPixels.begin(), AdjustHalfShroudPixel);
    mFrames.push_back(std::move(cached));
    mCachedBytes += pixelCount;
    return mFrames.back().mPixels.data();
}

void ShroudFrameCache::Clear() noexcept
{
    std::vector<CachedFrame>().swap(mFrames);
    mCachedBytes = 0;
    mResourceIdentity = 0;
    mDataIdentity = 0;
}
}
