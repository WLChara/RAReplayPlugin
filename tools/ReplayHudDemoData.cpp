#include "ReplayHudDemoData.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rareplay::tools
{
namespace
{
using Rgba = std::array<std::uint8_t, 4>;

void SetPixel(viewer::IconImage& image, int x, int y, const Rgba& color)
{
    if (x < 0 || y < 0 || x >= image.mWidth || y >= image.mHeight)
    {
        return;
    }
    const std::size_t offset = (static_cast<std::size_t>(y) * image.mWidth + x) * 4;
    std::copy(color.begin(), color.end(), image.mRgba.begin() + offset);
}

void FillRect(viewer::IconImage& image, int left, int top, int right, int bottom, const Rgba& color)
{
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            SetPixel(image, x, y, color);
        }
    }
}

std::shared_ptr<const viewer::IconImage> CreateNeutralIcon(const std::string& key, viewer::UnitCategory category,
                                                           const Rgba& accent)
{
    auto image = std::make_shared<viewer::IconImage>();
    image->mKey = key;
    image->mWidth = 36;
    image->mHeight = 36;
    image->mRgba.resize(static_cast<std::size_t>(image->mWidth * image->mHeight * 4));
    const Rgba background{22, 30, 27, 255};
    const Rgba border{100, 118, 104, 255};
    const Rgba light{224, 213, 178, 255};
    for (int y = 0; y < image->mHeight; ++y)
    {
        for (int x = 0; x < image->mWidth; ++x)
        {
            SetPixel(*image, x, y, background);
        }
    }
    FillRect(*image, 1, 1, 35, 2, border);
    FillRect(*image, 1, 34, 35, 35, border);
    FillRect(*image, 1, 2, 2, 34, border);
    FillRect(*image, 34, 2, 35, 34, border);

    switch (category)
    {
    case viewer::UnitCategory::Infantry:
        FillRect(*image, 15, 7, 21, 14, light);
        FillRect(*image, 11, 15, 25, 27, accent);
        FillRect(*image, 9, 27, 16, 31, light);
        FillRect(*image, 20, 27, 27, 31, light);
        break;
    case viewer::UnitCategory::Vehicle:
        FillRect(*image, 7, 14, 29, 25, accent);
        FillRect(*image, 12, 9, 24, 14, light);
        FillRect(*image, 9, 25, 15, 30, light);
        FillRect(*image, 22, 25, 28, 30, light);
        break;
    case viewer::UnitCategory::Aircraft:
        FillRect(*image, 16, 5, 20, 30, light);
        FillRect(*image, 5, 13, 31, 18, accent);
        FillRect(*image, 11, 23, 25, 27, accent);
        break;
    case viewer::UnitCategory::Building:
        FillRect(*image, 7, 12, 29, 29, accent);
        FillRect(*image, 12, 17, 16, 22, light);
        FillRect(*image, 20, 17, 24, 22, light);
        FillRect(*image, 15, 24, 21, 29, light);
        break;
    default:
        FillRect(*image, 9, 9, 27, 27, accent);
        FillRect(*image, 14, 14, 22, 22, light);
        break;
    }
    return image;
}

viewer::UnitSummary MakeUnit(std::string typeId, std::string name, viewer::UnitCategory category,
                             std::uint32_t count, const std::shared_ptr<const viewer::IconImage>& icon)
{
    viewer::UnitSummary unit;
    unit.mTypeId = std::move(typeId);
    unit.mName = std::move(name);
    unit.mCategory = category;
    unit.mCount = count;
    unit.mIcon = icon;
    return unit;
}

viewer::ProductionLine MakeLine(std::uint32_t id, std::string label, std::string productName,
                                const std::shared_ptr<const viewer::IconImage>& icon, float progress,
                                std::uint32_t queuedCount = 0, bool ready = false, bool onHold = false)
{
    viewer::ProductionLine line;
    line.mId = id;
    line.mLabel = std::move(label);
    line.mProductName = std::move(productName);
    line.mIcon = icon;
    line.mProgress = progress;
    line.mQueuedCount = queuedCount;
    line.mReady = ready;
    line.mOnHold = onHold;
    return line;
}
}

