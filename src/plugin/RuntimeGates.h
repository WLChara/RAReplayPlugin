#pragma once

#include "../replay/ReplaySession.h"

#include <cstdint>
#include <optional>

namespace rareplay::plugin
{
/**
 * 回放运行时各项门控判定的全部输入；只含值，不含引擎对象，便于离线测试。
 * 由运行时在游戏线程按当前状态逐次填写，判定函数本身不读取任何全局状态。
 */
struct RuntimeGateState
{
    bool mIsStarted = false;
    /** 请求模式为 Play；启动请求读取时已校验模式只有 Play 与 Record 两种，所以为假即录制。 */
    bool mIsPlayback = false;
    replay::ReplayState mReplayState = replay::ReplayState::Inactive;
    bool mIsPlaybackSourceOpen = false;
    bool mHasStartFailed = false;
    bool mHasExitRequested = false;
    /** 回放界面已因异常停用；只停界面，不影响回放会话。 */
    bool mHasViewFailed = false;
    bool mIsHalfShroudEnabled = false;
    /** 时间表扫描已给出总帧数；扫描未结束或失败时为假。 */
    bool mHasTotalFrames = false;
    /**
     * 首次载入时的会话快照已取得，且可用于进程内重开（game_api 的 CanRestartFromSnapshot：不是战役）；
     * 重开要用它还原会话标量。
     */
    bool mCanRestartFromSnapshot = false;
};

/** 回放已开始、未请求退出、界面未失败，状态为播放、暂停或结束。 */
bool IsPlaybackViewActive(const RuntimeGateState& state) noexcept;

/** 回放已开始、半黑幕开启、未请求退出，状态为播放、暂停或结束；不看界面是否失败。 */
bool ShouldUseHalfShroud(const RuntimeGateState& state) noexcept;

/** 录制已开始且会话处于录制中。 */
bool IsRecordingActive(const RuntimeGateState& state) noexcept;

/** 录制已开始，不看会话状态。 */
bool IsRecordingStarted(const RuntimeGateState& state) noexcept;

/** 回放源已打开且未启动失败；不要求回放已开始。 */
bool ShouldBlockPlayerCommands(const RuntimeGateState& state) noexcept;

/** 回放已开始且会话处于播放中（不含暂停与结束）。 */
bool IsPlaybackPlaying(const RuntimeGateState& state) noexcept;

/**
 * 可以向前跳转（快照的 mCanSeek，也是开始快进的前提）：观看界面可用（IsPlaybackViewActive：已开始、回放、未请求退出、
 * 界面未失败）、状态为播放或暂停（不含结束），且总帧数已知。快进中状态为播放，所以仍为真，可以改目标。
 */
bool CanSeekPlayback(const RuntimeGateState& state) noexcept;

/**
 * 进程内重开（同进程重新播放与向后跳转）的前提，在开始拆除之前判定，也是快照的 mCanSeekBackward：
 * 首次载入的会话快照已取得且可用于重开（不是战役）；当前是已开始的播放会话（回放源已打开、未启动失败）；未请求退出；
 * 会话处于播放、暂停或结束（不含失败与已停止）。快进中仍为真。不看界面是否失败与总帧数：
 * 重新播放不需要它们，向后跳转的目标由路由按总帧数夹取。原生侧的检查（g_GameActive 等）由执行方在拆除前另做。
 */
bool CanRestartInProcess(const RuntimeGateState& state) noexcept;

/**
 * BEFORE_SIMULATION 是否交给会话：播放中（写回速度并重演输入效果）或录制进行中（采样游戏速度）。
 * 即 IsPlaybackPlaying 或 IsRecordingActive；会话再按自身状态分支。
 */
bool ShouldRunBeforeSimulation(const RuntimeGateState& state) noexcept;

/**
 * AircraftClass::Draw 画影子时的三次 SetHeight 是否中和其模拟副作用（R1）：
 * 播放请求一律中和；录制请求只在单机（gameMode 为 0 Campaign 或 5 Skirmish，即 FrameCodec::IsOfflineGameMode）中和。
 * 联机录制不中和，以免改变联机对局中本机的模拟。
 *
 * 只取决于启动请求类型与当前会话模式，与会话是否已开始、是否失败无关：从插件接受请求起（开局前、第一次绘制之前）
 * 到会话结束都适用，录制与播放两端因此从同一时刻开始中和。帧流 v5 的单机录像以此为录制契约。
 */
bool ShouldNeutralizeAircraftShadowSideEffects(bool isPlaybackRequest, std::uint32_t gameMode) noexcept;

/**
 * 载入提速 L1：DoBlit 整屏送屏后的 Sleep(50) 是否跳过。只在已接受的启动请求是播放、播放尚未开始、且没有启动失败时为真，
 * 也就是开局载入期间（载入画面的进度绘制、StartScenario 与 spawner 的整屏送屏）。录制、开始播放之后（对话框、影片等）
 * 以及启动失败后都保持原生等待；没有已接受请求时（bootstrap 之前）由调用方直接判为假。
 *
 * 只看“播放请求已接受且尚未开始”，不看载入进行到哪一步：以后若在同一进程内重开回放，重开的载入期间运行时会把
 * “尚未开始”重新置回，同一判定照样生效。被跳过的只是挂钟等待，不读写模拟状态。
 */
bool ShouldSkipLoadBlitSleep(const RuntimeGateState& state) noexcept;

/** 只读状态摘要的固定记录间隔（帧）：第 0 帧与此后帧号为它的倍数的帧完成时各记一次。 */
inline constexpr std::uint32_t STATE_DIGEST_FRAME_INTERVAL = 1000;

/**
 * 帧完成点是否记录只读状态摘要：只在回放播放中（IsPlaybackPlaying，录制、暂停、结束与失败都不记），且本帧帧号是
 * STATE_DIGEST_FRAME_INTERVAL 的倍数（含第 0 帧）或本帧是快进到达的目标帧；同一帧号已记过（lastRecordedFrame）时不再记。
 * 快进中照常按间隔记录。
 */
bool ShouldRecordStateDigest(const RuntimeGateState& state, std::uint32_t completedFrame, bool isSeekReached,
    std::optional<std::uint32_t> lastRecordedFrame) noexcept;
}
