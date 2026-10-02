#pragma once

#include "ReplayPorts.h"

#include <cstdint>
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
    /** 原生 Input 之后、Logic.Update 之前应用预取帧的玩家输入效果。 */
    bool OnBeforeSimulation();
    bool OnBeforeEvents();
    bool OnFrameEnd();
    /** 在完整帧的原生递增之后调用，预取下一帧并在 EOF 保留世界；可重复调用。 */
    bool OnAfterFrameLoop();
    bool SetPaused(bool paused) noexcept;
    bool IsPlaybackHeld() const noexcept;
    std::uint32_t GetLastCompletedFrame() const noexcept;
    /** 播放开始后始终返回 true，要求调用方绕过真实网络路径。 */
    bool ProcessPlaybackFrame();
    bool Finish();
    void Stop();

    ReplayState GetState() const noexcept;
    const std::string& GetLastError() const noexcept;

private:
    bool FailRecording(const std::string& error);
    bool FailPlayback(const std::string& error);
    void EndPlayback();
    bool PrefetchPlaybackFrame();

    IReplayGame& mGame;
    IReplaySource* mSource = nullptr;
    IReplaySink* mSink = nullptr;
    ReplayFrame mPendingFrame;
    ReplayFrame mReadyFrame;
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
