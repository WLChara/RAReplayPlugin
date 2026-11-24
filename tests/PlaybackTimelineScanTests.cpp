#include "../src/plugin/PlaybackTimelineScan.h"
#include "../src/replay/ReplayTypes.h"
#include "../src/stats/GameClock.h"
#include "../src/storage/FrameWriter.h"
#include "OperationEventBuilder.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
using rareplay::plugin::FormatTimelineReadyLog;
using rareplay::plugin::PlaybackTimelineScan;
using rareplay::plugin::PlaybackTimelineScanResult;
using rareplay::plugin::SelectTimelineTiming;
using rareplay::stats::TimelineTiming;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            mPath = std::filesystem::temp_directory_path() /
                ("RAReplay-timeline-scan-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(mPath, error))
                return;
        }
        throw std::runtime_error("unable to create a temporary directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(mPath, ignored);
    }
    std::filesystem::path File(const char* name) const { return mPath / name; }

private:
    std::filesystem::path mPath;
};

std::array<std::uint8_t, 32> TargetHash()
{
    std::array<std::uint8_t, 32> hash{};
    for (std::size_t index = 0; index < hash.size(); ++index)
        hash[index] = static_cast<std::uint8_t>(index + 7);
    return hash;
}

// 写一份完成的回放：frames 依次为各帧（帧号按下标）；v3 起第 0 帧带初始编队快照。
void WriteReplay(const std::filesystem::path& path, const rareplay::replay::ReplaySessionInfo& info,
                 std::vector<rareplay::replay::ReplayFrame> frames)
{
    rareplay::storage::FrameWriter writer;
    std::string error;
    Require(writer.Open(path.wstring(), TargetHash(), {1, 2, 3}, info, error), "writer opens: " + error);
    for (std::uint32_t index = 0; index < frames.size(); ++index)
    {
        frames[index].mFrameNumber = index;
        if (index == 0 && info.mStreamVersion >= 3)
            frames[index].mRecordedGroups.emplace();
        Require(writer.AppendFrame(frames[index], error), "frame is written: " + error);
    }
    Require(writer.Finish(error), "replay is finalized: " + error);
}

// 轮询直到扫描给出结果；超过 10 秒视为失败。
PlaybackTimelineScanResult WaitForResult(PlaybackTimelineScan& scan)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto result = scan.TakeResult())
            return std::move(*result);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("the scan did not finish in time");
}

void CheckTimingSelection()
{
    struct Case
    {
        std::uint32_t mVersion;
        std::uint32_t mGameMode;
        TimelineTiming mExpected;
    };
    const Case cases[] = {
        {4, 5, TimelineTiming::RecordedGameSpeed}, {5, 5, TimelineTiming::RecordedGameSpeed},
        {4, 0, TimelineTiming::RecordedGameSpeed}, {1, 5, TimelineTiming::RuntimeGameSpeed},
        {2, 5, TimelineTiming::RuntimeGameSpeed}, {3, 5, TimelineTiming::RuntimeGameSpeed},
        {3, 0, TimelineTiming::RuntimeGameSpeed}, {2, 3, TimelineTiming::NetworkFrameRate},
        {5, 3, TimelineTiming::NetworkFrameRate}, {5, 4, TimelineTiming::NetworkFrameRate},
        // GameClock 只把 LAN（3）与 Internet（4）当作联机；其余模式按单机速度计时，而联机帧流不带速度。
        {5, 1, TimelineTiming::RuntimeGameSpeed}, {5, 2, TimelineTiming::RuntimeGameSpeed},
        {5, 6, TimelineTiming::RuntimeGameSpeed}};
    for (const Case& item : cases)
        Require(SelectTimelineTiming(item.mVersion, item.mGameMode) == item.mExpected,
                "timing for v" + std::to_string(item.mVersion) + " mode " + std::to_string(item.mGameMode));

    rareplay::viewer::GameTimeline timeline;
    timeline.mCumulativeMs = {16, 32, 48};
    Require(FormatTimelineReadyLog(timeline, TimelineTiming::NetworkFrameRate) ==
                "Replay timeline ready: frames=3 timing=network nominalFrom=none",
            "ready log without nominal frames");
    timeline.mFirstNominalFrame = 2;
    Require(FormatTimelineReadyLog(timeline, TimelineTiming::RecordedGameSpeed) ==
                "Replay timeline ready: frames=3 timing=recorded nominalFrom=2",
            "ready log with a nominal frame");
    timeline.mFirstNominalFrame = 0;
    Require(FormatTimelineReadyLog(timeline, TimelineTiming::RuntimeGameSpeed) ==
                "Replay timeline ready: frames=3 timing=runtime nominalFrom=0",
            "ready log for runtime speed");
}

