#pragma once

#include "ReplayPorts.h"

#include <cstdint>
#include <optional>
#include <string>

namespace rareplay::replay
{
enum class ReplayState
{
    Inactive,
    Recording,
    Playing,
    Paused,
    Ended,
    Finished,
    Failed
};

/** 回放第 0 帧写回游戏速度前，游戏当前值与录制值不同时的记录；只用于诊断日志。 */
struct GameSpeedDifference
{
    std::uint32_t mRecorded = 0;
    std::int32_t mCurrent = 0;
};

/**
 * 游戏线程上的唯一回放状态所有者。端口对象由调用方持有，必须活得比本会话长。
 * 录制帧仅在 OnFrameEnd 提交；最后一个未完成帧会被丢弃。
 */
class ReplaySession
{
public:
    explicit ReplaySession(IReplayGame& game);

    bool StartRecording(IReplaySink& sink);
    bool StartPlayback(IReplaySource& source);
    bool OnFrameBegin();
    /**
     * 原生 Input 之后、Logic.Update 之前（BEFORE_SIMULATION）调用，按会话状态分支：
     * - 播放：先写回录制的游戏速度（从第一次读到该字段起每帧都写），再应用预取帧的玩家输入效果；
     * - 录制：v4 起（含 v5）的单机帧流采样游戏速度，首帧或与上一条已提交值不同时记入本帧，除此之外没有副作用；
     * - 其他状态不做任何事。
     */
    bool OnBeforeSimulation();
    bool OnBeforeEvents();
    /**
     * 录制时原生执行器每分派一条应录事件调用一次（须在本帧 OnBeforeEvents 之后），按分派顺序追加到本帧，
     * 帧尾随 OnFrameEnd 一并提交。事件字节由游戏适配层准备，会话不解释其内容。
     */
    bool OnEventDispatched(const RecordedEvent& event);
    bool OnFrameEnd();
    /** 在完整帧的原生递增之后调用，预取下一帧并在 EOF 保留世界；可重复调用。 */
    bool OnAfterFrameLoop();
    bool SetPaused(bool paused) noexcept;
    bool IsPlaybackHeld() const noexcept;
    std::uint32_t GetLastCompletedFrame() const noexcept;
    /** 仅包含已完成帧的观看元数据；预取或执行中途的帧不会改变此值。 */
    const std::optional<RecordedControlGroupSnapshot>& GetRecordedControlGroups() const noexcept;
    /**
     * 取出回放第 0 帧写回前发现的速度差异并清空；每个播放会话最多产生一次，由调用方记日志。
     * 没有差异或已取出时返回空。
     */
    std::optional<GameSpeedDifference> TakeInitialGameSpeedDifference() noexcept;
    /** 播放开始后始终返回 true，要求调用方绕过真实网络路径。 */
    bool ProcessPlaybackFrame();
    bool Finish();
    void Stop();
    /**
     * 进程内重开：播放会话（播放、暂停或结束）回到构造后的 Inactive，之后可再次 StartPlayback，从第 0 帧重新播放。
     * 与 Stop 不同，不调用 IReplayGame::EndReplay（世界由调用方重开，不退出），也不读写 source（由调用方重新打开）；
     * 帧观察者同样清空，由下一次开始观看时重新登记。录制、失败、未开始或已停止的会话不变并返回 false。
     */
    bool ResetForRestart() noexcept;
    /** 播放时每个帧被成功执行后通知观察者；传空指针取消。观察者须活得比会话长，会话不接管其所有权。 */
    void SetFrameObserver(IReplayFrameObserver* observer) noexcept { mFrameObserver = observer; }

    ReplayState GetState() const noexcept;
    const std::string& GetLastError() const noexcept;

private:
    bool FailRecording(const std::string& error);
    bool FailPlayback(const std::string& error);
    void EndPlayback();
    bool PrefetchPlaybackFrame();
    bool SampleRecordingGameSpeed();
    bool ApplyRecordedGameSpeed(std::string& error);

    IReplayGame& mGame;
    IReplaySource* mSource = nullptr;
    IReplaySink* mSink = nullptr;
    IReplayFrameObserver* mFrameObserver = nullptr;
    ReplayFrame mPendingFrame;
    ReplayFrame mReadyFrame;
    std::optional<RecordedControlGroupSnapshot> mCurrentGroups;
    std::optional<RecordedControlGroupSnapshot> mProcessedGroupUpdate;
    std::uint32_t mStreamVersion = 2;
    // 帧流头的 gameMode（录制取运行时，播放取录像；二者在播放开始时已核对一致）。
    std::uint32_t mGameMode = 0;
    // 录制：最后一条已提交记录中的游戏速度；用于判断本帧是否需要写入速度。
    std::optional<std::uint32_t> mLastRecordedGameSpeed;
    // 播放：从第一次读到速度字段起的当前录制速度，每帧模拟前写回。
    std::optional<std::uint32_t> mPlaybackGameSpeed;
    std::optional<GameSpeedDifference> mInitialGameSpeedDifference;
    bool mHasCheckedInitialGameSpeed = false;
    std::uint64_t mNextFrame = 0;
    std::uint64_t mLastProcessedFrame = UINT64_MAX;
    ReplayState mState = ReplayState::Inactive;
    bool mHasPendingFrame = false;
    bool mHasReadyFrame = false;
    bool mHasAppliedInputEffects = false;
    bool mHasCopiedEvents = false;
    bool mWasPlayback = false;
    bool mHasEndedGame = false;
    bool mHasCompletedPlaybackFrame = false;
    std::uint32_t mLastCompletedFrame = 0;
    std::string mLastError;
};
}
