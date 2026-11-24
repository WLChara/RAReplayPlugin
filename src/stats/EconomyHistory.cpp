#include "EconomyHistory.h"

#include <algorithm>
#include <limits>

namespace rareplay::stats
{
std::vector<std::size_t> SelectEconomyIndices(std::size_t count, std::size_t maxPoints)
{
    maxPoints = std::max<std::size_t>(maxPoints, 2);
    std::vector<std::size_t> indices;
    if (count == 0)
        return indices;
    if (count <= maxPoints)
    {
        indices.resize(count);
        for (std::size_t index = 0; index < count; ++index)
            indices[index] = index;
        return indices;
    }
    // 取点 0、step、2·step…（均小于最后下标，至多 maxPoints-1 项），再补上最后一个，总数不超过 maxPoints。
    const std::size_t last = count - 1;
    const std::size_t divisions = maxPoints - 1;
    const std::size_t step = (last + divisions - 1) / divisions;
    for (std::size_t index = 0; index < last; index += step)
        indices.push_back(index);
    indices.push_back(last);
    return indices;
}

EconomyHistory::EconomyHistory(std::size_t capacity) : mSamples(std::max<std::size_t>(capacity, 1)) {}

const EconomyHistory::Sample& EconomyHistory::At(std::size_t index) const noexcept
{
    return mSamples[(mHead + index) % mSamples.size()];
}

bool EconomyHistory::Add(std::int64_t timeMs, std::int64_t credits)
{
    if (timeMs < 0 || timeMs > std::numeric_limits<std::int32_t>::max())
        return false;
    if (mSize > 0 && timeMs < At(mSize - 1).mTimeMs)
        return false;
    Sample sample;
    sample.mTimeMs = static_cast<std::int32_t>(timeMs);
    sample.mCredits = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        credits, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    if (mSize < mSamples.size())
    {
        mSamples[(mHead + mSize) % mSamples.size()] = sample;
        ++mSize;
    }
    else
    {
        mSamples[mHead] = sample;
        mHead = (mHead + 1) % mSamples.size();
    }
    ++mVersion;
    return true;
}

std::shared_ptr<const viewer::EconomyCurve> EconomyHistory::BuildCurve(std::size_t maxPoints)
{
    if (mCache && mCachedVersion == mVersion && mCachedMaxPoints == maxPoints)
        return mCache;
    auto curve = std::make_shared<viewer::EconomyCurve>();
    const std::vector<std::size_t> indices = SelectEconomyIndices(mSize, maxPoints);
    curve->mPoints.reserve(indices.size());
    for (const std::size_t index : indices)
    {
        const Sample& sample = At(index);
        curve->mPoints.push_back({sample.mTimeMs, sample.mCredits});
    }
    mCache = std::move(curve);
    mCachedVersion = mVersion;
    mCachedMaxPoints = maxPoints;
    return mCache;
}
}