// 单机 v5：录像速度逐帧沿用；扫描结果与统计时钟逐帧一致。
void CheckRecordedSpeedScan(const TemporaryDirectory& directory)
{
    const auto path = directory.File("scan-v5.rep");
    std::vector<rareplay::replay::ReplayFrame> frames(300);
    frames[0].mGameSpeed = 4;
    frames[120].mGameSpeed = 2;
    frames[200].mGameSpeed = 0;
    WriteReplay(path, {5, 1, 0, 1, 42, 5}, frames);

    PlaybackTimelineScan scan;
    Require(scan.Start(path.wstring(), TargetHash()), "the scan starts");
    Require(!scan.Start(path.wstring(), TargetHash()), "a scan starts only once");
    auto result = WaitForResult(scan);
    Require(result.mBuilder.has_value() && result.mError.empty(), "a valid replay scans: " + result.mError);
    Require(!scan.TakeResult(), "the result is taken only once");
    Require(result.mBuilder->GetTiming() == TimelineTiming::RecordedGameSpeed && result.mBuilder->GetFrameCount() == 300,
            "every frame is scanned with recorded speeds");
    const auto timeline = result.mBuilder->TakeTimeline();
    Require(timeline && timeline->GetFrameCount() == 300 && timeline->mFirstNominalFrame == 200u, "timeline of 300 frames");

    rareplay::stats::GameClock clock;
    int speed = 0;
    for (std::uint32_t frame = 0; frame < frames.size(); ++frame)
    {
        if (frames[frame].mGameSpeed)
            speed = static_cast<int>(*frames[frame].mGameSpeed);
        rareplay::stats::FrameTimingInput input;
        input.mGameSpeed = speed;
        clock.OnFrameCompleted(frame, input);
        Require(timeline->mCumulativeMs[frame] == clock.GetMilliseconds(),
                "scanned time equals the game clock at frame " + std::to_string(frame));
    }
    scan.Stop();
    scan.Stop();
}

// 联机：Timing 事件当帧生效。
void CheckNetworkScan(const TemporaryDirectory& directory)
{
    using rareplay::tests::EventBuilder;
    const auto path = directory.File("scan-network.rep");
    std::vector<rareplay::replay::ReplayFrame> frames(50);
    frames[10].mEvents.push_back(EventBuilder(rareplay::stats::NativeEventType::Timing, 0)
                                     .U16(rareplay::stats::event_layout::TIMING_REQUESTED_FPS, 20)
                                     .Build());
    WriteReplay(path, {3, 0, 0, 4, 42, 5}, frames);

    PlaybackTimelineScan scan;
    Require(scan.Start(path.wstring(), TargetHash()), "the network scan starts");
    auto result = WaitForResult(scan);
    Require(result.mBuilder && result.mBuilder->GetTiming() == TimelineTiming::NetworkFrameRate,
            "a network replay uses the frame rate: " + result.mError);
    const auto timeline = result.mBuilder->TakeTimeline();
    Require(timeline && timeline->mCumulativeMs[9] == 10 * 16 && timeline->mCumulativeMs[10] == 10 * 16 + 50 &&
                timeline->mCumulativeMs[49] == 10 * 16 + 40 * 50,
            "the Timing event counts from its own frame");
}

