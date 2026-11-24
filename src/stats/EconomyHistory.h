#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rareplay::stats
{
/** 每 1 秒游戏时间采样一次资金。 */
constexpr std::int64_t ECONOMY_SAMPLE_INTERVAL_MS = 1000;
/** 环形缓冲容量：每秒一个样本，12000 个样本覆盖 3 小时 20 分钟游戏时间（要求至少 3 小时）。 */
constexpr std::size_t ECONOMY_SAMPLE_CAPACITY = 12000;
/**
 * 发布给界面的单条曲线点数上限。经济大窗口需要比侧栏小图更细的曲线，小图在绘制时再抽稀。
 * 1 秒一个样本时，约 25 分钟内的对局不降采样，3 小时以上约每 9 秒取一点。
 */
constexpr std::size_t ECONOMY_PUBLISH_MAX_POINTS = 1500;

/**
 * 从 count 个按时间升序排列的样本中挑出不超过 maxPoints 个下标：不超过上限时全部保留；否则按固定步长取点，
 * 步长随样本数增长，且总是包含第一个与最后一个样本。结果升序。maxPoints 小于 2 时按 2 处理。
 */
std::vector<std::size_t> SelectEconomyIndices(std::size_t count, std::size_t maxPoints);

/**
 * 单个玩家的资金样本环形缓冲。满后覆盖最旧样本，所以曲线只保留最近 capacity 个样本。
 * 样本以 32 位保存（时间毫秒与资金；资金超出 32 位时饱和），发布时转为 64 位的 viewer 点。只在游戏线程使用。
 */
class EconomyHistory final
{
public:
    explicit EconomyHistory(std::size_t capacity = ECONOMY_SAMPLE_CAPACITY);

    /** 追加一个样本；时间必须不小于上一个样本，否则忽略（返回 false）。 */
    bool Add(std::int64_t timeMs, std::int64_t credits);
    std::size_t GetSize() const noexcept { return mSize; }
    std::size_t GetCapacity() const noexcept { return mSamples.size(); }

    /**
     * 降采样后的曲线。样本没有变化时重复调用返回同一个不可变对象，快照之间因此共享而不复制；
     * 样本数为零时返回点数为零的曲线（不是空指针）。
     */
    std::shared_ptr<const viewer::EconomyCurve> BuildCurve(std::size_t maxPoints = ECONOMY_PUBLISH_MAX_POINTS);

private:
    struct Sample
    {
        std::int32_t mTimeMs = 0;
        std::int32_t mCredits = 0;
    };

    const Sample& At(std::size_t index) const noexcept;

    std::vector<Sample> mSamples;
    std::size_t mHead = 0;
    std::size_t mSize = 0;
    std::uint64_t mVersion = 0;
    std::uint64_t mCachedVersion = 0;
    std::size_t mCachedMaxPoints = 0;
    std::shared_ptr<const viewer::EconomyCurve> mCache;
};
}
