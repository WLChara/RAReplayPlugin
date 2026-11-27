#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace rareplay::game_api
{
/**
 * 32 位 FNV-1a（偏移基 0x811C9DC5，素数 0x01000193）。整数一律按 4 字节小端逐字节加入，
 * 摘要的每一项因此只取决于字段值与加入顺序，与宿主字节序无关。不含 YRpp，可离线测试。
 */
class Fnv1a32 final
{
public:
    static constexpr std::uint32_t OFFSET_BASIS = 0x811C9DC5u;
    static constexpr std::uint32_t PRIME = 0x01000193u;

    void AddByte(std::uint8_t value) noexcept;
    void AddBytes(std::span<const std::uint8_t> bytes) noexcept;
    void AddU32(std::uint32_t value) noexcept;
    void AddI32(std::int32_t value) noexcept;
    std::uint32_t GetValue() const noexcept { return mValue; }

private:
    std::uint32_t mValue = OFFSET_BASIS;
};

/** 场景随机数 250 项表的哈希：逐项按 4 字节小端加入 FNV-1a。 */
std::uint32_t HashRandomTable(std::span<const std::uint32_t> table) noexcept;

/** 变量值数组（全局 50 项或局部 100 项，每项一个字节）的哈希：按数组顺序逐字节加入 FNV-1a。 */
std::uint32_t HashVariableValues(std::span<const std::uint8_t> values) noexcept;

/**
 * 每个房屋参与摘要的字段，按声明顺序加入哈希。默认值表示数组中的空指针项，
 * 真实房屋的 ArrayIndex 不为 −1，两者可以区分。
 */
struct HouseDigestFields
{
    std::int32_t mArrayIndex = -1;
    /** 国家（HouseTypeClass）的 ArrayIndex；没有类型时为 −1。 */
    std::int32_t mTypeIndex = -1;
    std::int32_t mColorSchemeIndex = -1;
    /** 现金余额字段原值，不含矿石存量。 */
    std::int32_t mBalance = 0;
    std::int32_t mPowerOutput = 0;
    std::int32_t mPowerDrain = 0;
    /** 盟友位掩码，每个房屋 ArrayIndex 一位。 */
    std::uint32_t mAllyMask = 0;
};

void AddHouseDigest(Fnv1a32& hash, const HouseDigestFields& house) noexcept;

/**
 * 每个对象参与摘要的字段，按声明顺序加入哈希。默认值表示数组中的空指针项；
 * 真实对象的 UniqueID 由场景计数器递增分配（开局时计数器置为 1000000），不会为 0。
 */
struct ObjectDigestFields
{
    std::uint32_t mUniqueId = 0;
    /** 对象类型的 ArrayIndex；没有类型时为 −1。 */
    std::int32_t mTypeIndex = -1;
    /** 绝对坐标（lepton）。 */
    std::int32_t mX = 0;
    std::int32_t mY = 0;
    std::int32_t mZ = 0;
    std::int32_t mHealth = 0;
    std::int32_t mMission = -1;
    /** 所属房屋的 ArrayIndex；没有所属房屋时为 −1。 */
    std::int32_t mOwnerIndex = -1;
};

void AddObjectDigest(Fnv1a32& hash, const ObjectDigestFields& object) noexcept;

/** 一个对象数组的元素数与按数组顺序逐项（含空指针项）求出的哈希。 */
struct ObjectArrayDigest
{
    std::uint32_t mCount = 0;
    std::uint32_t mHash = Fnv1a32::OFFSET_BASIS;
};

/** 摘要中的四个对象数组，顺序固定。 */
enum class DigestObjectKind : std::size_t { Unit, Infantry, Building, Aircraft, Count };

/**
 * 某一帧完成时世界的只读状态摘要。用于比对两次播放（例如新进程跳到第 T 帧与同进程重开后跳到第 T 帧）
 * 的世界是否一致；单机录像没有录制 CRC，这是主要判据。只含值，由 game_api 读取后交给 plugin 写日志。
 */
struct StateDigest
{
    std::uint32_t mFrame = 0;
    // 场景随机数（ScenarioClass::Random）：两个表下标、250 项表的哈希，以及“恒定”标志（为真时取数返回 0 且不推进）。
    std::int32_t mRandomNext1 = 0;
    std::int32_t mRandomNext2 = 0;
    std::uint32_t mRandomTableHash = Fnv1a32::OFFSET_BASIS;
    std::uint8_t mRandomIsConstant = 0;
    /** 场景的对象 ID 计数器：最后一次分配出去的 ID。 */
    std::uint32_t mUniqueIdCounter = 0;
    std::uint32_t mHouseCount = 0;
    std::uint32_t mHouseHash = Fnv1a32::OFFSET_BASIS;
    std::array<ObjectArrayDigest, static_cast<std::size_t>(DigestObjectKind::Count)> mObjects{};
    // 会话与节奏。
    std::uint32_t mGameMode = 0;
    std::int32_t mMaxAhead = 0;
    std::int32_t mFrameSendRate = 0;
    std::int32_t mDesiredFrameRate = 0;
    std::int32_t mTimingTimeStamp = 0;
    std::int32_t mFrameSyncTargetFrame = 0;
    std::int32_t mDoListCount = 0;
    // 场景标志：特殊规则位（含战争迷雾等）与全局、局部变量的变化标志。
    std::uint32_t mSpecialFlags = 0;
    std::uint8_t mVariablesChanged = 0;
    // 全局变量值（50 项）与局部变量值（100 项）的哈希（HashVariableValues）；进程内重开须在载入前清零它们。
    std::uint32_t mGlobalVariablesHash = Fnv1a32::OFFSET_BASIS;
    std::uint32_t mLocalVariablesHash = Fnv1a32::OFFSET_BASIS;
};

/**
 * 一行英文技术日志，字段顺序与排版固定，便于用脚本逐行比对：
 * Replay state digest: frame=N rng=Next1/Next2/表哈希/恒定 uid=U houses=数量/哈希
 * objects=单位,步兵,建筑,飞机/四个哈希 session=模式/MaxAhead/FrameSendRate/DesiredFrameRate/时间戳/同步目标帧/DoList
 * flags=特殊规则位/变量变化 vars=全局变量值哈希/局部变量值哈希。哈希与特殊规则位为 8 位小写十六进制，其余为十进制。
 */
std::string FormatStateDigestLog(const StateDigest& digest);
}
