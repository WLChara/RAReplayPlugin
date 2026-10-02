#include "../src/assets/ShroudPixels.h"

#include <array>
#include <climits>
#include <stdexcept>

namespace
{
using namespace rareplay::assets;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void TestPixels()
{
    Require(AdjustHalfShroudPixel(0) == 63 && AdjustHalfShroudPixel(1) == 64 &&
        AdjustHalfShroudPixel(126) == 126 && AdjustHalfShroudPixel(127) == 127 &&
        AdjustHalfShroudPixel(128) == 128 && AdjustHalfShroudPixel(253) == 190 &&
        AdjustHalfShroudPixel(254) == 254 && AdjustHalfShroudPixel(255) == 255,
        "half-opacity interpolation and special pixels must match native intensity convention");
    for (int value = 0; value < 254; ++value)
    {
        const int delta = value - 127;
        const int reference = 127 + (delta * 50 + (delta >= 0 ? 50 : -50)) / 100;
        Require(AdjustHalfShroudPixel(static_cast<std::uint8_t>(value)) == reference,
            "all regular values must match the reference 50% interpolation");
    }
}

void TestCacheAndRestore()
{
    std::array<std::uint8_t, 6> pixels{0, 1, 127, 253, 254, 255};
    const auto original = pixels;
    ShroudFramePixels frame{1, 2, pixels.data(), 0, 3, 2, true};
    ShroudFrameCache cache;
    const auto* adjusted = cache.GetPixels(frame, true);
    Require(adjusted != pixels.data() && adjusted[0] == 63 && adjusted[4] == 254 &&
        cache.GetPixels(frame, true) == adjusted && pixels == original &&
        cache.GetCachedBytes() == 6 && cache.GetCachedFrames() == 1,
        "cache owns a stable copy and never modifies source pixels");
    Require(cache.GetPixels(frame, false) == pixels.data() && cache.GetCachedBytes() == 0 &&
        cache.GetCachedFrames() == 0, "disabling restores original pixels and releases copies");
    Require(cache.GetPixels(frame, true) != pixels.data(), "reenabling creates a fresh adjusted copy");
    frame.mDataIdentity = 3;
    frame.mFrame = 1;
    cache.GetPixels(frame, true);
    Require(cache.GetCachedFrames() == 1 && cache.GetCachedBytes() == 6,
        "resolved resource identity changes release previous data");
    std::array<std::uint8_t, 6> reloaded{127, 127, 127, 127, 127, 127};
    frame.mPixels = reloaded.data();
    Require(cache.GetPixels(frame, true)[0] == 127 && cache.GetCachedFrames() == 1,
        "same frame with a new source pointer replaces stale pixels");
    frame.mIsPlain = false;
    Require(cache.GetPixels(frame, true) == frame.mPixels, "compressed data must stay untouched");
    frame.mIsPlain = true;
    frame.mWidth = 0;
    Require(cache.GetPixels(frame, true) == frame.mPixels, "empty resource frame falls back");
    frame.mWidth = INT_MAX;
    frame.mHeight = INT_MAX;
    Require(cache.GetPixels(frame, true) == frame.mPixels, "overflowing dimensions must never read data");
    cache.Clear();
    Require(cache.GetCachedFrames() == 0 && pixels == original, "session end releases cache only");
}

void TestBudgets()
{
    std::uint8_t pixel = 0;
    ShroudFramePixels frame{1, 2, &pixel, 0, 1, 1, true};
    ShroudFrameCache cache;
    for (std::size_t index = 0; index < ShroudFrameCache::MAX_CACHE_FRAMES; ++index)
    {
        frame.mFrame = static_cast<int>(index);
        Require(cache.GetPixels(frame, true) != &pixel, "frame budget fills with owned pixels");
    }
    ++frame.mFrame;
    Require(cache.GetPixels(frame, true) == &pixel && cache.GetCachedFrames() == ShroudFrameCache::MAX_CACHE_FRAMES,
        "frame budget exhaustion falls back without growing");
    std::vector<std::uint8_t> large(ShroudFrameCache::MAX_FRAME_PIXELS, 0);
    frame = {4, 5, large.data(), 0, 1024, 1024, true};
    for (int index = 0; index < 4; ++index)
    {
        frame.mFrame = index;
        Require(cache.GetPixels(frame, true) != large.data(), "four one-megabyte frames fit byte budget");
    }
    frame.mFrame = 4;
    Require(cache.GetPixels(frame, true) == large.data() &&
        cache.GetCachedBytes() == ShroudFrameCache::MAX_CACHE_BYTES,
        "byte budget exhaustion preserves source pixels");
}
}

int main()
{
    TestPixels();
    TestCacheAndRestore();
    TestBudgets();
}
