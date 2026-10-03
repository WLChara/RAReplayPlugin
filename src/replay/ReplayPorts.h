#pragma once

#include "ReplayTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rareplay::replay
{
/** 游戏线程上的语义操作；实现方负责目标版本的引擎阶段与队列细节。 */
class IReplayGame
{
public:
    virtual ~IReplayGame() = default;
    virtual bool ReadSessionInfo(ReplaySessionInfo& outInfo, std::string& error) = 0;
    virtual bool ReadFrameContext(std::uint32_t& outFrameNumber, std::uint32_t& outChecksum,
        bool& outHasChecksum, std::string& error) = 0;
    virtual bool CopyDueEvents(std::vector<RecordedEvent>& outEvents, std::string& error) = 0;
    /** 帧尾取出并清空本帧玩家输入效果；仅游戏线程调用。 */
    virtual bool CopyPlayerInputEffects(std::vector<RecordedInputEffect>& outEffects,
        std::string& error) = 0;
    /** v3 录制帧尾采集最终编队变化；空 optional 沿用上一帧，空快照清空全部编队。 */
    virtual bool CopyRecordedControlGroups(std::optional<RecordedControlGroupSnapshot>& outUpdate,
        std::string& error) = 0;
    /** Logic 前应用完整帧的玩家输入效果；仅游戏线程调用。 */
    virtual bool ApplyPlayerInputEffects(const std::vector<RecordedInputEffect>& effects,
        std::string& error) = 0;
    virtual bool ExecuteReplayFrame(const ReplayFrame& frame, std::string& error) = 0;
    virtual void DiscardLocalCommands() = 0;
    virtual void EndReplay() = 0;
};

enum class ReplayReadResult
{
    Frame,
    End,
    Error
};

class IReplaySource
{
public:
    virtual ~IReplaySource() = default;
    virtual bool GetSessionInfo(ReplaySessionInfo& outInfo, std::string& error) = 0;
    virtual ReplayReadResult ReadFrame(ReplayFrame& outFrame, std::string& error) = 0;
};

class IReplaySink
{
public:
    virtual ~IReplaySink() = default;
    virtual bool AppendFrame(const ReplayFrame& frame, std::string& error) = 0;
    virtual bool Finish(std::string& error) = 0;
    virtual void Abort() noexcept = 0;
};
}
