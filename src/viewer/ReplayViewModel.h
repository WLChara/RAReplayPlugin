#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rareplay::viewer
{
enum class UnitCategory { Infantry, Vehicle, Aircraft, Building, Other };

/** 图像是独立的 RGBA 像素副本；不持有游戏对象或图形设备指针。 */
struct IconImage
{
    std::string mKey;
    int mWidth = 0;
    int mHeight = 0;
    std::vector<std::uint8_t> mRgba;
};

struct UnitSummary
{
    std::string mTypeId;
    std::string mName;
    UnitCategory mCategory = UnitCategory::Other;
    std::uint32_t mCount = 0;
    std::shared_ptr<const IconImage> mIcon;
};

struct ProductionLine
{
    std::uint32_t mId = 0;
    std::string mLabel;
    std::string mProductName;
    std::shared_ptr<const IconImage> mIcon;
    float mProgress = 0.0f;
    bool mOnHold = false;
    bool mReady = false;
    std::uint32_t mQueuedCount = 0;
};

struct PlayerSummary
{
    int mHouseIndex = -1;
    std::string mName;
    std::string mFaction;
    std::uint32_t mColorRgb = 0xD4A753;
    bool mIsHuman = false;
    bool mDefeated = false;
    std::optional<std::int64_t> mCredits;
    std::optional<int> mPowerOutput;
    std::optional<int> mPowerDrain;
    bool mHasProductionData = false;
    bool mHasUnitData = false;
    std::vector<ProductionLine> mProductionLines;
    std::vector<UnitSummary> mUnits;
};

struct ControlGroup
{
    int mNumber = 0;
    std::vector<UnitSummary> mUnits;
    std::uint32_t mUnmatchedMemberCount = 0;
};

struct FpsSample
{
    std::uint32_t mFrameNumber = 0;
    float mFps = 0.0f;
};

/** 原生游戏逻辑像素中的侧栏保留区；由游戏线程采样，渲染层负责缩放映射。 */
struct NativeUiLayout
{
    int mCanvasWidth = 0;
    int mCanvasHeight = 0;
    int mSidebarContentTop = 0;
};

enum class PlaybackStatus { Playing, Paused, Ended };
struct PlaybackControls
{
    PlaybackStatus mStatus = PlaybackStatus::Playing;
    std::uint32_t mLastCompletedFrame = 0;
    bool mCanRestart = false;
    bool mIsHalfShroudEnabled = false;
    std::string mRestartError;
};

/** 游戏线程发布的只读值快照；缺失数据用 optional/能力标志表示，不能当作零。 */
struct ReplayViewSnapshot
{
    std::uint64_t mSessionId = 0;
    bool mActive = false;
    bool mIsPreview = false;
    std::string mReplayName;
    std::string mMapName;
    std::string mMapHash;
    std::optional<std::int64_t> mStartedUtc;
    bool mWasNetworkGame = false;
    std::uint32_t mFrameNumber = 0;
    std::optional<std::uint32_t> mTotalFrames;
    int mRecordedLocalHouseIndex = -1;
    int mPlaybackSpeedSetting = 0;
    bool mCanChangePlaybackSpeed = false;
    PlaybackControls mPlaybackControls;
    bool mHasPlayerVision = false;
    bool mHasRecordedGroups = false;
    std::optional<NativeUiLayout> mNativeUiLayout;
    std::vector<PlayerSummary> mPlayers;
    std::vector<ControlGroup> mRecordedGroups;
    std::vector<FpsSample> mRecordedFps;
};

enum class ViewerCommandKind { SetPlaybackSpeed, SetPaused, ExitPlayback, RestartPlayback, SetHalfShroud };
struct ViewerCommand
{
    std::uint64_t mSessionId = 0;
    ViewerCommandKind mKind = ViewerCommandKind::SetPlaybackSpeed;
    int mValue = 0;
};
}