// 单机 v2：没有速度，扫描只记帧数，要运行时速度才能取出时间轴。
void CheckRuntimeSpeedScan(const TemporaryDirectory& directory)
{
    const auto path = directory.File("scan-v2.rep");
    WriteReplay(path, {5, 0, 0, 1, 42, 2}, std::vector<rareplay::replay::ReplayFrame>(25));
    PlaybackTimelineScan scan;
    Require(scan.Start(path.wstring(), TargetHash()), "the v2 scan starts");
    auto result = WaitForResult(scan);
    Require(result.mBuilder && result.mBuilder->NeedsRuntimeGameSpeed() && result.mBuilder->GetFrameCount() == 25,
            "a v2 replay needs the runtime speed: " + result.mError);
    Require(!result.mBuilder->TakeTimeline(), "no timeline without the runtime speed");
    const auto timeline = result.mBuilder->TakeTimeline(3);
    Require(timeline && timeline->mCumulativeMs[24] == 25 * 48 && timeline->mFirstNominalFrame == 0u,
            "the runtime speed converts the whole replay");
}

void CheckFailures(const TemporaryDirectory& directory)
{
    PlaybackTimelineScan missing;
    Require(missing.Start(directory.File("missing.rep").wstring(), TargetHash()), "a scan of a missing file starts");
    auto result = WaitForResult(missing);
    Require(!result.mBuilder && !result.mError.empty(), "a missing file fails with a reason");

    const auto path = directory.File("scan-hash.rep");
    std::vector<rareplay::replay::ReplayFrame> frames(5);
    frames[0].mGameSpeed = 3;
    WriteReplay(path, {5, 0, 0, 1, 42, 5}, frames);
    auto wrongHash = TargetHash();
    wrongHash[0] ^= 0xFF;
    PlaybackTimelineScan wrong;
    Require(wrong.Start(path.wstring(), wrongHash), "a scan with a wrong hash starts");
    result = WaitForResult(wrong);
    Require(!result.mBuilder && result.mError.find("hash") != std::string::npos,
            "a target hash mismatch fails: " + result.mError);

    PlaybackTimelineScan idle;
    Require(!idle.TakeResult(), "an idle scan has no result");
    idle.Stop();
}

// 停止与析构：Stop 等线程结束；未 Stop 就析构时 detach，线程只持有共享状态，不会访问已释放的对象。
void CheckStopAndDestroy(const TemporaryDirectory& directory)
{
    const auto path = directory.File("scan-long.rep");
    std::vector<rareplay::replay::ReplayFrame> frames(20'000);
    frames[0].mGameSpeed = 1;
    WriteReplay(path, {5, 0, 0, 1, 42, 5}, frames);

    PlaybackTimelineScan stopped;
    Require(stopped.Start(path.wstring(), TargetHash()), "the long scan starts");
    stopped.Stop();
    const auto afterStop = stopped.TakeResult();
    Require(afterStop.has_value(), "a stopped scan has delivered its result before Stop returns");
    Require(afterStop->mBuilder.has_value() || afterStop->mError == "scan stopped",
            "a stopped scan either finished or reports the stop: " + afterStop->mError);

    {
        PlaybackTimelineScan abandoned;
        Require(abandoned.Start(path.wstring(), TargetHash()), "the abandoned scan starts");
    }
    // 被 detach 的线程看到停止标志后很快结束；给它一点时间，避免进程退出时仍在读临时文件。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}
}

int main()
{
    try
    {
        CheckTimingSelection();
        TemporaryDirectory directory;
        CheckRecordedSpeedScan(directory);
        CheckNetworkScan(directory);
        CheckRuntimeSpeedScan(directory);
        CheckFailures(directory);
        CheckStopAndDestroy(directory);
        std::cout << "Playback timeline scan tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
