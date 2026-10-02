#include "ReplayViewPublisher.h"
#include "Diagnostics.h"

#include "../game_api/ReplayWorldReader.h"
#include "../viewer/ReplayViewExchange.h"

#include <cstdio>

namespace rareplay::plugin
{
void ReplayViewPublisher::Begin(std::string replayName, int recordedLocalHouseIndex)
{
    // 当前插件每个进程只有一个播放会话；序号仍保留重进场景的隔离语义。
    static std::uint64_t nextSessionId = 0;
    mSessionId = ++nextSessionId;
    mReplayName = std::move(replayName);
    mRecordedLocalHouseIndex = recordedLocalHouseIndex;
    mNextSample = {};
    mHasLoggedLayout = false;
    mWorldReader.Reset();
    mIconLogCount = 0;
    viewer::GetReplayViewExchange().Clear();
}

void ReplayViewPublisher::Publish(std::optional<int> playbackSpeedSetting, bool canChangeSpeed,
    const viewer::PlaybackControls& controls)
{
    const auto now = std::chrono::steady_clock::now();
    if (mSessionId == 0 || now < mNextSample)
        return;
    mNextSample = now + std::chrono::milliseconds(100);
    auto snapshot = std::make_shared<viewer::ReplayViewSnapshot>();
    snapshot->mSessionId = mSessionId;
    snapshot->mActive = true;
    snapshot->mReplayName = mReplayName;
    snapshot->mRecordedLocalHouseIndex = mRecordedLocalHouseIndex;
    snapshot->mCanChangePlaybackSpeed = canChangeSpeed;
    snapshot->mPlaybackControls = controls;
    mWorldReader.Read(*snapshot);
    snapshot->mFrameNumber = controls.mLastCompletedFrame;
    const auto& iconStats = mWorldReader.GetIconStats();
    const std::uint32_t iconAttempts = iconStats.mPcx + iconStats.mShp + iconStats.mMissing;
    if (iconAttempts > mIconLogCount && mIconLogCount < 12)
    {
        char message[256]{};
        std::snprintf(message, sizeof(message), "Replay HUD cameo: PCX=%u SHP=%u missing=%u invalid=%u; %.110s",
            iconStats.mPcx, iconStats.mShp, iconStats.mMissing, iconStats.mInvalidSurface,
            iconStats.mRecentDetail.c_str());
        Log(message);
        mIconLogCount = iconAttempts;
    }
    if (snapshot->mNativeUiLayout && !mHasLoggedLayout)
    {
        const auto& layout = *snapshot->mNativeUiLayout;
        char message[160]{};
        std::snprintf(message, sizeof(message), "Replay HUD layout: canvas=%dx%d nativeContentTop=%d",
            layout.mCanvasWidth, layout.mCanvasHeight, layout.mSidebarContentTop);
        Log(message);
        mHasLoggedLayout = true;
    }
    if (playbackSpeedSetting)
        snapshot->mPlaybackSpeedSetting = *playbackSpeedSetting;
    viewer::GetReplayViewExchange().Publish(std::move(snapshot));
}

void ReplayViewPublisher::End()
{
    const auto& iconStats = mWorldReader.GetIconStats();
    if (mSessionId != 0)
    {
        char message[160]{};
        std::snprintf(message, sizeof(message), "Replay HUD cameo total: PCX=%u SHP=%u missing=%u invalid=%u limit=%u",
            iconStats.mPcx, iconStats.mShp, iconStats.mMissing, iconStats.mInvalidSurface,
            iconStats.mCacheLimit);
        Log(message);
    }
    mWorldReader.Reset();
    mSessionId = 0;
    viewer::GetReplayViewExchange().Clear();
}
}
