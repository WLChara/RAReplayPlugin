#pragma once

#include "../replay/ReplayPorts.h"
#include "PlayerInputEffects.h"
#include "ViewerPlaybackSpeed.h"
#include "capture/DispatchEventCapture.h"
#include "capture/EventCaptureDiagnostics.h"
#include "control_groups/ControlGroupRecorder.h"


namespace rareplay::game_api
{
/** 除注明的初始化入口外，只在游戏线程、已验证目标映像且世界就绪后调用。 */
class GameApi final : public replay::IReplayGame
{
public:
    GameApi();
    bool IsWorldReady() const;
    bool ReadSessionInfo(replay::ReplaySessionInfo& outInfo, std::string& error) override;
    bool ReadFrameContext(std::uint32_t& outFrameNumber, std::uint32_t& outChecksum,
        bool& outHasChecksum, std::string& error) override;
    bool CopyDueEvents(std::vector<replay::RecordedEvent>& outEvents, std::string& error) override;
    bool CopyPlayerInputEffects(std::vector<replay::RecordedInputEffect>& outEffects,
        std::string& error) override;
    bool CopyRecordedControlGroups(std::optional<replay::RecordedControlGroupSnapshot>& outUpdate,
        std::string& error) override;
    bool RecordControlGroupUpdate(std::string& error);
    ControlGroupCaptureStats GetControlGroupCaptureStats() const noexcept;
    bool ReadGameSpeed(std::int32_t& outSpeed, std::string& error) override;
    bool ApplyGameSpeed(std::uint32_t speed, std::string& error) override;
    bool ApplyPlayerInputEffects(const std::vector<replay::RecordedInputEffect>& effects,
        std::string& error) override;
    bool RecordPlayerInputEffect(const replay::RecordedInputEffect& effect,
        std::string& error);
    bool ExecuteReplayFrame(const replay::ReplayFrame& frame, std::string& error) override;
    void DiscardLocalCommands() override;
    void EndReplay() override;
    void BeginFrame();
    /** 仅录制时在 handleAny 入口调用：准备联机 CRC，单机则打开本帧分派采集标记并统计入口队列。 */
    void BeforeNativeEventExecution();
    /** 原生帧解析器（单机 handleAny 的唯一调用方）返回后调用：关闭分派采集标记并记录解析器已运行。 */
    void AfterNativeFrameParser() noexcept;
    /** 单机（Campaign/Skirmish）在原生分派点逐条采集；联机保持入口 Frame==N 采集。 */
    bool UsesDispatchEventCapture() const noexcept;
    bool IsCapturingDispatchedEvents() const noexcept { return mDispatchCapture.IsOpen(); }
    /**
     * 仅录制时在 handleAny 分派点调用。标记未打开时不采集（单机计入诊断）并返回 true；
     * FrameInfo 不采集。outIsCaptured 为 true 时 outEvent 是 Frame 已改写为当前帧的副本。
     */
    bool CopyDispatchedEvent(std::uint32_t ringIndex, replay::RecordedEvent& outEvent,
        bool& outIsCaptured, std::string& error);
    EventCaptureContext ReadEventCaptureContext() const;
    /** 帧尾取出本帧诊断计数；联机或本帧没有开始时返回空。 */
    std::optional<EventCaptureFrameStats> CompleteEventCaptureFrame() noexcept;
    const EventCaptureTotals& GetEventCaptureTotals() const noexcept;
    bool TrySetViewerPlaybackSpeed(int setting) noexcept;
    bool TryGetViewerPlaybackSpeed(int& outSetting) const noexcept;
    bool CanControlViewerPlaybackSpeed() const noexcept;
    // 以下供 plugin 使用；Hook 需要的原生调用直接调用 game_api::native 的自由函数。
    /** SessionClass::GameMode 原值；不要求世界就绪，开局前也可调用。 */
    std::uint32_t ReadSessionGameMode() const noexcept;
    bool IsReplayExitRequested() const;
    void UpdatePausedCamera();
    void InvalidateViewerShroud();
    void ClearViewerShroud() noexcept;
    // 向前跳转（快进）的游戏线程入口，语义见 native/NativeGame.h 的同名函数；本身不保存状态，是否静音由调用方持有。
    // 当前游戏速度复用 ReadGameSpeed。
    /** 本帧不限速；只在 FRAME_COMPLETE（RegulateFrameRate 之前）调用才对本帧有效。 */
    void ClearFrameBudgets() noexcept;
    /** 请求下一次绘制完整重绘（战术区与侧栏、命令条）；只在世界存在时调用。 */
    void RequestFullRedraw() noexcept;
    void MuteAudioForSeek() noexcept;
    void RestoreAudioAfterSeek() noexcept;

private:
    /**
     * 只由 DiscardLocalCommands 在清空原生 OutList（本机输入产生、等待排入执行队列的事件）之前调用，
     * 从中只取两类本机请求：Options 转为打开原生选项菜单，GameSpeed 转为观看调速（仅遭遇战生效）。
     * 其余事件不执行；本函数不清空 OutList。与 viewer::ViewerCommand（HUD 观看命令）无关。
     */
    void ConsumeLocalOutList();
    void RecordEntryDiagnostics();
    /** 按本帧上下文与当前引擎帧号、模式组装分派采集状态机的输入。 */
    DispatchCaptureFrame ReadDispatchCaptureFrame() const noexcept;

    PlayerInputEffects mPlayerInputEffects;
    ControlGroupRecorder mControlGroupRecorder;
    ViewerPlaybackSpeed mViewerPlaybackSpeed;
    // 单机分派采集标记、解析器返回帧与诊断计数；帧号与模式由 GameApi 每次传入。
    DispatchEventCapture mDispatchCapture;
    std::string mPlayerInputEffectsError;
    // 本帧上下文：录制、播放、输入效果与编队共用，分派采集只读取。
    std::uint32_t mActiveFrame = 0;
    bool mIsFrameActive = false;
    bool mHasNativeChecksum = false;
};
}
