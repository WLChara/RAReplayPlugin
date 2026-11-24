#pragma once

#include "EventCaptureDiagnostics.h"
#include "../../replay/ReplayTypes.h"

#include <cstdint>
#include <optional>

namespace rareplay::game_api
{
/** GameApi 在各采集点读取并传入的帧与模式；状态机只依据这些值判断，不读取引擎。 */
struct DispatchCaptureFrame
{
    // GameApi::BeginFrame 记录的帧号，以及该帧是否可采集（世界就绪且输入效果帧已开始）。
    std::uint32_t mActiveFrame = 0;
    bool mIsFrameActive = false;
    // 调用时的 Unsorted::CurrentFrame。
    std::uint32_t mCurrentFrame = 0;
    // 单机（Campaign/Skirmish）在分派点采集；联机由入口 Frame==N 采集，不经过本状态机。
    bool mUsesDispatchCapture = false;
};

/** 分派点是否应读取本条事件。 */
enum class DispatchAdmission
{
    // 采集标记未打开：不采集，录制继续；单机已计入未标记分派诊断，联机不计。
    Unmarked,
    // 标记已打开但当前不在本帧录制范围内：调用方按录制失败处理。
    OutsideActiveFrame,
    // 调用方应按环索引读取事件字节并交给 Capture。
    Admitted
};

/**
 * 单机录制在原生分派点逐条采集事件的状态机与诊断计数。
 *
 * 采集标记由本帧 handleAny 入口打开，帧开始、原生帧解析器返回时关闭；只在打开期间采集分派事件。
 * 帧号、模式与事件字节都由 GameApi 读取后传入，本类不包含 YRpp 或引擎访问。
 * 由 GameApi 持有，只在游戏线程使用。
 */
class DispatchEventCapture final
{
public:
    /** 帧开始：关闭标记，诊断进入该帧；同一帧号重入（暂停）时保留本帧计数。 */
    void BeginFrame(std::uint32_t activeFrame) noexcept;
    /**
     * handleAny 入口：单机、本帧可采集且当前帧仍是本帧时打开标记，否则关闭。
     * @return 标记是否已打开；打开时调用方随后统计入口队列并交给 RecordEntry。
     */
    bool OpenAtEventExecution(const DispatchCaptureFrame& frame) noexcept;
    /** 记入 handleAny 入口时 DoList 中未执行事件的帧关系计数与 OutList 长度。 */
    void RecordEntry(std::uint32_t staleCount, std::uint32_t currentCount,
        std::uint32_t futureCount, std::uint32_t outListCount) noexcept;
    /** 原生帧解析器返回：关闭标记，记下返回时的帧号供 MoveFlash 诊断区分解析器前后。 */
    void CloseAfterFrameParser(std::uint32_t currentFrame) noexcept;
    bool IsOpen() const noexcept { return mIsOpen; }
    /** 分派点第一步：判断是否读取本条事件；标记未打开时在此计入诊断（仅单机）。 */
    DispatchAdmission AdmitDispatch(const DispatchCaptureFrame& frame) noexcept;
    /**
     * 分派点第二步，仅在 AdmitDispatch 返回 Admitted 后调用。FrameInfo 不采集并计入诊断；
     * 其余事件按单机录像规则复制，副本的 Frame 改写为 activeFrame。
     * @return 是否应写入录像；返回 false 时 outEvent 不变。
     */
    bool Capture(const replay::RecordedEvent& dispatched, std::uint32_t activeFrame,
        replay::RecordedEvent& outEvent) noexcept;
    /** 已记录一条玩家输入效果（MoveFlash）后调用；只在单机计数，按本帧解析器是否已返回分开。 */
    void RecordMoveFlash(const DispatchCaptureFrame& frame) noexcept;
    /** 帧尾取出本帧诊断计数；联机、本帧已完成或尚未开始时返回空。 */
    std::optional<EventCaptureFrameStats> CompleteFrame(bool usesDispatchCapture) noexcept;
    const EventCaptureTotals& GetTotals() const noexcept { return mDiagnostics.GetTotals(); }

private:
    EventCaptureDiagnostics mDiagnostics;
    // 最近一次解析器返回时的帧号；不随帧开始清除，MoveFlash 诊断按帧号比较。
    std::uint32_t mParserReturnedFrame = 0;
    bool mIsOpen = false;
    bool mHasParserReturned = false;
};
}
