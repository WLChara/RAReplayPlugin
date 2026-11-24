#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>

namespace rareplay::stats
{
/**
 * EventClass 字节布局中与操作计数有关的位置：Type 在 +0，HouseIndex（有符号字节，-1 表示无效）在 +2，
 * 联合体载荷（DataBuffer）从 +7 开始，整条事件 111 字节。game_api 以 YRpp 声明做 static_assert 核对。
 */
constexpr std::size_t EVENT_TYPE_OFFSET = 0;
constexpr std::size_t EVENT_HOUSE_INDEX_OFFSET = 2;
constexpr std::size_t EVENT_PAYLOAD_OFFSET = 7;
constexpr std::size_t NATIVE_EVENT_BYTES = 111;
constexpr std::size_t EVENT_PAYLOAD_BYTES = NATIVE_EVENT_BYTES - EVENT_PAYLOAD_OFFSET;

/**
 * 各事件载荷内的字段偏移，相对 EventClass::DataBuffer（即事件字节 +7）。合并键只取这里描述的有定义的字段，
 * 不取联合体里没有被构造函数写入的填充或未用字节（例如 MegaMission 的 _gap_，它不被原生构造函数初始化）。
 * game_api 的 StatsReader.cpp 以 YRpp EventClass 的联合体成员声明逐项 static_assert，并经 IDA 核对各事件构造函数
 * 与 handle 分支对这些偏移的读写；各事件取哪些字段见 OperationEvents.cpp 的归类表。
 */
namespace event_layout
{
/** TargetClass：4 字节 ID 加 1 字节 RTTI，packed。 */
constexpr std::size_t TARGET_BYTES = 5;

constexpr std::size_t MEGA_MISSION_WHOM = 0;
constexpr std::size_t MEGA_MISSION_MISSION = 5;
constexpr std::size_t MEGA_MISSION_TARGET = 7;
constexpr std::size_t MEGA_MISSION_DESTINATION = 12;
constexpr std::size_t MEGA_MISSION_FOLLOW = 17;
constexpr std::size_t MEGA_MISSION_PLANNING = 22;
constexpr std::size_t MEGA_MISSION_BYTES = 23;

constexpr std::size_t MEGA_MISSION_F_WHOM = 0;
constexpr std::size_t MEGA_MISSION_F_MISSION = 5;
constexpr std::size_t MEGA_MISSION_F_TARGET = 6;
constexpr std::size_t MEGA_MISSION_F_DESTINATION = 11;
constexpr std::size_t MEGA_MISSION_F_SPEED = 16;
constexpr std::size_t MEGA_MISSION_F_MAX_SPEED = 20;
constexpr std::size_t MEGA_MISSION_F_BYTES = 24;

/** Produce、Suspend、Abandon 共用：RTTIType、HeapID、IsNaval 各 4 字节；AbandonAll 与 Abandon 共用同一处理分支。 */
constexpr std::size_t PRODUCTION_RTTI = 0;
constexpr std::size_t PRODUCTION_HEAP_ID = 4;
constexpr std::size_t PRODUCTION_IS_NAVAL = 8;
constexpr std::size_t PRODUCTION_BYTES = 12;

constexpr std::size_t PLACE_RTTI = 0;
constexpr std::size_t PLACE_HEAP_ID = 4;
constexpr std::size_t PLACE_IS_NAVAL = 8;
constexpr std::size_t PLACE_LOCATION = 12;
constexpr std::size_t PLACE_BYTES = 16;

constexpr std::size_t SPECIAL_PLACE_ID = 0;
constexpr std::size_t SPECIAL_PLACE_LOCATION = 4;
constexpr std::size_t SPECIAL_PLACE_BYTES = 8;

constexpr std::size_t SELL_CELL_LOCATION = 0;
constexpr std::size_t SELL_CELL_BYTES = 4;

constexpr std::size_t ALLY_HOUSE_ID = 0;
constexpr std::size_t ALLY_BYTES = 4;

constexpr std::size_t ARCHIVE_FIRST = 0;
constexpr std::size_t ARCHIVE_SECOND = 5;
constexpr std::size_t ARCHIVE_BYTES = 10;

constexpr std::size_t PLAN_CONNECT_FIRST = 0;
constexpr std::size_t PLAN_CONNECT_SECOND = 5;
constexpr std::size_t PLAN_CONNECT_BYTES = 10;

/**
 * Timing：RequestedFPS u16、MaxAhead u16、FrameSendRate u8，与 YRpp EventClass::Timing 一致。依据是处理分支的反汇编
 * （0x4C807D 把 [esi+7] 的 u16 原样写入 DesiredFrameRate），
 * game_api 的 StatsReader.cpp 对这三项与总长做 static_assert。只有 RequestedFPS 被时间表使用。
 */
constexpr std::size_t TIMING_REQUESTED_FPS = 0;
constexpr std::size_t TIMING_MAX_AHEAD = 2;
constexpr std::size_t TIMING_FRAME_SEND_RATE = 4;
constexpr std::size_t TIMING_BYTES = 5;
}

/**
 * 原生事件类型，数值与名称同 YRpp 的 EventType（0..46）。LAST_EVENT=47 只是哨兵。
 * game_api 逐项 static_assert 与 YRpp 一致。
 */
enum class NativeEventType : std::uint8_t
{
    Empty = 0, PowerOn = 1, PowerOff = 2, Ally = 3, MegaMission = 4, MegaMissionF = 5, Idle = 6, Scatter = 7,
    Destruct = 8, Deploy = 9, Detonate = 10, Place = 11, Options = 12, GameSpeed = 13, Produce = 14,
    Suspend = 15, Abandon = 16, Primary = 17, SpecialPlace = 18, Exit = 19, Animation = 20, Repair = 21,
    Sell = 22, SellCell = 23, Special = 24, FrameSync = 25, Message = 26, ResponseTime = 27, FrameInfo = 28,
    SaveGame = 29, Archive = 30, AddPlayer = 31, Timing = 32, ProcessTime = 33, PageUser = 34,
    RemovePlayer = 35, LatencyFudge = 36, MegaFrameInfo = 37, PacketTiming = 38, AboutToExit = 39,
    FallbackHost = 40, AddressChange = 41, PlanConnect = 42, PlanCommit = 43, PlanNodeDelete = 44,
    AllCheer = 45, AbandonAll = 46
};

constexpr std::size_t NATIVE_EVENT_TYPE_COUNT = 47;

/** 载荷中一段字节，偏移相对 EventClass::DataBuffer。 */
struct PayloadSpan
{
    std::uint8_t mOffset = 0;
    std::uint8_t mLength = 0;
};

/**
 * 同一帧内合并同类命令时，合并键取哪些载荷字节。一次群体命令会为每个被选单位各产生一条事件，
 * 它们除执行者（Whom）以外的参数完全相同，合并后只算一次操作。
 */
enum class MergeBasis
{
    /** 没有有意义的载荷（处理分支不读取载荷）：键就是类型。 */
    NoPayload,
    /** 载荷只有执行者（Whom）：键就是类型，同帧同玩家的该类事件只算一次。 */
    ExecutorOnly,
    /** 载荷含执行者与命令参数：键为类型加除执行者以外、有定义的全部载荷字段。 */
    ExcludingExecutor,
    /** 执行者字段的语义无法确定，或每个对象是独立操作：键为类型加全部有定义的载荷字段。 */
    AllPayload
};

struct EventClassification
{
    NativeEventType mType;
    const char* mName;
    /** 是否计入“操作总量”。 */
    bool mIsOperation;
    /** 归类理由：简述为何计入或不计入操作总量，不得为空。 */
    const char* mReason;
    /** 以下三项只对计入类事件有意义。 */
    MergeBasis mMergeBasis = MergeBasis::NoPayload;
    /** 合并键取的载荷字节段，只有前 mKeySpanCount 段有效；按偏移升序且互不重叠。 */
    std::array<PayloadSpan, 2> mKeySpans{};
    std::uint8_t mKeySpanCount = 0;
};

/** 全部 47 个类型，按类型值升序、下标等于类型值。 */
const std::array<EventClassification, NATIVE_EVENT_TYPE_COUNT>& GetEventClassifications() noexcept;

/** 事件类型是否计入操作总量；超出已知范围的类型值不计入。 */
bool IsOperationEvent(std::uint8_t type) noexcept;

/** 游戏时间不足它时不给 APM，避免开局几秒的少量命令得出夸张数值。 */
constexpr std::int64_t APM_MIN_GAME_TIME_MS = 10'000;

/** 操作数 ÷ 游戏分钟数；游戏时间不足 APM_MIN_GAME_TIME_MS 时为空。 */
std::optional<float> ComputeApm(std::uint32_t operationCount, std::int64_t gameTimeMs) noexcept;

/**
 * 分房屋统计操作，只在游戏线程使用。
 *
 * 计数单位是“操作”：同一帧、同一玩家、同一事件类型、除执行者以外有定义的载荷字段完全相同的计入类事件，
 * 只算一次（合并键见 EventClassification）。不同帧之间不合并；不同玩家互不影响。
 * 同时保留合并前的原始事件条数，仅供参考。
 */
class OperationCounter final
{
public:
    /**
     * 登记回放中某一帧执行的一条原生事件（完整 NATIVE_EVENT_BYTES 字节）。非计入类事件、房屋下标越界
     * （含 -1）或长度不足的事件被忽略。返回这条事件是否算作新的一次操作（false 也可能是被合并）。
     * 帧号变化时清空上一帧的合并状态，所以同一帧的事件必须连续登记。
     */
    bool Count(std::uint32_t frameNumber, std::span<const std::uint8_t> event);
    std::uint32_t GetOperationCount(int houseIndex) const noexcept;
    /** 合并前计入类事件的条数。 */
    std::uint32_t GetRawEventCount(int houseIndex) const noexcept;
    void Reset() noexcept;

    /** HouseIndex 是有符号字节，计数表按其正值范围分配。 */
    static constexpr int MAX_HOUSES = 128;

private:
    std::array<std::uint32_t, MAX_HOUSES> mOperations{};
    std::array<std::uint32_t, MAX_HOUSES> mRawEvents{};
    std::optional<std::uint32_t> mFrame;
    std::unordered_set<std::string> mSeenKeys;
};
}