std::shared_ptr<viewer::ReplayViewSnapshot> CreateReplayHudDemoSnapshot(const ReplayHudDemoOptions& options)
{
    const auto infantryIcon = CreateNeutralIcon("preview-infantry", viewer::UnitCategory::Infantry, {172, 169, 120, 255});
    const auto vehicleIcon = CreateNeutralIcon("preview-vehicle", viewer::UnitCategory::Vehicle, {167, 127, 70, 255});
    const auto aircraftIcon = CreateNeutralIcon("preview-aircraft", viewer::UnitCategory::Aircraft, {112, 157, 163, 255});
    const auto buildingIcon = CreateNeutralIcon("preview-building", viewer::UnitCategory::Building, {116, 143, 98, 255});

    auto snapshot = std::make_shared<viewer::ReplayViewSnapshot>();
    snapshot->mSessionId = 1;
    snapshot->mActive = true;
    snapshot->mIsPreview = true;
    snapshot->mReplayName = "示例回放_01.yrreplay";
    snapshot->mMapName = "Crossroads · 示例地图";
    snapshot->mMapHash = "preview-only";
    snapshot->mStartedUtc = 1'767'226'800;
    snapshot->mWasNetworkGame = true;
    snapshot->mFrameNumber = 28'540;
    snapshot->mPlaybackControls.mLastCompletedFrame = snapshot->mFrameNumber;
    snapshot->mPlaybackControls.mCanRestart = true;
    snapshot->mPlaybackControls.mIsHalfShroudEnabled = true;
    snapshot->mTotalFrames = 64'800;
    snapshot->mRecordedLocalHouseIndex = 1;
    snapshot->mPlaybackSpeedSetting = 1;
    snapshot->mCanChangePlaybackSpeed = true;
    snapshot->mHasPlayerVision = false;
    snapshot->mHasRecordedGroups = true;
    // 示例游戏画布及原生雷达下方建造区起点，不是假装从运行中游戏取得的数据。
    snapshot->mNativeUiLayout = viewer::NativeUiLayout{1600, 900, 235};

    viewer::PlayerSummary localPlayer;
    localPlayer.mHouseIndex = 1;
    localPlayer.mName = "北岸指挥部";
    localPlayer.mFaction = "盟军";
    localPlayer.mColorRgb = 0x4E9A78;
    localPlayer.mIsHuman = true;
    localPlayer.mCredits = 6'450;
    localPlayer.mPowerOutput = 1'480;
    localPlayer.mPowerDrain = 1'120;
    localPlayer.mHasProductionData = true;
    localPlayer.mHasUnitData = true;
    localPlayer.mProductionLines = {
        MakeLine(1, "战车工厂", "灰熊坦克", vehicleIcon, 0.72f, 2),
        MakeLine(2, "兵营", "火箭飞行兵", infantryIcon, 1.0f, 0, true),
        MakeLine(3, "空军指挥部", "入侵者战机", aircraftIcon, 0.18f, 1, false, true)};
    localPlayer.mUnits = {
        MakeUnit("E1", "美国大兵", viewer::UnitCategory::Infantry, 18, infantryIcon),
        MakeUnit("FV", "多功能步兵车", viewer::UnitCategory::Vehicle, 5, vehicleIcon),
        MakeUnit("MTNK", "灰熊坦克", viewer::UnitCategory::Vehicle, 7, vehicleIcon),
        MakeUnit("ORCA", "入侵者战机", viewer::UnitCategory::Aircraft, 3, aircraftIcon),
        MakeUnit("GAPOWR", "发电厂", viewer::UnitCategory::Building, 4, buildingIcon)};

    viewer::PlayerSummary aiPlayer;
    aiPlayer.mHouseIndex = 2;
    aiPlayer.mName = "电脑对手";
    aiPlayer.mFaction = "苏联";
    aiPlayer.mColorRgb = 0xB45044;
    aiPlayer.mIsHuman = false;
    aiPlayer.mCredits = 4'200;
    aiPlayer.mPowerOutput = 0;
    aiPlayer.mPowerDrain = 860;
    aiPlayer.mHasProductionData = true;
    aiPlayer.mHasUnitData = true;
    aiPlayer.mProductionLines = {
        MakeLine(11, "战车工厂（北）", "犀牛坦克", vehicleIcon, 0.91f, 3),
        MakeLine(12, "兵营（东）", "动员兵", infantryIcon, 0.43f, 8),
        MakeLine(13, "战车工厂（南）", "犀牛坦克", vehicleIcon, 0.12f, 1),
        MakeLine(14, "兵营（西）", "磁暴步兵", infantryIcon, 1.0f, 0, true)};
    aiPlayer.mUnits = {
        MakeUnit("E2", "动员兵", viewer::UnitCategory::Infantry, 27, infantryIcon),
        MakeUnit("HTNK", "犀牛坦克", viewer::UnitCategory::Vehicle, 11, vehicleIcon),
        MakeUnit("Kirov", "基洛夫飞艇", viewer::UnitCategory::Aircraft, 1, aircraftIcon),
        MakeUnit("NARADR", "雷达站", viewer::UnitCategory::Building, 1, buildingIcon)};

    viewer::PlayerSummary thirdPlayer;
    thirdPlayer.mHouseIndex = 3;
    thirdPlayer.mName = "东部基地";
    thirdPlayer.mFaction = "尤里";
    thirdPlayer.mColorRgb = 0x8B68A8;
    thirdPlayer.mIsHuman = true;
    thirdPlayer.mDefeated = true;
    thirdPlayer.mCredits = 0;
    thirdPlayer.mPowerOutput = 760;
    thirdPlayer.mPowerDrain = 690;
    thirdPlayer.mHasProductionData = false;
    thirdPlayer.mHasUnitData = false;

    snapshot->mPlayers = {std::move(localPlayer), std::move(aiPlayer), std::move(thirdPlayer)};
    snapshot->mRecordedGroups = {
        {1, {MakeUnit("E1", "美国大兵", viewer::UnitCategory::Infantry, 6, infantryIcon),
             MakeUnit("FV", "多功能步兵车", viewer::UnitCategory::Vehicle, 2, vehicleIcon)}},
        {2, {MakeUnit("HTNK", "灰熊坦克", viewer::UnitCategory::Vehicle, 4, vehicleIcon)}},
        {3, {MakeUnit("ORCA", "入侵者战机", viewer::UnitCategory::Aircraft, 3, aircraftIcon)}}};
    snapshot->mRecordedGroups.front().mUnmatchedMemberCount = 1;

    snapshot->mRecordedFps.reserve(90);
    for (std::uint32_t index = 0; index < 90; ++index)
    {
        const float wave = static_cast<float>((index * 7u) % 19u);
        snapshot->mRecordedFps.push_back({snapshot->mFrameNumber - 89u + index, 54.0f + wave});
    }

    if (options.mMissingData)
    {
        snapshot->mStartedUtc.reset();
        snapshot->mMapHash.clear();
        snapshot->mHasRecordedGroups = false;
        snapshot->mRecordedGroups.clear();
        snapshot->mRecordedFps.clear();
        snapshot->mTotalFrames.reset();
        snapshot->mPlayers.front().mCredits.reset();
        snapshot->mPlayers.front().mHasProductionData = false;
        snapshot->mPlayers.front().mHasUnitData = false;
        snapshot->mPlayers.front().mProductionLines.clear();
        snapshot->mPlayers.front().mUnits.clear();
    }

    (void)options.mPanel;
    return snapshot;
}
}
