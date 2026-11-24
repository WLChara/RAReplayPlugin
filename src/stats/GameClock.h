#pragma once

#include "StatsObservation.h"

#include <cstdint>
#include <optional>

namespace rareplay::stats
{
/** SystemTimer 一个 tick 为 timeGetTime()>>4，即 16 ms。 */
constexpr int TIMER_TICK_MS = 16;
/** 单机速度 0 原生不限速，没有真实的每帧时长，约定按一个 tick 计，并标记为名义时间。 */
constexpr int NOMINAL_FRAME_MS = TIMER_TICK_MS;
/** 联机 DesiredFrameRate 为 0 时原生按 33 ms 一帧。 */
constexpr int NETWORK_FALLBACK_FRAME_MS = 33;
/** 单机速度档的有效范围。 */
constexpr int MAX_GAME_SPEED = 6;

struct FrameDuration
{
    int mMilliseconds = NOMINAL_FRAME_MS;
    /** 本帧时长是约定的名义值而不是原生真实节奏。 */
    bool mIsNominal = false;
};

/**
 * 一帧的游戏时长：
 * - 联机：1000 / DesiredFrameRate 取整（与 0x55D532 一致），DesiredFrameRate 不是正数时为 33 ms。
 * - 单机与战役：GameSpeed × 16 ms；速度 0 或超出 0..6 的非法值按名义 16 ms 计并标记名义。
 * 观看者调速不改变 GameOptions.GameSpeed 与 DesiredFrameRate，所以不影响这里的结果。
 */
FrameDuration GetFrameDuration(const FrameTimingInput& input) noexcept;

/**
 * 游戏时间累加器：每完成一帧累加该帧时长。暂停时帧号不推进，所以不累加；
 * 同一帧号重复登记（例如暂停时反复进入帧完成点）不重复累加。
 * 只在游戏线程使用。
 */
class GameClock final
{
public:
    /**
     * 登记已完成的帧。帧号相对上一次登记前进 n 时累加 n 个帧时长（正常为 1；首次登记按 1 帧计）；
     * 帧号相同或回退时忽略。返回是否累加了时间。
     */
    bool OnFrameCompleted(std::uint32_t frameNumber, const FrameTimingInput& input) noexcept;

    std::int64_t GetMilliseconds() const noexcept { return mMilliseconds; }
    /** 累加过名义帧之后为真，且不会回到假。 */
    bool IsNominal() const noexcept { return mIsNominal; }
    bool HasStarted() const noexcept { return mLastFrame.has_value(); }
    void Reset() noexcept;

private:
    std::int64_t mMilliseconds = 0;
    bool mIsNominal = false;
    std::optional<std::uint32_t> mLastFrame;
};
}
