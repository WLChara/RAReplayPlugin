#include "../src/game_api/StateDigest.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace rareplay::game_api;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::uint32_t HashText(std::string_view text)
{
    Fnv1a32 hash;
    for (const char value : text)
        hash.AddByte(static_cast<std::uint8_t>(value));
    return hash.GetValue();
}

// 公开的 32 位 FNV-1a 测试向量。
void TestFnvVectors()
{
    Require(Fnv1a32{}.GetValue() == 0x811C9DC5u, "empty input is the offset basis");
    Require(HashText("a") == 0xE40C292Cu, "FNV-1a of \"a\"");
    Require(HashText("foobar") == 0xBF9CF968u, "FNV-1a of \"foobar\"");
}

void TestIntegersAreLittleEndianBytes()
{
    Fnv1a32 byWord;
    byWord.AddU32(0x12345678u);
    Fnv1a32 byBytes;
    const std::array<std::uint8_t, 4> bytes{0x78, 0x56, 0x34, 0x12};
    byBytes.AddBytes(bytes);
    Require(byWord.GetValue() == byBytes.GetValue(), "AddU32 adds four little-endian bytes");

    Fnv1a32 negative;
    negative.AddI32(-1);
    Fnv1a32 allOnes;
    allOnes.AddU32(0xFFFFFFFFu);
    Require(negative.GetValue() == allOnes.GetValue(), "AddI32 adds the two's complement bits");
}

void TestRandomTableHash()
{
    std::array<std::uint32_t, 250> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index)
        table[index] = index * 0x9E3779B9u;
    Require(HashRandomTable(table) == 0x11705379u, "fixed random table hashes to a fixed value");
    table[249] ^= 1u;
    Require(HashRandomTable(table) != 0x11705379u, "the last table entry takes part in the hash");
}

HouseDigestFields MakeHouse(std::int32_t index, std::int32_t type, std::int32_t color, std::int32_t balance,
    std::int32_t output, std::int32_t drain, std::uint32_t allies)
{
    HouseDigestFields house;
    house.mArrayIndex = index;
    house.mTypeIndex = type;
    house.mColorSchemeIndex = color;
    house.mBalance = balance;
    house.mPowerOutput = output;
    house.mPowerDrain = drain;
    house.mAllyMask = allies;
    return house;
}

void TestHouseHash()
{
    const auto first = MakeHouse(0, 3, 5, 10000, 200, 150, 0x1);
    const auto second = MakeHouse(1, 7, 9, -25, 0, 50, 0x2);
    Fnv1a32 hash;
    AddHouseDigest(hash, first);
    AddHouseDigest(hash, second);
    Require(hash.GetValue() == 0xF9FC4D1Eu, "fixed houses hash to a fixed value");

    Fnv1a32 swapped;
    AddHouseDigest(swapped, second);
    AddHouseDigest(swapped, first);
    Require(swapped.GetValue() == 0x6706CA4Au, "house order is part of the hash");

    // 每个字段都参与：逐一改动一个字段，哈希都会变化。
    const std::vector<HouseDigestFields> changed = {
        MakeHouse(2, 3, 5, 10000, 200, 150, 0x1), MakeHouse(0, 4, 5, 10000, 200, 150, 0x1),
        MakeHouse(0, 3, 6, 10000, 200, 150, 0x1), MakeHouse(0, 3, 5, 10001, 200, 150, 0x1),
        MakeHouse(0, 3, 5, 10000, 201, 150, 0x1), MakeHouse(0, 3, 5, 10000, 200, 151, 0x1),
        MakeHouse(0, 3, 5, 10000, 200, 150, 0x3)};
    Fnv1a32 reference;
    AddHouseDigest(reference, first);
    for (const auto& house : changed)
    {
        Fnv1a32 other;
        AddHouseDigest(other, house);
        Require(other.GetValue() != reference.GetValue(), "every house field takes part in the hash");
    }
}

ObjectDigestFields MakeObject(std::uint32_t id, std::int32_t type, std::int32_t x, std::int32_t y, std::int32_t z,
    std::int32_t health, std::int32_t mission, std::int32_t owner)
{
    ObjectDigestFields object;
    object.mUniqueId = id;
    object.mTypeIndex = type;
    object.mX = x;
    object.mY = y;
    object.mZ = z;
    object.mHealth = health;
    object.mMission = mission;
    object.mOwnerIndex = owner;
    return object;
}

