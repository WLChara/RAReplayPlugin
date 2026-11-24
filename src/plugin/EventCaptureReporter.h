#pragma once

#include "../game_api/capture/EventCaptureDiagnostics.h"

#include <cstdint>

namespace rareplay::plugin
{
/**
 * 单机录制事件采集诊断的技术日志。游戏线程独占；逐帧只输出异常帧，最多 MAX_FRAME_LINES 行，
 * 其余异常只计入录制结束时的汇总行。
 */
class EventCaptureReporter final
{
public:
    void ReportContext(const game_api::EventCaptureContext& context) noexcept;
    void ReportFrame(const game_api::EventCaptureFrameStats& stats) noexcept;
    void ReportSummary(const game_api::EventCaptureTotals& totals) noexcept;

private:
    static constexpr std::uint32_t MAX_FRAME_LINES = 50;

    std::uint32_t mFrameLines = 0;
};
}
