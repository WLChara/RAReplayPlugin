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
    /**
     * 读取当前游戏速度（GameOptions.GameSpeed）的原值；越界值原样返回，由会话判定。
     * 只读，仅游戏线程在本帧内调用。
     */
    virtual bool ReadGameSpeed(std::int32_t& outSpeed, std::string& error) = 0;
    /** Logic 前写回录制的游戏速度（调用方已保证 0..6）；仅游戏线程调用。 */
    virtual bool ApplyGameSpeed(std::uint32_t speed, std::string& error) = 0;
    /** Logic 前应用完整帧的玩家输入效果；仅游戏线程调用。 */
    virtual bool ApplyPlayerInputEffects(const std::vector<RecordedInputEffect>& effects,
        std::string& error) = 0;
    virtual bool ExecuteReplayFrame(const ReplayFrame& frame, std::string& error) = 0;
    virtual void DiscardLocalCommands() = 0;
    virtual void EndReplay() = 0;
};

/**
 * 播放时每个回放帧被原生执行器成功执行之后通知一次（还未完成该帧）。只读观察，实现方不得抛出异常，
 * 也不得修改游戏状态或帧内容；生命周期由调用方保证长于会话。
 */
class IReplayFrameObserver
{
public:
    virtual ~IReplayFrameObserver() = default;
    virtual void OnPlaybackFrameExecuted(const ReplayFrame& frame) noexcept = 0;
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
