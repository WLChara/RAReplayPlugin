#include "ReplayWorldReader.h"
#include "HouseRoster.h"
#include "NativeSidebar.h"
#include "icons/CameoReader.h"
#include "names/NativeDisplayNames.h"
#include "../core/encoding/Utf8.h"
#include "control_groups/RecordedControlGroupProjection.h"

using byte = unsigned char;

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <FactoryClass.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Unsorted.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace rareplay::game_api
{
namespace
{
viewer::UnitCategory Category(AbstractType type)
{
    switch (type)
    {
    case AbstractType::Infantry: return viewer::UnitCategory::Infantry;
    case AbstractType::Unit: return viewer::UnitCategory::Vehicle;
    case AbstractType::Aircraft: return viewer::UnitCategory::Aircraft;
    case AbstractType::Building: return viewer::UnitCategory::Building;
    default: return viewer::UnitCategory::Other;
    }
}
}

struct ReplayWorldReader::Impl
{
    CameoReader mCameos;
};

ReplayWorldReader::ReplayWorldReader() : mImpl(std::make_unique<Impl>()) {}
ReplayWorldReader::~ReplayWorldReader() = default;

void ReplayWorldReader::Reset() noexcept
{
    mImpl->mCameos.Clear();
}

const IconStats& ReplayWorldReader::GetIconStats() const noexcept
{
    return mImpl->mCameos.GetStats();
}

void ReplayWorldReader::Read(viewer::ReplayViewSnapshot& snapshot,
    const std::optional<replay::RecordedControlGroupSnapshot>& recordedGroups)
{
    mImpl->mCameos.BeginSample();
    snapshot.mFrameNumber = static_cast<std::uint32_t>(Unsorted::CurrentFrame);
    snapshot.mPlaybackSpeedSetting = GameOptionsClass::Instance.GameSpeed;
    // 面板直接占用 SidebarBounds；雷达块与资金条在其上方，均为不随分辨率缩放的常量布局。
    snapshot.mNativeUiLayout = ReadNativeUiLayout();
    snapshot.mIsNativeRadarAvailable = IsNativeRadarAvailable();
    snapshot.mWasNetworkGame = SessionClass::Instance.GameMode == GameMode::LAN ||
        SessionClass::Instance.GameMode == GameMode::Internet;
    if (ScenarioClass::Instance)
    {
        snapshot.mMapName = core::encoding::Utf8(ScenarioClass::Instance->UINameLoaded, 0x2D);
        if (snapshot.mMapName.empty())
            snapshot.mMapName = core::encoding::Utf8(ScenarioClass::Instance->Name, 0x2D);
    }

    std::unordered_map<HouseClass*, std::size_t> playerIndices;
    for (HouseClass* house : GetValidatedHouseArray())
    {
        if (!IsParticipatingHouse(house))
            continue;
        viewer::PlayerSummary player;
        player.mHouseIndex = house->ArrayIndex;
        player.mName = ReadHouseDisplayName(house);
        player.mFaction = ReadTypeDisplayName(house->Type);
        player.mColorRgb = ReadHouseColorRgb(house);
        player.mIsHuman = house->IsHumanPlayer;
        player.mDefeated = house->Defeated;
        // 此 const 查询包含可用矿石价值；直接读取 Balance 会少算可花费资金。
        player.mCredits = house->Available_Money();
        player.mPowerOutput = house->PowerOutput;
        player.mPowerDrain = house->PowerDrain;
        playerIndices.emplace(house, snapshot.mPlayers.size());
        snapshot.mPlayers.push_back(std::move(player));
    }

    // 生产中的对象不算部队：各工厂的 Object 在统计部队总数时排除。
    std::unordered_set<const TechnoClass*> factoryObjects;
    std::unordered_map<FactoryClass*, std::string> factoryLabels;
    const auto& buildings = BuildingClass::Array;
    if (buildings.Count >= 0 && buildings.Count <= 100000 &&
        (buildings.Count == 0 || buildings.Items))
    {
        for (int index = 0; index < buildings.Count; ++index)
        {
            const auto* building = buildings.Items[index];
            if (building && building->Factory && building->Type)
                factoryLabels.try_emplace(building->Factory, ReadTypeDisplayName(building->Type));
        }
    }

    const auto& factories = FactoryClass::Array;
    if (factories.Count >= 0 && factories.Count <= 100000 &&
        (factories.Count == 0 || factories.Items))
    {
        for (auto& player : snapshot.mPlayers)
            player.mHasProductionData = true;
        for (int index = 0; index < factories.Count; ++index)
        {
            const auto* factory = factories.Items[index];
            if (!factory || !factory->Object)
                continue;
            factoryObjects.insert(factory->Object);
            const auto playerIndex = playerIndices.find(factory->Owner);
            if (playerIndex == playerIndices.end())
                continue;
            viewer::ProductionLine line;
            line.mId = factory->UniqueID;
            const auto label = factoryLabels.find(const_cast<FactoryClass*>(factory));
            line.mLabel = label == factoryLabels.end() ? "生产线" : label->second;
            const auto* productType = factory->Object->GetTechnoType();
            line.mProductName = ReadTypeDisplayName(productType);
            line.mIcon = mImpl->mCameos.Resolve(productType);
            line.mProgress = std::clamp(static_cast<float>(factory->Production.Value) / 54.0f, 0.0f, 1.0f);
            line.mReady = factory->Production.Value >= 54;
            line.mOnHold = !line.mReady && (factory->OnHold || factory->IsSuspended);
            line.mQueuedCount = static_cast<std::uint32_t>(std::max(0, factory->QueuedObjects.Count));
            snapshot.mPlayers[playerIndex->second].mProductionLines.push_back(std::move(line));
        }
    }

    std::vector<std::map<std::pair<int, std::string>, viewer::UnitSummary>> summaries(snapshot.mPlayers.size());
    std::unordered_set<std::uint32_t> recordedIds;
    if (recordedGroups)
        for (const auto& member : recordedGroups->mMembers)
            recordedIds.insert(member.mObjectId);
    std::unordered_map<std::uint32_t, viewer::UnitSummary> resolvedGroupMembers;
    const auto describeUnit = [this](TechnoClass* techno, const TechnoTypeClass* type)
    {
        viewer::UnitSummary unit;
        unit.mTypeId.assign(type->ID, strnlen_s(type->ID, sizeof(type->ID)));
        unit.mName = ReadTypeDisplayName(type);
        unit.mCategory = Category(techno->WhatAmI());
        unit.mIcon = mImpl->mCameos.Resolve(type);
        unit.mCount = 1;
        return unit;
    };
    std::vector<std::uint32_t> unitTotals(snapshot.mPlayers.size(), 0);
    const auto& technos = TechnoClass::Array;
    if (technos.Count >= 0 && technos.Count <= 100000 &&
        (technos.Count == 0 || technos.Items))
    {
        for (auto& player : snapshot.mPlayers)
            player.mHasUnitData = true;
        for (int index = 0; index < technos.Count; ++index)
        {
            auto* techno = technos.Items[index];
            if (!techno || !techno->IsAlive || techno->Health <= 0)
                continue;
            const auto* type = techno->GetTechnoType();
            if (!type)
                continue;
            // 部队总数：步兵、载具、飞机，含矿车，不含建筑；在场上，或被运输工具载着（Transporter 非空）；
            // 排除生产中的对象。这与下面按类型汇总的“场上单位”过滤不同，所以在它之前单独计数。
            // 先做便宜的类别与位置判断，再查玩家表与生产对象集，避免对建筑和未部署对象做哈希查找。
            const auto kind = Category(techno->WhatAmI());
            const bool isMobile = kind == viewer::UnitCategory::Infantry ||
                kind == viewer::UnitCategory::Vehicle || kind == viewer::UnitCategory::Aircraft;
            if (isMobile && ((techno->IsOnMap && !techno->InLimbo) || techno->Transporter != nullptr))
            {
                const auto owner = playerIndices.find(techno->Owner);
                if (owner != playerIndices.end() && !factoryObjects.contains(techno))
                    ++unitTotals[owner->second];
            }
            // 历史成员按现有 UniqueID 匹配；不读 Group、不调用会维护 TargetIndex 的 As_Techno。
            // 运输或部署中的存活成员仍可匹配，普通场上单位统计继续保留其原有过滤。
            if (recordedIds.contains(techno->UniqueID))
                resolvedGroupMembers.try_emplace(techno->UniqueID, describeUnit(techno, type));
            if (!techno->IsOnMap || techno->InLimbo)
                continue;
            const auto playerIndex = playerIndices.find(techno->Owner);
            if (playerIndex == playerIndices.end())
                continue;
            const auto category = Category(techno->WhatAmI());
            const std::string id(type->ID, strnlen_s(type->ID, sizeof(type->ID)));
            auto& summary = summaries[playerIndex->second][{static_cast<int>(category), id}];
            if (summary.mCount == 0)
            {
                summary = describeUnit(techno, type);
                summary.mCount = 0;
            }
            ++summary.mCount;
        }
    }
    for (std::size_t index = 0; index < summaries.size(); ++index)
    {
        for (auto& [key, summary] : summaries[index])
            snapshot.mPlayers[index].mUnits.push_back(std::move(summary));
        if (snapshot.mPlayers[index].mHasUnitData)
            snapshot.mPlayers[index].mUnitTotal = unitTotals[index];
    }
    ProjectRecordedControlGroups(recordedGroups, resolvedGroupMembers, snapshot);
}
}
