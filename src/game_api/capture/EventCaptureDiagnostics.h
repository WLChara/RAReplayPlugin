#pragma once

#include "DispatchedEvents.h"

#include <cstdint>
#include <optional>

namespace rareplay::game_api
{
/** 录制开始时的会话条件；只用于诊断日志，不进入录像。 */
struct EventCaptureContext
{
    std::uint32_t mSessionGameMode = 0;
    std::uintptr_t mMpGameModeAddress = 0;
    // MPGameModeClass::MPModeIndex（this+0x28，按构造函数字节静态核对）；MPGameMode 为空时保持 -1。
    std::int32_t mMpModeIndex = -1;
    bool mUsesDispatchCapture = false;
};

/** 单机录制一帧内的采集观测值。 */
struct EventCaptureFrameStats
{
    std::uint32_t mFrameNumber = 0;
    bool mHasEntry = false;
    // 0x64C380 入口时 DoList 中未执行事件按 Frame 与 N 的关系计数。
    std::uint32_t mEntryStale = 0;
    std::uint32_t mEntryCurrent = 0;
    std::uint32_t mEntryFuture = 0;
    std::uint32_t mOutListCount = 0;
    // 0x64C63D 采入录像的分派事件；过期与上一帧（Frame == N-1）为其子集。
    std::uint32_t mDispatched = 0;
    std::uint32_t mStaleDispatched = 0;
    std::uint32_t mPreviousFrameDispatched = 0;
    std::uint32_t mFrameInfoSkipped = 0;
    // 录制中经过分派点但本帧入口标记未打开，这些事件没有写入录像。
    std::uint32_t mUnmarkedDispatches = 0;
    std::uint32_t mMoveFlashBeforeParser = 0;
    std::uint32_t mMoveFlashAfterParser = 0;

    /** 有过期分派、未标记分派，或有 MoveFlash 却没有任何分派时视为异常帧。 */
    bool IsAnomalous() const noexcept;
};

struct EventCaptureTotals
{
    std::uint64_t mFrames = 0;
    std::uint64_t mEntryFrames = 0;
    std::uint64_t mEntryStale = 0;
    std::uint64_t mEntryCurrent = 0;
    std::uint64_t mEntryFuture = 0;
    std::uint32_t mMaxOutList = 0;
    std::uint64_t mDispatched = 0;
    std::uint64_t mStaleDispatched = 0;
    std::uint64_t mPreviousFrameDispatched = 0;
    std::uint64_t mFrameInfoSkipped = 0;
    std::uint64_t mUnmarkedDispatches = 0;
    std::uint64_t mMoveFlashBeforeParser = 0;
    std::uint64_t mMoveFlashAfterParser = 0;
    std::uint64_t mAnomalyFrames = 0;
};

/**
 * 单机录制事件采集的诊断计数。游戏线程独占；同一帧号重复进入（暂停）时保留本帧计数，
 * 只有到达帧尾的帧并入汇总。
 */
class EventCaptureDiagnostics final
{
public:
    void BeginFrame(std::uint32_t frameNumber) noexcept;
    void RecordEntry(std::uint32_t staleCount, std::uint32_t currentCount,
        std::uint32_t futureCount, std::uint32_t outListCount) noexcept;
    void RecordDispatch(std::uint32_t originalFrame) noexcept;
    void RecordFrameInfoSkipped() noexcept;
    void RecordUnmarkedDispatch() noexcept;
    void RecordMoveFlash(bool isAfterParser) noexcept;
    /** 帧尾调用：并入汇总并返回本帧计数；本帧已完成或尚未开始时返回空。 */
    std::optional<EventCaptureFrameStats> CompleteFrame() noexcept;
    const EventCaptureTotals& GetTotals() const noexcept { return mTotals; }

private:
    EventCaptureFrameStats mFrame;
    EventCaptureTotals mTotals;
    bool mHasFrame = false;
};
}
