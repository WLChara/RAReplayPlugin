#pragma once

#include "../replay/ReplayPorts.h"
#include "PlayerInputEffects.h"
#include "ViewerPlaybackSpeed.h"


namespace rareplay::game_api
{
/** 除注明的初始化入口外，只在游戏线程、已验证目标映像且世界就绪后调用。 */
class GameApi final : public replay::IReplayGame
{
public:
    bool IsWorldReady() const;
    bool ReadSessionInfo(replay::ReplaySessionInfo& outInfo, std::string& error) override;
    bool ReadFrameContext(std::uint32_t& outFrameNumber, std::uint32_t& outChecksum,
        bool& outHasChecksum, std::string& error) override;
    bool CopyDueEvents(std::vector<replay::RecordedEvent>& outEvents, std::string& error) override;
    bool CopyPlayerInputEffects(std::vector<replay::RecordedInputEffect>& outEffects,
        std::string& error) override;
    bool ApplyPlayerInputEffects(const std::vector<replay::RecordedInputEffect>& effects,
        std::string& error) override;
    bool RecordPlayerInputEffect(const replay::RecordedInputEffect& effect,
        std::string& error);
    bool ExecuteReplayFrame(const replay::ReplayFrame& frame, std::string& error) override;
    void DiscardLocalCommands() override;
    void EndReplay() override;
    void BeginFrame();
    void BeforeNativeEventExecution();
    bool TrySetViewerPlaybackSpeed(int setting) noexcept;
    bool TryGetViewerPlaybackSpeed(int& outSetting) const noexcept;
    bool CanControlViewerPlaybackSpeed() const noexcept;
    static void InvokeNativeFrameParser();
    static bool InvokeNativePreworldWait();
    static void DrawViewerOnTop();
    static void ProcessGameOver();
    static bool IsReplayExitRequested();
    static void UpdatePausedCamera();
    void InvalidateViewerShroud();
    void ClearViewerShroud() noexcept;
    /** 仅在已验证目标的 InitRandom 选种后、任一 RNG 初始化前的游戏线程调用；不要求世界就绪。 */
    static void RestoreRandomSeedForInitialization(std::uint32_t seed);

private:
    void ProcessViewerCommands();

    PlayerInputEffects mPlayerInputEffects;
    ViewerPlaybackSpeed mViewerPlaybackSpeed;
    std::string mPlayerInputEffectsError;
    std::uint32_t mActiveFrame = 0;
    bool mIsFrameActive = false;
    bool mHasNativeChecksum = false;
};
}
