#include "OperationEvents.h"

namespace rareplay::stats
{
namespace
{
using T = NativeEventType;
using namespace event_layout;

constexpr PayloadSpan Span(std::size_t offset, std::size_t length)
{
    return {static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(length)};
}

constexpr EventClassification Skip(T type, const char* name, const char* reason)
{
    return {type, name, false, reason};
}

constexpr EventClassification Op(T type, const char* name, const char* reason, MergeBasis basis,
                                 PayloadSpan first = {}, PayloadSpan second = {})
{
    EventClassification result{type, name, true, reason, basis, {first, second}, 0};
    result.mKeySpanCount = static_cast<std::uint8_t>((first.mLength > 0 ? 1 : 0) + (second.mLength > 0 ? 1 : 0));
    return result;
}

// 归类依据：ClientNetworkHandler::handle（0x4C6CB0）的跳转表 0x4C8114 与各分支行为、handleAny（0x64C380）
// 对 Exit/Options/PageUser 的本地路由；合并键依据各事件构造函数（0x4C65E0、0x4C6780、0x4C6860、0x4C68E0、
// 0x4C6970、0x4C6AE0、0x4C6B60）与 handle 分支对载荷的读写，与 YRpp EventClass 的联合体声明一致。
constexpr std::array<EventClassification, NATIVE_EVENT_TYPE_COUNT> CLASSIFICATIONS = {{
    Skip(T::Empty, "Empty", "空事件，handle 无分支"),
    // 建筑电源的执行者字段语义不明确（每个建筑是独立的切换），不猜：按全部载荷合并。
    Op(T::PowerOn, "PowerOn", "玩家开启建筑电源", MergeBasis::AllPayload, Span(0, TARGET_BYTES)),
    Op(T::PowerOff, "PowerOff", "玩家关闭建筑电源", MergeBasis::AllPayload, Span(0, TARGET_BYTES)),
    Op(T::Ally, "Ally", "玩家变更结盟", MergeBasis::AllPayload, Span(ALLY_HOUSE_ID, ALLY_BYTES)),
    // 键为任务、目标、目的地、跟随对象、是否规划事件；排除执行者 Whom 与未初始化的填充字节 _gap_（载荷 +6）。
    Op(T::MegaMission, "MegaMission", "单位指令，每个被选单位一条", MergeBasis::ExcludingExecutor,
       Span(MEGA_MISSION_MISSION, 1), Span(MEGA_MISSION_TARGET, MEGA_MISSION_PLANNING + 1 - MEGA_MISSION_TARGET)),
    // 键为任务、目标、目的地、速度、最大速度；排除执行者 Whom。
    Op(T::MegaMissionF, "MegaMissionF", "单位指令（带速度），每个被选单位一条", MergeBasis::ExcludingExecutor,
       Span(MEGA_MISSION_F_MISSION, MEGA_MISSION_F_BYTES - MEGA_MISSION_F_MISSION)),
    Op(T::Idle, "Idle", "单位停止或待命", MergeBasis::ExecutorOnly),
    Op(T::Scatter, "Scatter", "单位散开", MergeBasis::ExecutorOnly),
    Skip(T::Destruct, "Destruct", "投降：整户进入结束计时（0x4C7BDF），不是对局内操作"),
    Op(T::Deploy, "Deploy", "单位部署", MergeBasis::ExecutorOnly),
    Op(T::Detonate, "Detonate", "引爆已安放的炸弹", MergeBasis::ExecutorOnly),
    Op(T::Place, "Place", "放置建筑或防御", MergeBasis::AllPayload, Span(0, PLACE_BYTES)),
    Skip(T::Options, "Options", "只请求本地选项菜单（0x4C7939），不改变世界，回放注入时也被跳过"),
    Skip(T::GameSpeed, "GameSpeed", "速度设置，不是对局内指令"),
    Op(T::Produce, "Produce", "开始或排队生产", MergeBasis::AllPayload, Span(0, PRODUCTION_BYTES)),
    Op(T::Suspend, "Suspend", "暂停或恢复生产", MergeBasis::AllPayload, Span(0, PRODUCTION_BYTES)),
    Op(T::Abandon, "Abandon", "取消生产", MergeBasis::AllPayload, Span(0, PRODUCTION_BYTES)),
    // 设为主工厂的目标建筑是被点击的那一座，每个建筑是独立操作，不猜：按全部载荷合并。
    Op(T::Primary, "Primary", "设为主工厂", MergeBasis::AllPayload, Span(0, TARGET_BYTES)),
    Op(T::SpecialPlace, "SpecialPlace", "释放超级武器", MergeBasis::AllPayload, Span(0, SPECIAL_PLACE_BYTES)),
    Skip(T::Exit, "Exit", "玩家离开，会话事件"),
    Skip(T::Animation, "Animation", "点击的视觉反馈动画，伴随真实指令，不单独计"),
    // 修理与出售的 Whom 是被点击的建筑，每个建筑是独立操作：按全部载荷合并。
    Op(T::Repair, "Repair", "切换建筑修理", MergeBasis::AllPayload, Span(0, TARGET_BYTES)),
    Op(T::Sell, "Sell", "出售建筑", MergeBasis::AllPayload, Span(0, TARGET_BYTES)),
    Op(T::SellCell, "SellCell", "出售围墙", MergeBasis::AllPayload, Span(0, SELL_CELL_BYTES)),
    Skip(T::Special, "Special", "修改场景特殊选项并打印提示，属设置变更"),
    Skip(T::FrameSync, "FrameSync", "联机帧同步，handle 无分支"),
    Skip(T::Message, "Message", "聊天消息，handle 无分支"),
    Skip(T::ResponseTime, "ResponseTime", "联机响应时间，只写 SessionClass.MaxAhead"),
    Skip(T::FrameInfo, "FrameInfo", "联机逐帧 CRC 同步，handleAny 单独处理"),
    Skip(T::SaveGame, "SaveGame", "存档请求，不改变模拟"),
    // 归档目标事件带两个目标，哪个是执行者无法确定，不猜：按全部载荷合并。
    Op(T::Archive, "Archive", "设置单位的集结或归档目标", MergeBasis::AllPayload, Span(0, ARCHIVE_BYTES)),
    Skip(T::AddPlayer, "AddPlayer", "会话事件，只释放载荷缓冲"),
    Skip(T::Timing, "Timing", "联机帧率协商，设置 DesiredFrameRate"),
    Skip(T::ProcessTime, "ProcessTime", "联机处理时间统计"),
    Skip(T::PageUser, "PageUser", "仅设置界面对话状态，handleAny 与 Options 同路由"),
    Skip(T::RemovePlayer, "RemovePlayer", "会话事件，移除掉线玩家"),
    Skip(T::LatencyFudge, "LatencyFudge", "联机延迟调节"),
    Skip(T::MegaFrameInfo, "MegaFrameInfo", "联机帧同步，handle 无分支"),
    Skip(T::PacketTiming, "PacketTiming", "联机包时序，handle 无分支"),
    Skip(T::AboutToExit, "AboutToExit", "会话事件，玩家即将退出"),
    Skip(T::FallbackHost, "FallbackHost", "联机主机迁移"),
    Skip(T::AddressChange, "AddressChange", "联机地址变更"),
    Op(T::PlanConnect, "PlanConnect", "规划模式：连接路径点", MergeBasis::AllPayload, Span(0, PLAN_CONNECT_BYTES)),
    Op(T::PlanCommit, "PlanCommit", "规划模式：执行路径", MergeBasis::NoPayload),
    Op(T::PlanNodeDelete, "PlanNodeDelete", "规划模式：删除路径点", MergeBasis::AllPayload, Span(0, TARGET_BYTES)),
    // 处理分支 0x4C6E2E 只用事件所属房屋调用 HouseClass::Cheer，不读载荷。
    Op(T::AllCheer, "AllCheer", "全体欢呼", MergeBasis::NoPayload),
    // 与 Abandon 共用处理分支 0x4C7165（先压入“全部”标志），载荷布局相同。
    Op(T::AbandonAll, "AbandonAll", "取消全部生产", MergeBasis::AllPayload, Span(0, PRODUCTION_BYTES)),
}};

constexpr bool CheckTable()
{
    for (std::size_t index = 0; index < CLASSIFICATIONS.size(); ++index)
    {
        const EventClassification& entry = CLASSIFICATIONS[index];
        if (static_cast<std::size_t>(entry.mType) != index)
            return false;
        std::size_t previousEnd = 0;
        for (std::size_t span = 0; span < entry.mKeySpanCount; ++span)
        {
            const PayloadSpan current = entry.mKeySpans[span];
            if (current.mLength == 0 || current.mOffset < previousEnd ||
                static_cast<std::size_t>(current.mOffset) + current.mLength > EVENT_PAYLOAD_BYTES)
                return false;
            previousEnd = static_cast<std::size_t>(current.mOffset) + current.mLength;
        }
        // 只有“按载荷合并”的类型有键字段，其余基础的键就是类型本身。
        const bool usesPayload = entry.mMergeBasis == MergeBasis::ExcludingExecutor ||
                                 entry.mMergeBasis == MergeBasis::AllPayload;
        if (entry.mIsOperation && (usesPayload != (entry.mKeySpanCount > 0)))
            return false;
        if (!entry.mIsOperation && entry.mKeySpanCount != 0)
            return false;
    }
    return true;
}
static_assert(CheckTable(), "事件归类表必须按类型值升序、无空洞，且键字段在载荷范围内、与合并基础一致");
}

const std::array<EventClassification, NATIVE_EVENT_TYPE_COUNT>& GetEventClassifications() noexcept
{
    return CLASSIFICATIONS;
}

bool IsOperationEvent(std::uint8_t type) noexcept
{
    return type < NATIVE_EVENT_TYPE_COUNT && CLASSIFICATIONS[type].mIsOperation;
}

std::optional<float> ComputeApm(std::uint32_t operationCount, std::int64_t gameTimeMs) noexcept
{
    if (gameTimeMs < APM_MIN_GAME_TIME_MS)
        return std::nullopt;
    return static_cast<float>(static_cast<double>(operationCount) * 60000.0 / static_cast<double>(gameTimeMs));
}

bool OperationCounter::Count(std::uint32_t frameNumber, std::span<const std::uint8_t> event)
{
    if (event.size() < NATIVE_EVENT_BYTES)
        return false;
    const std::uint8_t type = event[EVENT_TYPE_OFFSET];
    const int houseIndex = static_cast<std::int8_t>(event[EVENT_HOUSE_INDEX_OFFSET]);
    if (houseIndex < 0 || houseIndex >= MAX_HOUSES || !IsOperationEvent(type))
        return false;
    ++mRawEvents[static_cast<std::size_t>(houseIndex)];

    // 合并只发生在同一帧内：帧号变化就丢弃上一帧的键。
    if (!mFrame || *mFrame != frameNumber)
    {
        mSeenKeys.clear();
        mFrame = frameNumber;
    }
    const EventClassification& rule = CLASSIFICATIONS[type];
    std::string key;
    key.push_back(static_cast<char>(houseIndex));
    key.push_back(static_cast<char>(type));
    for (std::size_t span = 0; span < rule.mKeySpanCount; ++span)
    {
        const PayloadSpan current = rule.mKeySpans[span];
        key.append(reinterpret_cast<const char*>(event.data()) + EVENT_PAYLOAD_OFFSET + current.mOffset,
                   current.mLength);
    }
    if (!mSeenKeys.insert(std::move(key)).second)
        return false;
    ++mOperations[static_cast<std::size_t>(houseIndex)];
    return true;
}

std::uint32_t OperationCounter::GetOperationCount(int houseIndex) const noexcept
{
    if (houseIndex < 0 || houseIndex >= MAX_HOUSES)
        return 0;
    return mOperations[static_cast<std::size_t>(houseIndex)];
}

std::uint32_t OperationCounter::GetRawEventCount(int houseIndex) const noexcept
{
    if (houseIndex < 0 || houseIndex >= MAX_HOUSES)
        return 0;
    return mRawEvents[static_cast<std::size_t>(houseIndex)];
}

void OperationCounter::Reset() noexcept
{
    mOperations.fill(0);
    mRawEvents.fill(0);
    mFrame.reset();
    mSeenKeys.clear();
}
}
