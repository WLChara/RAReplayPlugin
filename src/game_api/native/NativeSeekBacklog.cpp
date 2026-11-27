#include "NativeSeekBacklog.h"

#include "../../target/TargetAbi.h"

using byte = unsigned char;

#include <objidl.h>
// FootClass.h 先于其他 YRpp 头：TechnoClass 的内联转换要求 FootClass 已完整定义。
#include <FootClass.h>
#include <AbstractClass.h>
#include <ArrayClasses.h>
#include <Drawing.h>
#include <EBolt.h>
#include <GScreenClass.h>
#include <LineTrail.h>
#include <ObjectClass.h>
#include <RadBeam.h>

#include <cstddef>
#include <cstdint>

namespace rareplay::game_api::native
{
namespace
{
// 向量布局与 TargetAbi 的 DYNAMIC_VECTOR_* 一致；YRpp 声明漂移时编译期失败。
static_assert(offsetof(VectorClass<EBolt*>, Items) == target::DYNAMIC_VECTOR_ITEMS_OFFSET);
static_assert(offsetof(DynamicVectorClass<EBolt*>, Count) == target::DYNAMIC_VECTOR_COUNT_OFFSET);
static_assert(offsetof(DynamicVectorClass<DirtyAreaStruct>, Count) == target::DYNAMIC_VECTOR_COUNT_OFFSET);
// RegisterDirtyArea（0x6D2790）与 Render 的移除循环（0x6D451C 起）都按 20 字节一项复制脏矩形。
static_assert(sizeof(DirtyAreaStruct) == 20);
// RadBeam：原生过期（0x6591B0 内 0x659317–0x659380）读 +0 处的对象指针（只核对其 +0x510），以及 +4 处的目标
// （先以字节测试 +0x14 的 Techno 位，再核对 +0x510）；+0x510 即 TechnoClass::FiringRadBeam。
static_assert(offsetof(RadBeam, unknown_0) == 0);
static_assert(offsetof(RadBeam, Owner) == 4);
static_assert(offsetof(TechnoClass, FiringRadBeam) == 0x510);
static_assert(offsetof(AbstractClass, AbstractFlags) == 0x14);
static_assert(static_cast<unsigned int>(AbstractFlags::Techno) == 1);
// LineTrail：构造函数 0x556A20 写 +4 拥有者、+8 衰减量、+0xC 当前槽，32 个节点从 +0x10 起每个 16 字节；节点 +0xC
// （即记录的 +0x1C + 0x10×i）是 int 透明度：0x556B70 以 dword 写 0xFF 并逐次减去衰减量、夹到 0，0x556D70 以 dword 读取。
static_assert(offsetof(LineTrail, Owner) == 4);
static_assert(offsetof(LineTrail, Trails) == 0x10);
static_assert(sizeof(LineTrailNode) == 0x10);
static_assert(offsetof(LineTrailNode, Value) == 0xC);
static_assert(sizeof(LineTrailNode::Value) == 4);
// LineTrail 析构 0x556B30 与 LineTrail::DeleteAll 0x556DF0 清除的物体尾迹指针；这里的清理只删除拥有者已为空的记录，不碰它。
static_assert(offsetof(ObjectClass, LineTrailer) == 0xA8);
// 屏震是 GScreenClass 虚表之后的两个 int；TargetAbi 的 SCREEN_SHAKE_X/Y 即 MouseClass::Instance 的这两个字段
// （DoBlit 0x4F4780 按绝对地址读写）。
static_assert(offsetof(GScreenClass, ScreenShakeX) == 4);
static_assert(offsetof(GScreenClass, ScreenShakeY) == 8);

using NoArgumentFunction = void(__cdecl*)();
using OperatorDeleteFunction = void(__cdecl*)(void*);

template <typename T>
DynamicVectorClass<T>& GetVector(std::uintptr_t address) noexcept
{
    return *reinterpret_cast<DynamicVectorClass<T>*>(address);
}

// 与原生过期路径一样只用游戏的 operator delete 释放：RadBeam 没有析构逻辑，LineTrail 的析构 0x556B30 只在拥有者非空时
// 断开物体的尾迹指针，这里只释放拥有者为空的尾迹。
void FreeWithGameDelete(void* object) noexcept
{
    reinterpret_cast<OperatorDeleteFunction>(target::GAME_OPERATOR_DELETE)(object);
}

// 与原生过期（0x659317–0x659380）相同：两端都先判空，反向指针仍指向本光束时才清除，并把光束上的对应字段清零。
void ReleaseRadBeamBacklinks(RadBeam& beam) noexcept
{
    if (beam.unknown_0 != 0)
    {
        auto*& backlink = *reinterpret_cast<RadBeam**>(
            static_cast<std::uintptr_t>(beam.unknown_0) + offsetof(TechnoClass, FiringRadBeam));
        if (backlink == &beam)
        {
            backlink = nullptr;
            beam.unknown_0 = 0;
        }
    }
    TechnoClass* const target = beam.Owner;
    if (target == nullptr)
        return;
    // 原生只以字节测试 +0x14 的最低位。
    const std::uint8_t flags = *reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(target) + offsetof(AbstractClass, AbstractFlags));
    if ((flags & static_cast<std::uint8_t>(AbstractFlags::Techno)) != 0 && target->FiringRadBeam == &beam)
    {
        target->FiringRadBeam = nullptr;
        beam.Owner = nullptr;
    }
}

// 原生 EBolt_DeleteAll 逐个清除拥有者反向指针并删除，再释放 Items；空表时不调用，免得无谓释放缓冲。
std::int32_t DeleteAllEBolts() noexcept
{
    const std::int32_t count = GetVector<EBolt*>(target::EBOLT_ARRAY).Count;
    if (count <= 0)
        return 0;
    reinterpret_cast<NoArgumentFunction>(target::EBOLT_DELETE_ALL)();
    return count;
}

// 逐个按原生过期的做法删除；全部处理完后 Count=0，保留 Items 与容量。原生每帧遍历时不判空，这里遇到空项只跳过。
std::int32_t DeleteAllRadBeams() noexcept
{
    auto& beams = GetVector<RadBeam*>(target::RADBEAM_ARRAY);
    std::int32_t deleted = 0;
    for (int index = 0; index < beams.Count; ++index)
    {
        RadBeam* const beam = beams.Items[index];
        if (beam == nullptr)
            continue;
        ReleaseRadBeamBacklinks(*beam);
        FreeWithGameDelete(beam);
        ++deleted;
    }
    beams.Count = 0;
    return deleted;
}

// 删除拥有者已解绑（+4 为空）的尾迹，其余记录按原顺序前移；原生淡出完毕时（0x556D40）以同一条件从表中移出并释放。
std::int32_t DeleteDetachedLineTrails() noexcept
{
    auto& trails = GetVector<LineTrail*>(target::LINE_TRAIL_ARRAY);
    std::int32_t kept = 0;
    std::int32_t deleted = 0;
    for (int index = 0; index < trails.Count; ++index)
    {
        LineTrail* const trail = trails.Items[index];
        if (trail != nullptr && trail->Owner == nullptr)
        {
            FreeWithGameDelete(trail);
            ++deleted;
            continue;
        }
        trails.Items[kept++] = trail;
    }
    trails.Count = kept;
    return deleted;
}

// 仍挂在物体上的尾迹：透明度全部为 0 时原生不画；物体下次移动，0x556B70 在当前位置写入新点（透明度 0xFF）重新开始。
void HideAttachedLineTrails() noexcept
{
    const auto& trails = GetVector<LineTrail*>(target::LINE_TRAIL_ARRAY);
    for (int index = 0; index < trails.Count; ++index)
    {
        LineTrail* const trail = trails.Items[index];
        if (trail == nullptr || trail->Owner == nullptr)
            continue;
        for (LineTrailNode& node : trail->Trails)
            node.Value = 0;
    }
}
}

