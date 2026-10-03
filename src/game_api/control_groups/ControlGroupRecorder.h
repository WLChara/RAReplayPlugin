#pragma once

#include "../../replay/RecordedControlGroups.h"

namespace rareplay::game_api
{
struct ControlGroupCaptureStats
{
    std::uint64_t mNativeUpdateCount = 0;
    std::uint64_t mSnapshotUpdateCount = 0;
};

/** 游戏线程拥有采样和上次输出的全量表；采样器只读原生世界，不分配引擎对象 ID。 */
class ControlGroupRecorder final
{
public:
    using SnapshotReader = bool (*)(replay::RecordedControlGroupSnapshot&, std::string&);

    explicit ControlGroupRecorder(SnapshotReader reader) : mReader(reader) {}
    bool CaptureAfterNativeUpdate(std::string& error);
    /** 帧尾重新采样生命周期变化；首次必输出，即使为空。后续仅输出变化的完整表。 */
    bool CopyFrameUpdate(std::uint32_t frameNumber,
        std::optional<replay::RecordedControlGroupSnapshot>& outUpdate, std::string& error);
    void Reset() noexcept;
    ControlGroupCaptureStats GetStats() const noexcept { return mStats; }

private:
    bool Capture(std::uint32_t frameNumber, std::string& error);

    SnapshotReader mReader;
    std::optional<replay::RecordedControlGroupSnapshot> mCaptured;
    std::optional<replay::RecordedControlGroupSnapshot> mLastOutput;
    ControlGroupCaptureStats mStats;
};
}
