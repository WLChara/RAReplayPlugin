#include "PlaybackTimelineScan.h"

#include "../replay/FrameCodec.h"
#include "../storage/FrameReader.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

namespace rareplay::plugin
{
struct PlaybackTimelineScan::SharedState
{
    std::atomic<bool> mStopRequested = false;
    std::mutex mMutex;
    /** 线程结束前写入一次；游戏线程取出后清空。 */
    std::optional<PlaybackTimelineScanResult> mResult;
};

namespace
{
// 与 StatsReader 判定联机的 GameMode 相同（YRpp GameMode::LAN、Internet）。
constexpr std::uint32_t GAME_MODE_LAN = 3;
constexpr std::uint32_t GAME_MODE_INTERNET = 4;

PlaybackTimelineScanResult MakeFailure(std::string error)
{
    PlaybackTimelineScanResult result;
    result.mError = std::move(error);
    return result;
}

PlaybackTimelineScanResult ScanTimeline(const std::atomic<bool>& stopRequested, const std::wstring& path,
    const std::array<std::uint8_t, 32>& targetHash)
{
    storage::FrameReader reader;
    std::vector<std::uint8_t> metadata;
    std::string error;
    if (!reader.Open(path, targetHash, metadata, error))
        return MakeFailure("cannot open the replay: " + error);
    replay::ReplaySessionInfo info;
    if (!reader.GetSessionInfo(info, error))
        return MakeFailure("cannot read the stream header: " + error);
    stats::GameTimelineBuilder builder(SelectTimelineTiming(info.mStreamVersion, info.mGameMode));
    while (true)
    {
        if (stopRequested.load(std::memory_order_relaxed))
            return MakeFailure("scan stopped");
        replay::ReplayFrame frame;
        const auto readResult = reader.ReadFrame(frame, error);
        if (readResult == replay::ReplayReadResult::End)
            break;
        if (readResult == replay::ReplayReadResult::Error)
            return MakeFailure("frame " + std::to_string(builder.GetFrameCount()) + " cannot be read: " + error);
        if (!builder.AddFrame(frame.mFrameNumber, frame.mGameSpeed, frame.mEvents))
            return MakeFailure(builder.GetError());
    }
    if (builder.GetFrameCount() == 0)
        return MakeFailure("the replay has no frames");
    PlaybackTimelineScanResult result;
    result.mBuilder = std::move(builder);
    return result;
}
}

stats::TimelineTiming SelectTimelineTiming(std::uint32_t streamVersion, std::uint32_t gameMode) noexcept
{
    if (replay::FrameCodec::CarriesGameSpeed(streamVersion, gameMode))
        return stats::TimelineTiming::RecordedGameSpeed;
    if (gameMode == GAME_MODE_LAN || gameMode == GAME_MODE_INTERNET)
        return stats::TimelineTiming::NetworkFrameRate;
    return stats::TimelineTiming::RuntimeGameSpeed;
}

const char* GetTimelineTimingName(stats::TimelineTiming timing) noexcept
{
    switch (timing)
    {
    case stats::TimelineTiming::RecordedGameSpeed: return "recorded";
    case stats::TimelineTiming::RuntimeGameSpeed: return "runtime";
    case stats::TimelineTiming::NetworkFrameRate: return "network";
    }
    return "unknown";
}

std::string FormatTimelineReadyLog(const viewer::GameTimeline& timeline, stats::TimelineTiming timing)
{
    return "Replay timeline ready: frames=" + std::to_string(timeline.GetFrameCount()) + " timing=" +
        GetTimelineTimingName(timing) + " nominalFrom=" +
        (timeline.mFirstNominalFrame ? std::to_string(*timeline.mFirstNominalFrame) : std::string("none"));
}

PlaybackTimelineScan::~PlaybackTimelineScan()
{
    // 会话结束、失败与退出的路径都已 Stop 并 join。这里仍可 join，只会是进程退出时的静态析构（g_Runtime 在 DllMain 的
    // 进程分离通知里析构）：那时操作系统已先结束其他线程，在 DllMain 或静态析构里 join 可能死锁；而析构一个可 join 的
    // std::thread 会调用 terminate。所以只请求停止并 detach。线程只持有共享状态的 shared_ptr，detach 后也不会访问本对象。
    if (mState)
        mState->mStopRequested.store(true, std::memory_order_relaxed);
    if (mThread.joinable())
        mThread.detach();
}

bool PlaybackTimelineScan::Start(const std::wstring& path, const std::array<std::uint8_t, 32>& targetHash)
{
    if (mState)
        return false;
    mState = std::make_shared<SharedState>();
    try
    {
        mThread = std::thread(&PlaybackTimelineScan::Run, mState, path, targetHash);
        return true;
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(mState->mMutex);
        mState->mResult = MakeFailure(std::string("cannot start the scan thread: ") + error.what());
        return false;
    }
}

std::optional<PlaybackTimelineScanResult> PlaybackTimelineScan::TakeResult()
{
    if (!mState)
        return std::nullopt;
    std::optional<PlaybackTimelineScanResult> result;
    {
        std::lock_guard lock(mState->mMutex);
        result = std::move(mState->mResult);
        mState->mResult.reset();
    }
    // 结果写入后线程立即结束，这里 join 只等它退出线程函数。
    if (result && mThread.joinable())
        mThread.join();
    return result;
}

void PlaybackTimelineScan::Stop() noexcept
{
    if (mState)
        mState->mStopRequested.store(true, std::memory_order_relaxed);
    if (!mThread.joinable())
        return;
    try
    {
        mThread.join();
    }
    catch (...)
    {
        // join 只在同一线程或线程已失效时抛出；此时无法等待，只能 detach，避免可 join 的线程对象在析构时 terminate。
        mThread.detach();
    }
}

void PlaybackTimelineScan::Run(std::shared_ptr<SharedState> state, std::wstring path,
    std::array<std::uint8_t, 32> targetHash) noexcept
{
    PlaybackTimelineScanResult result;
    try
    {
        result = ScanTimeline(state->mStopRequested, path, targetHash);
    }
    catch (const std::exception& error)
    {
        result = PlaybackTimelineScanResult();
        result.mError = std::string("scan exception: ") + error.what();
    }
    catch (...)
    {
        result = PlaybackTimelineScanResult();
        result.mError = "scan exception";
    }
    try
    {
        std::lock_guard lock(state->mMutex);
        state->mResult = std::move(result);
    }
    catch (...)
    {
        // 互斥量失效时没有可交付结果的途径；游戏线程会一直看不到结果，总帧数保持未知，不影响回放。
    }
}
}
