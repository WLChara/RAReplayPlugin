#include "StateDigest.h"

#include <bit>
#include <cstdio>

namespace rareplay::game_api
{
void Fnv1a32::AddByte(std::uint8_t value) noexcept
{
    mValue = (mValue ^ value) * PRIME;
}

void Fnv1a32::AddBytes(std::span<const std::uint8_t> bytes) noexcept
{
    for (const std::uint8_t value : bytes)
        AddByte(value);
}

void Fnv1a32::AddU32(std::uint32_t value) noexcept
{
    for (int shift = 0; shift < 32; shift += 8)
        AddByte(static_cast<std::uint8_t>(value >> shift));
}

void Fnv1a32::AddI32(std::int32_t value) noexcept
{
    AddU32(std::bit_cast<std::uint32_t>(value));
}

std::uint32_t HashRandomTable(std::span<const std::uint32_t> table) noexcept
{
    Fnv1a32 hash;
    for (const std::uint32_t value : table)
        hash.AddU32(value);
    return hash.GetValue();
}

std::uint32_t HashVariableValues(std::span<const std::uint8_t> values) noexcept
{
    Fnv1a32 hash;
    hash.AddBytes(values);
    return hash.GetValue();
}

void AddHouseDigest(Fnv1a32& hash, const HouseDigestFields& house) noexcept
{
    hash.AddI32(house.mArrayIndex);
    hash.AddI32(house.mTypeIndex);
    hash.AddI32(house.mColorSchemeIndex);
    hash.AddI32(house.mBalance);
    hash.AddI32(house.mPowerOutput);
    hash.AddI32(house.mPowerDrain);
    hash.AddU32(house.mAllyMask);
}

void AddObjectDigest(Fnv1a32& hash, const ObjectDigestFields& object) noexcept
{
    hash.AddU32(object.mUniqueId);
    hash.AddI32(object.mTypeIndex);
    hash.AddI32(object.mX);
    hash.AddI32(object.mY);
    hash.AddI32(object.mZ);
    hash.AddI32(object.mHealth);
    hash.AddI32(object.mMission);
    hash.AddI32(object.mOwnerIndex);
}

std::string FormatStateDigestLog(const StateDigest& digest)
{
    const auto& objects = digest.mObjects;
    const auto& units = objects[static_cast<std::size_t>(DigestObjectKind::Unit)];
    const auto& infantry = objects[static_cast<std::size_t>(DigestObjectKind::Infantry)];
    const auto& buildings = objects[static_cast<std::size_t>(DigestObjectKind::Building)];
    const auto& aircraft = objects[static_cast<std::size_t>(DigestObjectKind::Aircraft)];
    char line[512]{};
    std::snprintf(line, sizeof(line),
        "Replay state digest: frame=%u rng=%d/%d/%08x/%u uid=%u houses=%u/%08x objects=%u,%u,%u,%u/%08x,%08x,%08x,%08x "
        "session=%u/%d/%d/%d/%d/%d/%d flags=%08x/%u vars=%08x/%08x",
        digest.mFrame, digest.mRandomNext1, digest.mRandomNext2, digest.mRandomTableHash,
        static_cast<unsigned>(digest.mRandomIsConstant), digest.mUniqueIdCounter, digest.mHouseCount, digest.mHouseHash,
        units.mCount, infantry.mCount, buildings.mCount, aircraft.mCount,
        units.mHash, infantry.mHash, buildings.mHash, aircraft.mHash,
        digest.mGameMode, digest.mMaxAhead, digest.mFrameSendRate, digest.mDesiredFrameRate, digest.mTimingTimeStamp,
        digest.mFrameSyncTargetFrame, digest.mDoListCount, digest.mSpecialFlags,
        static_cast<unsigned>(digest.mVariablesChanged), digest.mGlobalVariablesHash, digest.mLocalVariablesHash);
    return line;
}
}
