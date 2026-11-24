#pragma once

#include <cstdint>
#include <string>

namespace rareplay::game_api
{
struct IconStats
{
    std::uint32_t mPcx = 0;
    std::uint32_t mShp = 0;
    std::uint32_t mMissing = 0;
    std::uint32_t mInvalidSurface = 0;
    std::uint32_t mCacheLimit = 0;
    std::string mRecentDetail;
};
}
