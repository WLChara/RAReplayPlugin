#pragma once

#include "../stats/GameTimelineBuilder.h"
#include "../viewer/ReplayViewModel.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace rareplay::plugin
{
/**
 * 按帧流头选择时间表的计时口径，与统计时钟 GameClock 的判定一致：
 * - v4 起的单机帧流（FrameCodec::CarriesGameSpeed）用帧记录里的速度；
 * - GameMode 为 LAN（3）或 Internet（4）用 DesiredFrameRate，与 StatsReader 判定联机的口径相同；
 * - 其余（v1–v3 单机）录像没有速度，需要运行时速度。
 */
stats::TimelineTiming SelectTimelineTiming(std::uint32_t streamVersion, std::uint32_t gameMode) noexcept;

/** 计时口径在日志中的名称：recorded、runtime、network。 */
const char* GetTimelineTimingName(stats::TimelineTiming timing) noexcept;

/** 时间表就绪的技术日志：“Replay timeline ready: frames=N timing=recorded|runtime|network nominalFrom=K|none”。 */
std::string FormatTimelineReadyLog(const viewer::GameTimeline& timeline, stats::TimelineTiming timing);

/** 一次扫描的结果，只含值。 */
struct PlaybackTimelineScanResult
{
    /**
     * 成功时为已登记全部帧的构建器，由游戏线程取出时间轴（RuntimeGameSpeed 口径只记了帧数，要等运行时速度）；
     * 失败时为空。
     */
    std::optional<stats::GameTimelineBuilder> mBuilder;
    /** 失败原因（技术日志用英文）；成功时为空。 */
    std::string mError;
};

/**
 * 回放时间表的扫描，插件唯一的工作线程。线程用独立的 storage::FrameReader 从头读一遍
 * 逻辑帧流，逐帧把帧号、速度与事件交给 stats::GameTimelineBuilder；不与播放共用读取位置，不访问游戏，也不写日志。
 * 结果经互斥量交给游戏线程轮询。停止标志逐帧检查；打开文件时的完整校验无法中途打断，Stop 会等它结束。
 * Start、TakeResult、Stop 与析构只在游戏线程调用。
 */
class PlaybackTimelineScan final
{
public:
    PlaybackTimelineScan() = default;
    /** 正常路径已经 Stop；仍可 join 时只请求停止并 detach，理由见实现。 */
    ~PlaybackTimelineScan();
    PlaybackTimelineScan(const PlaybackTimelineScan&) = delete;
    PlaybackTimelineScan& operator=(const PlaybackTimelineScan&) = delete;

    /**
     * 启动扫描线程。path 与 targetHash 与播放的 FrameReader::Open 相同。每个对象只启动一次，再次调用返回 false。
     * 创建线程失败时返回 false，失败原因作为结果交给 TakeResult。
     */
    bool Start(const std::wstring& path, const std::array<std::uint8_t, 32>& targetHash);

    /** 结果就绪时取出一次，之后返回空；尚未启动、尚未就绪或已取出时为空。不阻塞。 */
    std::optional<PlaybackTimelineScanResult> TakeResult();

    /** 请求停止并等待线程结束；没有线程或已停止时立即返回，可重复调用。 */
    void Stop() noexcept;

private:
    struct SharedState;

    /** 线程函数：扫描并把结果写入共享状态；任何异常都转为失败结果。 */
    static void Run(std::shared_ptr<SharedState> state, std::wstring path,
        std::array<std::uint8_t, 32> targetHash) noexcept;

    // 线程只持有共享状态，不引用本对象：析构时 detach 的线程也不会访问已释放的内存。
    std::shared_ptr<SharedState> mState;
    std::thread mThread;
};
}
