#pragma once

#include "OperationEvents.h"
#include "../viewer/ReplayViewModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rareplay::stats
{
/** 联机开局时 spawner 写入的 DesiredFrameRate（常量 60）。 */
constexpr int NETWORK_INITIAL_DESIRED_FRAME_RATE = 60;

/** 一条原生事件的完整字节（布局见 OperationEvents.h）；与录像帧记录里的事件数组同一类型。 */
using NativeEventBytes = std::array<std::uint8_t, NATIVE_EVENT_BYTES>;

/**
 * Timing（类型 32）事件的 RequestedFPS：事件字节 +7 的小端 u16，原生把它原样写入 DesiredFrameRate（不夹取，0 也照写）。
 * 不是 Timing 事件或长度不足 NATIVE_EVENT_BYTES 时为空。
 */
std::optional<int> DecodeTimingDesiredFrameRate(std::span<const std::uint8_t> event) noexcept;

/** 时间表的计时口径，由帧流头决定（帧流版本与 GameMode 的换算由调用方完成）。 */
enum class TimelineTiming
{
    /** 单机 v4 起：帧记录可带“本帧起的游戏速度”，空表示沿用前值，第 0 帧必有。 */
    RecordedGameSpeed,
    /** 单机 v1–v3：录像没有速度。构建时只记帧数，拿到运行时速度后整段按同一速度换算，并标名义。 */
    RuntimeGameSpeed,
    /** 联机：DesiredFrameRate 初值 NETWORK_INITIAL_DESIRED_FRAME_RATE，由各帧执行的 Timing 事件改变。 */
    NetworkFrameRate
};

/**
 * 按帧增量构建“帧号 → 累计游戏时间”表（viewer::GameTimeline），口径与统计时钟 GameClock 相同：
 * 第 L 项 = 第 0 帧到第 L 帧时长之和，每帧时长由 GetFrameDuration 给出，名义标记一旦出现就延续到表尾。
 * - 单机：第 k 帧的速度是第 k 帧记录的速度，没有时沿用前一帧的值（录制在模拟前采样，回放在模拟前写回）。
 * - 联机：第 k 帧执行的 Timing 事件从第 k 帧本身起计入时长。统计时钟在帧完成点读取 DesiredFrameRate，
 *   那时本帧事件已经执行；帧预算要到下一帧才生效，但时间表按统计时钟的口径，HUD 显示的时间与进度条一致。
 * 结果是名义上的游戏时间，原局的网络停顿与机器跑不满预算的超时都无法重建。
 * 不含线程同步；同一实例只能在一个线程上使用。
 */
class GameTimelineBuilder final
{
public:
    explicit GameTimelineBuilder(TimelineTiming timing) noexcept : mTiming(timing) {}

    /**
     * 登记下一帧。frameNumber 必须等于已登记的帧数（帧流从第 0 帧起连续）。gameSpeed 是帧记录携带的速度原值，
     * 只有 RecordedGameSpeed 口径可以带，且第 0 帧必须带；速度 0 或超出 0..6 的值按 GetFrameDuration 计 16 ms 并标名义。
     * events 为本帧按执行顺序排列的事件，只有联机口径读取其中的 Timing 事件。
     * 输入违反上述规则时返回 false，构建器进入失败状态，之后的登记一律返回 false。
     */
    bool AddFrame(std::uint32_t frameNumber, std::optional<std::uint32_t> gameSpeed,
        std::span<const NativeEventBytes> events);

    TimelineTiming GetTiming() const noexcept { return mTiming; }
    std::uint32_t GetFrameCount() const noexcept { return mFrameCount; }
    bool HasFailed() const noexcept { return !mError.empty(); }
    /** 失败原因（技术日志用英文）；没有失败时为空。 */
    const std::string& GetError() const noexcept { return mError; }
    /** 为真表示录像没有速度，生成时间轴还需要运行时速度（只在 RuntimeGameSpeed 口径下为真）。 */
    bool NeedsRuntimeGameSpeed() const noexcept { return mTiming == TimelineTiming::RuntimeGameSpeed; }

    /**
     * 取出时间轴，成功后构建器回到初始状态（同一口径、零帧）。
     * RuntimeGameSpeed 口径：每帧按 runtimeGameSpeed（第 0 帧读到的 GameOptions.GameSpeed 原值）换算，
     * 规则同 GetFrameDuration，整表从第 0 帧起标名义；其他口径忽略该参数。
     * 已失败、没有任何帧，或 RuntimeGameSpeed 口径而 runtimeGameSpeed 为空时返回空，构建器不变。
     */
    std::optional<viewer::GameTimeline> TakeTimeline(std::optional<int> runtimeGameSpeed = std::nullopt);

private:
    bool Fail(std::string error);

    TimelineTiming mTiming;
    std::uint32_t mFrameCount = 0;
    /** RecordedGameSpeed：当前沿用的速度原值。 */
    std::optional<std::uint32_t> mGameSpeed;
    /** NetworkFrameRate：当前的 DesiredFrameRate。 */
    int mDesiredFrameRate = NETWORK_INITIAL_DESIRED_FRAME_RATE;
    /** RuntimeGameSpeed 口径不累加，只计帧数。 */
    std::vector<std::int64_t> mCumulativeMs;
    std::optional<std::uint32_t> mFirstNominalFrame;
    std::string mError;
};
}