SeekBacklogCounts ReadSeekBacklogCounts() noexcept
{
    SeekBacklogCounts counts;
    counts.mDirtyAreas = GetVector<DirtyAreaStruct>(target::DIRTY_AREAS).Count;
    counts.mEBolts = GetVector<EBolt*>(target::EBOLT_ARRAY).Count;
    counts.mRadBeams = GetVector<RadBeam*>(target::RADBEAM_ARRAY).Count;
    counts.mLineTrails = GetVector<LineTrail*>(target::LINE_TRAIL_ARRAY).Count;
    counts.mRadarEvents = GetVector<void*>(target::RADAR_EVENT_ARRAY).Count;
    return counts;
}

SeekBacklogCleanupResult CleanSeekBacklog(const SeekBacklogCleanup& cleanup) noexcept
{
    SeekBacklogCleanupResult result;
    if (cleanup.mClearDirtyAreas)
        GetVector<DirtyAreaStruct>(target::DIRTY_AREAS).Count = 0;
    if (cleanup.mDeleteEBolts)
        result.mEBoltsDeleted = DeleteAllEBolts();
    if (cleanup.mDeleteRadBeams)
        result.mRadBeamsDeleted = DeleteAllRadBeams();
    if (cleanup.mDeleteDetachedLineTrails)
        result.mLineTrailsDeleted = DeleteDetachedLineTrails();
    if (cleanup.mHideAttachedLineTrails)
        HideAttachedLineTrails();
    if (cleanup.mClearRadarEvents)
    {
        result.mRadarEventsCleared = GetVector<void*>(target::RADAR_EVENT_ARRAY).Count;
        reinterpret_cast<NoArgumentFunction>(target::CLEAR_RADAR_EVENTS)();
    }
    if (cleanup.mClearScreenShake)
    {
        *reinterpret_cast<volatile int*>(target::SCREEN_SHAKE_X) = 0;
        *reinterpret_cast<volatile int*>(target::SCREEN_SHAKE_Y) = 0;
    }
    return result;
}
}