void TestObjectHash()
{
    Fnv1a32 missing;
    AddObjectDigest(missing, ObjectDigestFields{});
    Require(missing.GetValue() == 0x97927A59u, "a missing entry hashes as the default fields");

    Fnv1a32 hash;
    AddObjectDigest(hash, MakeObject(1000123, 12, 25600, 51200, 0, 400, 5, 0));
    AddObjectDigest(hash, ObjectDigestFields{});
    AddObjectDigest(hash, MakeObject(1000456, 2, 128, 256, 104, 125, 1, 1));
    Require(hash.GetValue() == 0xD98C3BF2u, "fixed objects hash to a fixed value");

    const auto base = MakeObject(1000123, 12, 25600, 51200, 0, 400, 5, 0);
    const std::vector<ObjectDigestFields> changed = {MakeObject(1000124, 12, 25600, 51200, 0, 400, 5, 0),
        MakeObject(1000123, 13, 25600, 51200, 0, 400, 5, 0), MakeObject(1000123, 12, 25601, 51200, 0, 400, 5, 0),
        MakeObject(1000123, 12, 25600, 51201, 0, 400, 5, 0), MakeObject(1000123, 12, 25600, 51200, 1, 400, 5, 0),
        MakeObject(1000123, 12, 25600, 51200, 0, 399, 5, 0), MakeObject(1000123, 12, 25600, 51200, 0, 400, 6, 0),
        MakeObject(1000123, 12, 25600, 51200, 0, 400, 5, 1)};
    Fnv1a32 reference;
    AddObjectDigest(reference, base);
    for (const auto& object : changed)
    {
        Fnv1a32 other;
        AddObjectDigest(other, object);
        Require(other.GetValue() != reference.GetValue(), "every object field takes part in the hash");
    }
}

void TestFormat()
{
    StateDigest digest;
    digest.mFrame = 12000;
    digest.mRandomNext1 = 17;
    digest.mRandomNext2 = 241;
    digest.mRandomTableHash = 0x11705379u;
    digest.mRandomIsConstant = 0;
    digest.mUniqueIdCounter = 1012345;
    digest.mHouseCount = 4;
    digest.mHouseHash = 0xF9FC4D1Eu;
    digest.mObjects[static_cast<std::size_t>(DigestObjectKind::Unit)] = {37, 0xD98C3BF2u};
    digest.mObjects[static_cast<std::size_t>(DigestObjectKind::Infantry)] = {52, 0x0000ABCDu};
    digest.mObjects[static_cast<std::size_t>(DigestObjectKind::Building)] = {41, 0x97927A59u};
    // 飞机数组为空：哈希保持偏移基。
    digest.mObjects[static_cast<std::size_t>(DigestObjectKind::Aircraft)] = {0, Fnv1a32::OFFSET_BASIS};
    digest.mGameMode = 5;
    digest.mMaxAhead = 24;
    digest.mFrameSendRate = 4;
    digest.mDesiredFrameRate = 60;
    digest.mTimingTimeStamp = 0;
    digest.mFrameSyncTargetFrame = -1;
    digest.mDoListCount = 3;
    digest.mSpecialFlags = 0x00001040u;
    digest.mVariablesChanged = 1;
    const std::string expected =
        "Replay state digest: frame=12000 rng=17/241/11705379/0 uid=1012345 houses=4/f9fc4d1e "
        "objects=37,52,41,0/d98c3bf2,0000abcd,97927a59,811c9dc5 session=5/24/4/60/0/-1/3 flags=00001040/1";
    const std::string actual = FormatStateDigestLog(digest);
    if (actual != expected)
    {
        std::cerr << "actual:   " << actual << "\nexpected: " << expected << '\n';
        throw std::runtime_error("state digest log line has the fixed layout");
    }

    const std::string defaults = FormatStateDigestLog(StateDigest{});
    Require(defaults ==
            "Replay state digest: frame=0 rng=0/0/811c9dc5/0 uid=0 houses=0/811c9dc5 "
            "objects=0,0,0,0/811c9dc5,811c9dc5,811c9dc5,811c9dc5 session=0/0/0/0/0/0/0 flags=00000000/0",
        "a default digest formats every field");
}
}

int main()
{
    try
    {
        TestFnvVectors();
        TestIntegersAreLittleEndianBytes();
        TestRandomTableHash();
        TestHouseHash();
        TestObjectHash();
        TestFormat();
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
