#include "BattlefieldZoomAdapter.h"

#include "ZoomFramePlan.h"
#include "../../target/TargetAbi.h"
#include "../../zoom/ScalePixels.h"
#include "../../zoom/ZoomAnchor.h"

// 此版 YRpp 的若干声明仍使用 Win32 的小写 byte 基类型。
using byte = unsigned char;

#include <objidl.h>
#include <Drawing.h>
#include <GameOptionsClass.h>
#include <Memory.h>
#include <MouseClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <Unsorted.h>
#include <WWMouseClass.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace rareplay::game_api::zoom
{
namespace
{
using rareplay::zoom::BattlefieldScaler;
using rareplay::zoom::ConstPixelView16;
using rareplay::zoom::IDENTITY_ZOOM_INDEX;
using rareplay::zoom::LogicalSize;
using rareplay::zoom::PixelFormat16;
using rareplay::zoom::PixelView16;
using rareplay::zoom::ZoomLevelPolicy;
using rareplay::zoom::ZoomPoint;
using rareplay::zoom::ZoomTransform;

// 以下偏移经 IDA 对照原生指令核对；YRpp 声明漂移时编译期失败。
// 注意 YRpp 的若干字段名与实际含义不符，只按偏移使用。
static_assert(sizeof(Point2D) == 8 && sizeof(RectangleStruct) == 16);
static_assert(offsetof(TacticalClass, TacticalPos) == 0xB0);
static_assert(offsetof(TacticalClass, LastTacticalPos) == 0xB8);
static_assert(offsetof(TacticalClass, ZoomInFactor) == 0xC0);
// field_D8：FocusOn 镜头平移动画的剩余量（float）。
static_assert(offsetof(TacticalClass, field_D8) == 0xD8);
static_assert(offsetof(TacticalClass, VisibleCellCount) == 0xE0);
static_assert(offsetof(TacticalClass, Band) == 0xD90);
// unknown_byte_554A 实际位于 +0x555A：鼠标捕获（手势进行中）标志，绝对地址 0x884D42。
static_assert(offsetof(ScrollClass, unknown_byte_554A) == 0x555A);
static_assert(offsetof(DisplayClass, LeftPressAndDraggingRectangle) == 0x11CF);
// unknown_bool_11D0：左键已按下、尚未越过拖框阈值。
static_assert(offsetof(DisplayClass, unknown_bool_11D0) == 0x11D0);
static_assert(offsetof(GameOptionsClass, SidebarMode) == 0x1C);
static_assert(offsetof(DSurface, LockLevel) == 0xC && offsetof(DSurface, BytesPerPixel) == 0x10);
static_assert(offsetof(DSurface, IsInVideoRam) == 0x19 && offsetof(DSurface, VideoSurfacePtr) == 0x1C);
static_assert(offsetof(DSurface, VideoSurfaceDescription) == 0x20);
static_assert(sizeof(DSurface) == target::VDSURFACE_OBJECT_BYTES);
static_assert(sizeof(ZBuffer) == target::DEPTH_BUFFER_OBJECT_BYTES && sizeof(ABuffer) == target::DEPTH_BUFFER_OBJECT_BYTES);
static_assert(offsetof(ZBuffer, Width) == 0x28 && offsetof(ABuffer, Width) == 0x28);
static_assert(offsetof(DDSURFACEDESC2, ddpfPixelFormat) == 0x48);

// 布尔参数以 int 0/1 压栈：VDSurface_Create 用 cmp al,1 判定 requestSystemMemory。
using VDSurfaceCreateFunction = DSurface*(__thiscall*)(void* storage, int width, int height, int requestSystemMemory,
    int request3DDevice);
using SetViewportRectFunction = void(__thiscall*)(TacticalClass* tactical, const RectangleStruct* rect);
using SetCenterPixelFunction = void(__thiscall*)(TacticalClass* tactical, const Point2D* center);
using GetRgbModeFunction = int(__cdecl*)();
// 虚表槽 0：标量删除析构（DSurface 为 0x4C1AC0），flags & 1 时用游戏 operator delete 释放对象。
using ScalarDeletingDestructorFunction = void*(__thiscall*)(void* self, unsigned int flags);

constexpr std::size_t MAX_PENDING_LOG_LINES = 64;
// 每类反复出现的诊断只记前几行，其余只计数并在汇总中报告。
constexpr std::uint32_t MAX_REPEATED_LOG_LINES = 4;
// 资源退档循环的安全上限：每轮要么退一档、要么降低最远档，档位表只有 12 项。
constexpr int MAX_APPLY_ATTEMPTS = 32;

int& TacticalViewportWidth() noexcept
{
    return *reinterpret_cast<int*>(target::TACTICAL_VIEWPORT_WIDTH);
}

int& TacticalViewportHeight() noexcept
{
    return *reinterpret_cast<int*>(target::TACTICAL_VIEWPORT_HEIGHT);
}

bool IsByteSet(std::uintptr_t address) noexcept
{
    return *reinterpret_cast<const volatile std::uint8_t*>(address) != 0;
}

// 原生以 test al,al 按字节判非零；按字节读取，避免非 0/1 值的 bool 语义。
bool IsFlagSet(const bool& flag) noexcept
{
    return reinterpret_cast<const volatile std::uint8_t&>(flag) != 0;
}

bool IsFlagSet(const BYTE& flag) noexcept
{
    return flag != 0;
}

std::uint32_t ReadFloatBits(const float& value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint64_t ReadDoubleBits(const double& value) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int GetRgbMode() noexcept
{
    return reinterpret_cast<GetRgbModeFunction>(target::GET_RGB_MODE)();
}

std::optional<PixelFormat16> ToPixelFormat(int rgbMode) noexcept
{
    if (rgbMode == target::RGB_MODE_565)
        return PixelFormat16::Rgb565;
    if (rgbMode == target::RGB_MODE_555)
        return PixelFormat16::Rgb555;
    return std::nullopt;
}

void SetViewportRect(TacticalClass* tactical, int width, int height) noexcept
{
    const RectangleStruct rect{0, 0, width, height};
    reinterpret_cast<SetViewportRectFunction>(target::TACTICAL_SET_VIEWPORT_RECT)(tactical, &rect);
}

void SetCenterPixel(TacticalClass* tactical, ZoomPoint center) noexcept
{
    const Point2D point{center.mX, center.mY};
    reinterpret_cast<SetCenterPixelFunction>(target::TACTICAL_SET_CENTER_PIXEL)(tactical, &point);
}

// 存储由游戏 operator new 分配；VDSurface_Create 构造后返回同一地址。分配失败返回空，不调用原生函数。
DSurface* CreateSurface(int width, int height, int requestSystemMemory, int request3DDevice) noexcept
{
    void* storage = YRMemory::Allocate(target::VDSURFACE_OBJECT_BYTES);
    if (!storage)
        return nullptr;
    return reinterpret_cast<VDSurfaceCreateFunction>(target::VDSURFACE_CREATE)(storage, width, height,
        requestSystemMemory, request3DDevice);
}

// 走原生销毁入口 vt[0](1)：描述与对象都由游戏 operator delete 释放，IDirectDrawSurface 为空时不调用 Release（0x4C1B0D）。
void DestroySurface(DSurface* surface) noexcept
{
    if (!surface)
        return;
    const auto destroy = (*reinterpret_cast<ScalarDeletingDestructorFunction* const*>(surface))[0];
    destroy(surface, 1);
}

bool IsSurfaceUsable(const DSurface* surface) noexcept
{
    return surface && surface->VideoSurfacePtr != nullptr;
}

enum class WindowPhase
{
    // 窗口外：全局都是原生物理值。
    Closed,
    // H1 → H2：Render(0)、E、Render(1)，全局换成逻辑值。
    World,
    // H2 → H3：TabClass::Draw，全局恢复为物理值。
    Tab,
    // H3 → H4（或 D）：Render(2) 世界层，全局换成逻辑值。
    WorldLayer
};

/** H1 时保存的物理状态；H2、H4、D 写回，H3 重新读取（TabClass::Draw 期间原生自行恢复的值）。 */
struct PhysicalFrame
{
    // Pframe：本帧 H1 时的 g_CompositeSurface（时空特效嵌套离屏重绘时是特效临时面）。
    DSurface* mComposite = nullptr;
    DSurface* mTile = nullptr;
    DSurface* mTemp = nullptr;
    ZBuffer* mZBuffer = nullptr;
    ABuffer* mABuffer = nullptr;
    int mViewWidth = 0;
    int mViewHeight = 0;
};

/** H1 读到的引擎状态。 */
struct FrameReadings
{
    ZoomGateState mGate;
    int mPhysicalWidth = 0;
    int mPhysicalHeight = 0;
    int mViewportWidth = 0;
    int mViewportHeight = 0;
    bool mIsGestureActive = false;
    bool mIsCameraPanActive = false;
    bool mIsNestedOffscreenRedraw = false;
    int mVisibleCellCount = 0;
};

struct Counters
{
    std::uint32_t mEnteredFrames = 0;
    std::uint32_t mScaledFrames = 0;
    std::uint32_t mLockFailures = 0;
    std::uint32_t mPreLockedFrames = 0;
    std::uint32_t mLevelChanges = 0;
    std::uint32_t mReapplies = 0;
    std::uint32_t mDeferredFrames = 0;
    std::uint32_t mGateExits = 0;
    std::uint32_t mNestedFrames = 0;
    std::uint32_t mVisibleCellRedraws = 0;
    std::uint32_t mTailFinishes = 0;
    std::uint32_t mResourceFallbacks = 0;
    std::uint32_t mDroppedLogLines = 0;
};

const char* DescribeLevel(int index) noexcept
{
    static constexpr const char* NAMES[] = {"4x", "3x", "2.5x", "2x", "1.5x", "1.25x", "1x", "0.8x", "2/3x", "0.5x",
        "0.4x", "1/3x"};
    static_assert(std::size(NAMES) == rareplay::zoom::ZOOM_LEVELS.size());
    return index >= 0 && index < static_cast<int>(std::size(NAMES)) ? NAMES[index] : "?";
}

/**
 * 游戏线程上的缩放状态机。窗口内（World、WorldLayer）逻辑资源换入全局，窗口外全局永远是原生物理值；
 * 已应用档位非 1× 时战术视口常驻逻辑尺寸 L。逻辑面只在窗口外创建与销毁。
 */
class BattlefieldZoomAdapter final
{
public:
    void Enable()
    {
        if (!mIsEnabled)
        {
            mIsEnabled = true;
            // 上一会话在窗口内停用而未收尾时，由本会话接管：下一次 H1 按复原请求退回 1×。
            mIsDisablePending = false;
            mDisableReason = ZoomDisableReason::None;
            mRequestedIndex = IDENTITY_ZOOM_INDEX;
            mRequestAnchor = ZoomStepAnchor::Center;
            mFarthestLimitIndex = rareplay::zoom::FARTHEST_SHRINK_ZOOM_INDEX;
            mPolicyLimitIndex = -1;
            mCounters = {};
            mGateLogCount = 0;
            mHasLoggedPixelFormat = false;
            mIsGateClosed = false;
            mHasSessionStarted = true;
            char line[96];
            std::snprintf(line, sizeof(line), "Battlefield zoom enabled (memory budget %u MiB)",
                static_cast<unsigned>(rareplay::zoom::DEFAULT_ZOOM_MEMORY_BUDGET_BYTES / (1024 * 1024)));
            AddLog(line);
        }
    }

    void Disable()
    {
        mIsEnabled = false;
        if (mDisableReason == ZoomDisableReason::None)
            mDisableReason = ZoomDisableReason::NotEnabled;
        if (mPhase != WindowPhase::Closed)
        {
            // 不在窗口内释放换入全局的资源；由下一次 H1 完成收尾。
            mIsDisablePending = true;
            return;
        }
        if (ShutDown())
            mIsFullRedrawPending = true;
        if (mHasSessionStarted)
        {
            mHasSessionStarted = false;
            AddSummary();
        }
    }

    void RequestStep(int steps, ZoomStepAnchor anchor)
    {
        if (!mIsEnabled)
            return;
        if (mPhase == WindowPhase::Closed)
            RefreshPolicy(DSurface::ViewBounds.Width, DSurface::ViewBounds.Height);
        mRequestedIndex = mPolicy.Step(mRequestedIndex, steps);
        mRequestAnchor = anchor;
    }

    void RequestReset()
    {
        if (!mIsEnabled)
            return;
        mRequestedIndex = IDENTITY_ZOOM_INDEX;
        mRequestAnchor = ZoomStepAnchor::Center;
    }

    BattlefieldZoomStatus GetStatus() const noexcept
    {
        BattlefieldZoomStatus status;
        status.mIsEnabled = mIsEnabled;
        status.mIsActive = IsActive();
        status.mAppliedIndex = mAppliedIndex;
        status.mRequestedIndex = mRequestedIndex;
        status.mMostMagnifiedIndex = mPolicy.GetMostMagnifiedIndex();
        status.mFarthestShrinkIndex = mPolicy.GetFarthestShrinkIndex();
        status.mDisableReason = mDisableReason;
        status.mIsGateClosed = mIsGateClosed;
        status.mTransform = IsActive() ? mTransform
                                       : ZoomTransform{mPolicyWidth, mPolicyHeight, mPolicyWidth, mPolicyHeight};
        return status;
    }

    std::vector<std::string> TakeLogLines() noexcept
    {
        return std::exchange(mLogLines, {});
    }

    ZoomPoint MapViewPointToPhysical(ZoomPoint logicalPoint) const noexcept
    {
        return IsViewportResident() ? mTransform.MapLogicalToPhysical(logicalPoint) : logicalPoint;
    }

    // ---------------- H1 ----------------
    bool OnDrawEnter() noexcept
    {
        if (!IsEngaged())
            return false;
        try
        {
            return EnterFrame();
        }
        catch (...)
        {
            EmergencyExit();
            return true;
        }
    }

    // ---------------- E ----------------
    void OnTileExposed(bool isRedrawRequested) noexcept
    {
        if (mPhase != WindowPhase::World)
            return;
        TacticalClass* tactical = TacticalClass::Instance;
        DSurface* tile = DSurface::Tile;
        if (!tactical || !IsLogicalSurface(tile))
            return;
        const bool isFullRedraw = isRedrawRequested || IsByteSet(target::TACTICAL_FULL_REDRAW_REQUESTED);
        const ExposedBands bands = ComputeExposedBands(ClampToInt(static_cast<std::int64_t>(tactical->LastTacticalPos.X) -
                                                           tactical->TacticalPos.X),
            ClampToInt(static_cast<std::int64_t>(tactical->LastTacticalPos.Y) - tactical->TacticalPos.Y),
            mTransform.mLogicalWidth, mTransform.mLogicalHeight, isFullRedraw);
        // 地图外的区域原生从不重画，Tile 里留着上一帧的像素；Render(1) 随后只在合法格上重画地形。
        for (int index = 0; index < bands.mCount; ++index)
        {
            const ZoomRect& band = bands.mRects[static_cast<std::size_t>(index)];
            RectangleStruct rect{band.mX, band.mY, band.mWidth, band.mHeight};
            tile->FillRect(&rect, 0);
        }
    }

    // ---------------- H2 ----------------
    void OnTabDrawBegin() noexcept
    {
        if (mPhase != WindowPhase::World)
            return;
        // Render(0) 滚屏时交换 Composite/Tile，逻辑对以当前全局为准。
        const bool isPairIntact = TakeLogicalPairFromGlobals();
        if (!isPairIntact)
            Fail(ZoomDisableReason::UnexpectedEngineState, "Battlefield zoom: logical surfaces were replaced before TabClass::Draw");
        RestorePhysicalFrame();
        // 逻辑对完好时照常走完本帧（即使已标记停用），否则 Render(2) 会以常驻 L 视口画进物理面。
        mPhase = isPairIntact ? WindowPhase::Tab : WindowPhase::Closed;
    }

    // ---------------- H3 ----------------
    void OnTabDrawEnd() noexcept
    {
        if (mPhase != WindowPhase::Tab)
            return;
        RestoreRadarFrame();
        if (DSurface::Composite != mFrame.mComposite)
        {
            Fail(ZoomDisableReason::UnexpectedEngineState, "Battlefield zoom: composite surface changed during TabClass::Draw");
            mPhase = WindowPhase::Closed;
            return;
        }
        CapturePhysicalFrame(mFrame.mComposite);
        SwapInLogicalFrame();
        mPhase = WindowPhase::WorldLayer;
    }

    // ---------------- H4 / D ----------------
    void FinishWorldLayer(const void* renderSurface, bool isTail) noexcept
    {
        if (mPhase == WindowPhase::Closed)
            return;
        if (mPhase == WindowPhase::Tab)
        {
            // 只在 D 处可能：H3 未到达，全局已是物理值。
            mPhase = WindowPhase::Closed;
            return;
        }
        if (isTail)
            ++mCounters.mTailFinishes;
        DSurface* source = DSurface::Composite;
        const bool isExpected = TakeLogicalPairFromGlobals() && (!renderSurface || renderSurface == source);
        if (isExpected)
            ScaleToFrame(source);
        else
            Fail(ZoomDisableReason::UnexpectedEngineState, "Battlefield zoom: logical composite mismatch at the end of the world layer");
        // 无论缩放成功与否都恢复物理状态：之后的计时器、文字、界面与光标都按物理值画进 Pframe。
        RestorePhysicalFrame();
        mPhase = WindowPhase::Closed;
    }

    // ---------------- 窗口外读取的代入值 ----------------
    std::optional<int> GetPhysicalViewWidth() const noexcept
    {
        return IsActive() ? std::optional<int>(PhysicalViewWidth()) : std::nullopt;
    }

    std::optional<int> GetPhysicalViewHeight() const noexcept
    {
        return IsActive() ? std::optional<int>(PhysicalViewHeight()) : std::nullopt;
    }

    std::optional<int> GetViewportWidth() const noexcept
    {
        return IsActive() ? std::optional<int>(TacticalViewportWidth()) : std::nullopt;
    }

    std::optional<int> GetViewportHeight() const noexcept
    {
        return IsActive() ? std::optional<int>(TacticalViewportHeight()) : std::nullopt;
    }

    // 原生复位视口（分辨率切换、读场景）时视口等于 P，min 自动退化为原生值。
    std::optional<int> GetCameraClampWidth() const noexcept
    {
        return IsActive() ? std::optional<int>(std::min(TacticalViewportWidth(), PhysicalViewWidth())) : std::nullopt;
    }

    std::optional<int> GetCameraClampHeight() const noexcept
    {
        return IsActive() ? std::optional<int>(std::min(TacticalViewportHeight(), PhysicalViewHeight())) : std::nullopt;
    }

    // RF1/RF2：只做整数读写，不得触碰 x87（RF2 处 x87 栈有 1 项）。
    void OnRadarFrameBegin() noexcept
    {
        if (!IsActive())
            return;
        RestoreRadarFrame();
        mRadarSavedWidth = DSurface::ViewBounds.Width;
        mRadarSavedHeight = DSurface::ViewBounds.Height;
        DSurface::ViewBounds.Width = TacticalViewportWidth();
        DSurface::ViewBounds.Height = TacticalViewportHeight();
        mIsRadarFrameSwapped = true;
    }

    void RestoreRadarFrame() noexcept
    {
        if (!mIsRadarFrameSwapped)
            return;
        DSurface::ViewBounds.Width = mRadarSavedWidth;
        DSurface::ViewBounds.Height = mRadarSavedHeight;
        mIsRadarFrameSwapped = false;
    }

    void AdjustSoundFieldHalfExtents(void* halfWidthSlot, void* halfHeightSlot) const noexcept
    {
        if (!IsActive() || !halfWidthSlot || !halfHeightSlot)
            return;
        *static_cast<std::uint32_t*>(halfWidthSlot) = HalfExtentFloatBits(TacticalViewportWidth());
        *static_cast<std::uint32_t*>(halfHeightSlot) = HalfExtentFloatBits(TacticalViewportHeight());
    }

    const void* MapClickPoint(const void* physicalPoint) noexcept
    {
        return MapInputPoint(physicalPoint, mClickPoint);
    }

    const void* MapDragBandStart(const void* physicalPoint) noexcept
    {
        return MapInputPoint(physicalPoint, mDragBandStartPoint);
    }

    const void* MapDragBandEnd(const void* physicalPoint) noexcept
    {
        return MapInputPoint(physicalPoint, mDragBandEndPoint);
    }

    void Fail(ZoomDisableReason reason, const char* message) noexcept
    {
        if (mIsEnabled)
            AddLog(message);
        mIsEnabled = false;
        mDisableReason = reason;
    }

private:
    bool IsActive() const noexcept { return mAppliedIndex != IDENTITY_ZOOM_INDEX; }

    bool HasResources() const noexcept
    {
        return mLogicalComposite || mLogicalTile || mLogicalZBuffer || mLogicalABuffer;
    }

    bool IsEngaged() const noexcept
    {
        return mIsEnabled || IsActive() || HasResources() || mPhase != WindowPhase::Closed || mIsFullRedrawPending ||
            mIsDisablePending;
    }

    /** 战术视口仍是本适配器写入的 L（未被原生复位），输入与叠加图标才按缩放映射。 */
    bool IsViewportResident() const noexcept
    {
        return IsActive() && mTransform.IsValid() && TacticalViewportWidth() == mTransform.mLogicalWidth &&
            TacticalViewportHeight() == mTransform.mLogicalHeight;
    }

    bool IsLogicalSurface(const DSurface* surface) const noexcept
    {
        return surface && (surface == mLogicalComposite || surface == mLogicalTile);
    }

    // 物理战术区：窗口内 g_ViewBounds 已换成 L，取 H1/H3 保存的值；雷达视框期间取 RF1 保存的值；
    // 其余时刻 g_ViewBounds 就是物理 P（原生改变分辨率时也立即跟随）。
    int PhysicalViewWidth() const noexcept
    {
        if (mPhase == WindowPhase::World || mPhase == WindowPhase::WorldLayer)
            return mFrame.mViewWidth;
        return mIsRadarFrameSwapped ? mRadarSavedWidth : DSurface::ViewBounds.Width;
    }

    int PhysicalViewHeight() const noexcept
    {
        if (mPhase == WindowPhase::World || mPhase == WindowPhase::WorldLayer)
            return mFrame.mViewHeight;
        return mIsRadarFrameSwapped ? mRadarSavedHeight : DSurface::ViewBounds.Height;
    }

    static int ClampToInt(std::int64_t value) noexcept
    {
        return static_cast<int>(std::clamp<std::int64_t>(value, INT_MIN, INT_MAX));
    }

    const void* MapInputPoint(const void* physicalPoint, Point2D& copy) noexcept
    {
        if (!physicalPoint || !IsViewportResident())
            return nullptr;
        const auto& point = *static_cast<const Point2D*>(physicalPoint);
        const ZoomPoint logical = mTransform.MapPhysicalToLogical({point.X, point.Y});
        copy = Point2D{logical.mX, logical.mY};
        return &copy;
    }

    // 字符串只在 try 内构造：本类的大多数函数是 noexcept，日志失败只计数。
    void AddLog(const char* text, const char* suffix = "") noexcept
    {
        try
        {
            if (mLogLines.size() >= MAX_PENDING_LOG_LINES)
            {
                ++mCounters.mDroppedLogLines;
                return;
            }
            std::string line(text);
            line += suffix;
            mLogLines.push_back(std::move(line));
        }
        catch (...) { ++mCounters.mDroppedLogLines; }
    }

    void AddSummary() noexcept
    {
        try
        {
            char line[512];
            std::snprintf(line, sizeof(line),
                "Battlefield zoom summary: entered=%u scaled=%u lockFailures=%u preLocked=%u levelChanges=%u reapplied=%u "
                "deferred=%u gateExits=%u nested=%u visibleCellRedraws=%u tailFinishes=%u resourceFallbacks=%u "
                "droppedLogLines=%u disableReason=%d",
                mCounters.mEnteredFrames, mCounters.mScaledFrames, mCounters.mLockFailures, mCounters.mPreLockedFrames,
                mCounters.mLevelChanges, mCounters.mReapplies, mCounters.mDeferredFrames, mCounters.mGateExits,
                mCounters.mNestedFrames, mCounters.mVisibleCellRedraws, mCounters.mTailFinishes,
                mCounters.mResourceFallbacks, mCounters.mDroppedLogLines, static_cast<int>(mDisableReason));
            AddLog(line);
        }
        catch (...) {}
    }

    void RefreshPolicy(int physicalWidth, int physicalHeight) noexcept
    {
        if (physicalWidth == mPolicyWidth && physicalHeight == mPolicyHeight && mFarthestLimitIndex == mPolicyLimitIndex)
            return;
        mPolicy = ZoomLevelPolicy(physicalWidth, physicalHeight, rareplay::zoom::DEFAULT_ZOOM_MEMORY_BUDGET_BYTES,
            mFarthestLimitIndex);
        mPolicyWidth = physicalWidth;
        mPolicyHeight = physicalHeight;
        mPolicyLimitIndex = mFarthestLimitIndex;
        mRequestedIndex = mPolicy.Step(mRequestedIndex, 0);
    }

    FrameReadings ReadFrame() const noexcept
    {
        FrameReadings readings;
        TacticalClass* tactical = TacticalClass::Instance;
        const RectangleStruct& view = DSurface::ViewBounds;
        readings.mPhysicalWidth = view.Width;
        readings.mPhysicalHeight = view.Height;
        readings.mViewportWidth = TacticalViewportWidth();
        readings.mViewportHeight = TacticalViewportHeight();

        ZoomGateState& gate = readings.mGate;
        gate.mHasTactical = tactical != nullptr;
        gate.mIsZoomInFactorIdentity = tactical && ReadDoubleBits(tactical->ZoomInFactor) == target::ZOOM_IN_FACTOR_IDENTITY_BITS;
        gate.mIsSidebarOnRight = IsFlagSet(GameOptionsClass::Instance.SidebarMode);
        gate.mIsArmageddonMode = Unsorted::ArmageddonMode != 0;
        gate.mIsViewOriginZero = view.X == 0 && view.Y == 0;
        DSurface* frame = DSurface::Composite;
        gate.mIsFrameSurfaceSupported = frame && ToPixelFormat(GetRgbMode()) && frame->GetBytesPerPixel() == 2 &&
            frame->GetWidth() >= view.Width && frame->GetHeight() >= view.Height;

        // 手势判据：捕获标志 +0x555A 为主（充分必要），其余为防御项。
        readings.mIsGestureActive = IsFlagSet(ScrollClass::Instance.unknown_byte_554A) ||
            IsFlagSet(DisplayClass::Instance.LeftPressAndDraggingRectangle) ||
            IsFlagSet(DisplayClass::Instance.unknown_bool_11D0) ||
            (tactical && (tactical->Band.Left != 0 || tactical->Band.Top != 0));
        readings.mIsCameraPanActive = tactical && IsNonZeroFloatBits(ReadFloatBits(tactical->field_D8));
        readings.mIsNestedOffscreenRedraw = IsByteSet(target::CHRONO_OFFSCREEN_REDRAW);
        readings.mVisibleCellCount = tactical ? tactical->VisibleCellCount : 0;
        return readings;
    }

    bool EnterFrame()
    {
        bool isFullRedraw = std::exchange(mIsFullRedrawPending, false);
        if (mPhase != WindowPhase::Closed)
        {
            // 上一帧的窗口没有关闭（H4 与 D 都未到达）：先把全局恢复为物理值。
            AddLog("Battlefield zoom: previous draw window was not closed; physical state restored");
            RestorePhysicalFrame();
            mPhase = WindowPhase::Closed;
        }
        RestoreRadarFrame();
        if (mIsDisablePending || !mIsEnabled)
        {
            const bool wasPending = std::exchange(mIsDisablePending, false);
            const bool wasActive = ShutDown();
            if (wasPending && mHasSessionStarted)
            {
                mHasSessionStarted = false;
                AddSummary();
            }
            return isFullRedraw || wasActive;
        }

        // 允许范围随物理战术区更新（供状态查询）；1× 且没有换档请求时不再读取其余状态，Hook 原样返回。
        RefreshPolicy(DSurface::ViewBounds.Width, DSurface::ViewBounds.Height);
        if (!IsActive() && mRequestedIndex == IDENTITY_ZOOM_INDEX)
            return isFullRedraw;
        const FrameReadings readings = ReadFrame();
        // 只供状态查询（HUD 的“不支持缩放”提示），不参与本帧决策。
        mIsGateClosed = !IsZoomGateOpen(readings.mGate);
        ZoomFrameInput input;
        input.mIsEnabled = true;
        input.mGate = readings.mGate;
        input.mAreResourcesReady = AreResourcesReadyFor(mAppliedIndex);
        input.mIsGestureActive = readings.mIsGestureActive;
        input.mIsCameraPanActive = readings.mIsCameraPanActive;
        input.mIsNestedOffscreenRedraw = readings.mIsNestedOffscreenRedraw;
        input.mPhysicalWidth = readings.mPhysicalWidth;
        input.mPhysicalHeight = readings.mPhysicalHeight;
        input.mViewportWidth = readings.mViewportWidth;
        input.mViewportHeight = readings.mViewportHeight;
        input.mAppliedIndex = mAppliedIndex;
        input.mAppliedPhysicalWidth = mTransform.mPhysicalWidth;
        input.mAppliedPhysicalHeight = mTransform.mPhysicalHeight;
        input.mAppliedLogicalWidth = mTransform.mLogicalWidth;
        input.mAppliedLogicalHeight = mTransform.mLogicalHeight;
        input.mRequestedIndex = mRequestedIndex;
        input.mMostMagnifiedIndex = mPolicy.GetMostMagnifiedIndex();
        input.mFarthestShrinkIndex = mPolicy.GetFarthestShrinkIndex();
        input.mVisibleCellCount = readings.mVisibleCellCount;
        const ZoomFramePlan plan = PlanZoomFrame(input);

        if (readings.mIsNestedOffscreenRedraw)
            ++mCounters.mNestedFrames;
        if (plan.mTargetIndex != mRequestedIndex && IsZoomGateOpen(readings.mGate) &&
            (readings.mIsGestureActive || readings.mIsCameraPanActive || readings.mIsNestedOffscreenRedraw))
            ++mCounters.mDeferredFrames;

        switch (plan.mAction)
        {
        case ZoomFrameAction::KeepNative:
            return isFullRedraw;
        case ZoomFrameAction::ExitToIdentity:
            if (!plan.mIsLevelChange)
                ReportGateExit(readings);
            else
                ++mCounters.mLevelChanges;
            ExitToIdentity(plan.mIsLevelChange, readings);
            return true;
        case ZoomFrameAction::ApplyLevelThenEnter:
            if (plan.mIsLevelChange)
                ++mCounters.mLevelChanges;
            else
                ++mCounters.mReapplies;
            if (!ApplyLevel(plan.mTargetIndex, plan.mIsLevelChange, readings))
                return true;
            EnterWindow();
            return true;
        case ZoomFrameAction::Enter:
            if (plan.mIsFullRedraw)
                ++mCounters.mVisibleCellRedraws;
            EnterWindow();
            return isFullRedraw || plan.mIsFullRedraw;
        }
        return isFullRedraw;
    }

    void ReportGateExit(const FrameReadings& readings) noexcept
    {
        ++mCounters.mGateExits;
        if (mGateLogCount >= MAX_REPEATED_LOG_LINES)
            return;
        ++mGateLogCount;
        const ZoomGateState& gate = readings.mGate;
        char line[256];
        std::snprintf(line, sizeof(line),
            "Battlefield zoom gate closed; returning to 1x (tactical=%d zoomInFactorOne=%d sidebarRight=%d armageddon=%d "
            "viewOrigin=%d frameSurface=%d P=%dx%d)",
            gate.mHasTactical, gate.mIsZoomInFactorIdentity, gate.mIsSidebarOnRight, gate.mIsArmageddonMode,
            gate.mIsViewOriginZero, gate.mIsFrameSurfaceSupported, readings.mPhysicalWidth, readings.mPhysicalHeight);
        AddLog(line);
    }

    bool AreResourcesReadyFor(int index) const noexcept
    {
        if (index == IDENTITY_ZOOM_INDEX)
            return true;
        return IsSurfaceUsable(mLogicalComposite) && IsSurfaceUsable(mLogicalTile) && mLogicalZBuffer && mLogicalABuffer &&
            mSurfaceWidth >= mTransform.mLogicalWidth && mSurfaceHeight >= mTransform.mLogicalHeight &&
            mDepthWidth == mTransform.mLogicalWidth && mDepthHeight == mTransform.mLogicalHeight;
    }

    // 换档或重新应用：准备资源（失败时退回更近的一档），再写常驻视口与镜头。返回是否处于可进入窗口的激活状态。
    bool ApplyLevel(int targetIndex, bool isLevelChange, const FrameReadings& readings)
    {
        TacticalClass* tactical = TacticalClass::Instance;
        const int physicalWidth = readings.mPhysicalWidth;
        const int physicalHeight = readings.mPhysicalHeight;
        for (int attempt = 0; attempt < MAX_APPLY_ATTEMPTS; ++attempt)
        {
            if (targetIndex == IDENTITY_ZOOM_INDEX || !tactical)
            {
                ExitToIdentity(isLevelChange, readings);
                return false;
            }
            const LogicalSize logical = mPolicy.GetLogicalSize(targetIndex);
            const LogicalSize farthest = mPolicy.GetLogicalSize(mPolicy.GetFarthestShrinkIndex());
            const LogicalSize capacity{std::max({farthest.mWidth, logical.mWidth, physicalWidth}),
                std::max({farthest.mHeight, logical.mHeight, physicalHeight})};
            const ZoomTransform transform{physicalWidth, physicalHeight, logical.mWidth, logical.mHeight};
            if (!transform.IsValid())
            {
                Fail(ZoomDisableReason::ResourceUnavailable, "Battlefield zoom: logical size is out of range");
                ExitToIdentity(false, readings);
                return false;
            }
            if (!EnsureSurfaces(capacity))
            {
                ++mCounters.mResourceFallbacks;
                if (!LowerFarthestLimit(physicalWidth, physicalHeight))
                {
                    Fail(ZoomDisableReason::ResourceUnavailable, "Battlefield zoom: logical surfaces unavailable; zoom disabled");
                    ExitToIdentity(false, readings);
                    return false;
                }
                targetIndex = mPolicy.Step(targetIndex, 0);
                continue;
            }
            if (!EnsureDepthBuffers(logical))
            {
                ++mCounters.mResourceFallbacks;
                AddLog("Battlefield zoom: depth buffers unavailable; trying a level nearer to 1x than ", DescribeLevel(targetIndex));
                if (targetIndex > IDENTITY_ZOOM_INDEX)
                {
                    mFarthestLimitIndex = std::min(mFarthestLimitIndex, targetIndex - 1);
                    RefreshPolicy(physicalWidth, physicalHeight);
                }
                targetIndex = StepTowardIdentity(targetIndex);
                continue;
            }
            // 先登记为激活：0x6D5F60 与 0x6D6000 内的 clamp 经 CW/CH Hook 取 min(新视口, P)。
            const ZoomTransform oldTransform{physicalWidth, physicalHeight, readings.mViewportWidth, readings.mViewportHeight};
            const ZoomPoint oldTacticalPos{tactical->TacticalPos.X, tactical->TacticalPos.Y};
            const ZoomPoint anchor = isLevelChange ? ReadAnchor(physicalWidth, physicalHeight) : ZoomPoint{};
            mAppliedIndex = targetIndex;
            mTransform = transform;
            WriteViewport(tactical, oldTransform, transform, isLevelChange, anchor, oldTacticalPos);
            if (isLevelChange)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "Battlefield zoom applied %s: P=%dx%d L=%dx%d", DescribeLevel(targetIndex),
                    physicalWidth, physicalHeight, logical.mWidth, logical.mHeight);
                AddLog(line);
            }
            return true;
        }
        Fail(ZoomDisableReason::ResourceUnavailable, "Battlefield zoom: no usable level after resource fallbacks");
        ExitToIdentity(false, readings);
        return false;
    }

    bool LowerFarthestLimit(int physicalWidth, int physicalHeight) noexcept
    {
        const int current = mPolicy.GetFarthestShrinkIndex();
        if (current <= IDENTITY_ZOOM_INDEX)
            return false;
        mFarthestLimitIndex = current - 1;
        RefreshPolicy(physicalWidth, physicalHeight);
        AddLog("Battlefield zoom: logical surfaces unavailable; farthest level lowered to ",
            DescribeLevel(mPolicy.GetFarthestShrinkIndex()));
        return true;
    }

    // 锚点在 g_ViewBounds 换成 L 之前读取；区外（含读不到 WWMouse）由 ComputeAnchoredView 取 P 中心。
    ZoomPoint ReadAnchor(int physicalWidth, int physicalHeight) const noexcept
    {
        constexpr ZoomPoint OUTSIDE{-1, -1};
        WWMouseClass* mouse = WWMouseClass::Instance;
        if (mRequestAnchor == ZoomStepAnchor::Center || !mouse)
            return OUTSIDE;
        Point2D canvas{0, 0};
        mouse->GetCoords(&canvas);
        const RectangleStruct& view = DSurface::ViewBounds;
        const ZoomPoint relative{canvas.X - view.X, canvas.Y - view.Y};
        const bool isInside = relative.mX >= 0 && relative.mX < physicalWidth && relative.mY >= 0 && relative.mY < physicalHeight;
        return isInside ? relative : OUTSIDE;
    }

    // 0x6D5F60(&{0,0,L2}) 写视口并重新 clamp；换档时再 0x6D6000(&C2) 让锚点下的地面点不动。
    static void WriteViewport(TacticalClass* tactical, const ZoomTransform& oldTransform, const ZoomTransform& newTransform,
        bool useAnchor, ZoomPoint anchor, ZoomPoint oldTacticalPos) noexcept
    {
        std::optional<rareplay::zoom::AnchoredView> view;
        if (useAnchor)
            view = rareplay::zoom::ComputeAnchoredView(oldTransform, newTransform, anchor, oldTacticalPos);
        SetViewportRect(tactical, newTransform.mLogicalWidth, newTransform.mLogicalHeight);
        if (view)
            SetCenterPixel(tactical, view->mCenter);
    }

    // 完整退回 1×（不进入窗口）：视口仍为本适配器写入的 L 时写回物理视口；释放 Z/A，保留 Composite/Tile 供再次进入。
    void ExitToIdentity(bool useAnchor, const FrameReadings& readings) noexcept
    {
        TacticalClass* tactical = TacticalClass::Instance;
        if (tactical && IsViewportResident() && readings.mPhysicalWidth > 0 && readings.mPhysicalHeight > 0)
        {
            const ZoomTransform oldTransform = mTransform;
            const ZoomTransform identity{readings.mPhysicalWidth, readings.mPhysicalHeight, readings.mPhysicalWidth,
                readings.mPhysicalHeight};
            const ZoomPoint oldTacticalPos{tactical->TacticalPos.X, tactical->TacticalPos.Y};
            // 只有观看者换档到 1× 时按锚点重算镜头；门控失败、停用与 P 变化时只写回视口（镜头中心不变，按 P 重新 clamp）。
            const bool isAnchored = useAnchor && oldTransform.mPhysicalWidth == identity.mPhysicalWidth &&
                oldTransform.mPhysicalHeight == identity.mPhysicalHeight;
            const ZoomPoint anchor = isAnchored ? ReadAnchor(readings.mPhysicalWidth, readings.mPhysicalHeight) : ZoomPoint{};
            WriteViewport(tactical, oldTransform, identity, isAnchored, anchor, oldTacticalPos);
            if (useAnchor)
                AddLog("Battlefield zoom applied 1x");
        }
        mAppliedIndex = IDENTITY_ZOOM_INDEX;
        mTransform = {};
        ReleaseDepthBuffers();
    }

    // 窗口外的完整收尾：退回 1×、释放全部逻辑资源。返回收尾前是否激活（调用方据此请求全重绘）。
    bool ShutDown() noexcept
    {
        const bool wasActive = IsActive();
        FrameReadings readings;
        readings.mPhysicalWidth = DSurface::ViewBounds.Width;
        readings.mPhysicalHeight = DSurface::ViewBounds.Height;
        ExitToIdentity(false, readings);
        ReleaseSurfaces();
        mScaler.Release();
        return wasActive;
    }

    // H1 内异常：不能停在“常驻 L 视口却不进入窗口”的状态（Render 会以 L 画进物理面），立即写回物理视口；资源留给下一次 H1 释放。
    void EmergencyExit() noexcept
    {
        if (mPhase != WindowPhase::Closed)
        {
            RestorePhysicalFrame();
            mPhase = WindowPhase::Closed;
        }
        FrameReadings readings;
        readings.mPhysicalWidth = DSurface::ViewBounds.Width;
        readings.mPhysicalHeight = DSurface::ViewBounds.Height;
        TacticalClass* tactical = TacticalClass::Instance;
        if (tactical && IsViewportResident() && readings.mPhysicalWidth > 0 && readings.mPhysicalHeight > 0)
            SetViewportRect(tactical, readings.mPhysicalWidth, readings.mPhysicalHeight);
        mAppliedIndex = IDENTITY_ZOOM_INDEX;
        mTransform = {};
        Fail(ZoomDisableReason::Exception, "Battlefield zoom disabled after an exception; playback continues");
    }

    bool EnsureSurfaces(LogicalSize capacity) noexcept
    {
        if (IsSurfaceUsable(mLogicalComposite) && IsSurfaceUsable(mLogicalTile) && mSurfaceWidth >= capacity.mWidth &&
            mSurfaceHeight >= capacity.mHeight)
            return true;
        ReleaseSurfaces();
        if (mLogicalComposite || mLogicalTile)
            return false;
        // 与 AllocateRenderSurfaces 0x533FD0 的内存类别规则一致：先按 VideoBackBuffer 选择，两面显存标志不一致时都用 (1, 1) 重建。
        const int requestSystemMemory = IsFlagSet(Game::bVideoBackBuffer) ? 0 : 1;
        DSurface* composite = CreateSurface(capacity.mWidth, capacity.mHeight, requestSystemMemory, 1);
        DSurface* tile = CreateSurface(capacity.mWidth, capacity.mHeight, requestSystemMemory, 1);
        if (composite && tile && IsFlagSet(composite->IsInVideoRam) != IsFlagSet(tile->IsInVideoRam))
        {
            DestroySurface(composite);
            DestroySurface(tile);
            composite = CreateSurface(capacity.mWidth, capacity.mHeight, 1, 1);
            tile = CreateSurface(capacity.mWidth, capacity.mHeight, 1, 1);
        }
        // 必须是 DSurface：RenderRandomizedEnergyArcBetweenCoords 以 IsDSurface 门控一次全局随机数调用。
        if (!IsSurfaceUsable(composite) || !IsSurfaceUsable(tile) || !composite->IsDSurface() || !tile->IsDSurface())
        {
            DestroySurface(composite);
            DestroySurface(tile);
            return false;
        }
        composite->Fill(0);
        tile->Fill(0);
        mLogicalComposite = composite;
        mLogicalTile = tile;
        mSurfaceWidth = capacity.mWidth;
        mSurfaceHeight = capacity.mHeight;
        char line[160];
        std::snprintf(line, sizeof(line), "Battlefield zoom: logical surfaces %dx%d created (videoMemory=%d)",
            capacity.mWidth, capacity.mHeight, IsFlagSet(composite->IsInVideoRam) ? 1 : 0);
        AddLog(line);
        return true;
    }

    // 逻辑面只在窗口外、未加锁且不在任何全局里时销毁；否则只放弃引用（泄漏好过释放仍被引用的面）。
    void ReleaseSurfaces() noexcept
    {
        if (mPhase != WindowPhase::Closed)
            return;
        for (DSurface** slot : {&mLogicalComposite, &mLogicalTile})
        {
            DSurface* surface = std::exchange(*slot, nullptr);
            if (!surface)
                continue;
            const bool isReferenced = surface == DSurface::Composite || surface == DSurface::Tile || surface == DSurface::Temp;
            if (isReferenced || surface->LockLevel != 0)
            {
                AddLog("Battlefield zoom: logical surface still referenced or locked; not destroyed");
                continue;
            }
            DestroySurface(surface);
        }
        mSurfaceWidth = 0;
        mSurfaceHeight = 0;
    }

    bool EnsureDepthBuffers(LogicalSize size) noexcept
    {
        if (mLogicalZBuffer && mLogicalABuffer && mDepthWidth == size.mWidth && mDepthHeight == size.mHeight)
            return true;
        ReleaseDepthBuffers();
        if (mLogicalZBuffer || mLogicalABuffer)
            return false;
        // 原生构造不处理分配失败：先用游戏分配器试探两块缓冲，成功后立即释放，再构造。
        const std::size_t bufferBytes = 2u * static_cast<std::size_t>(size.mWidth) * static_cast<std::size_t>(size.mHeight);
        void* zProbe = YRMemory::Allocate(bufferBytes);
        void* aProbe = YRMemory::Allocate(bufferBytes);
        const bool isProbeAllocated = zProbe && aProbe;
        if (aProbe)
            YRMemory::Deallocate(aProbe);
        if (zProbe)
            YRMemory::Deallocate(zProbe);
        if (!isProbeAllocated)
            return false;
        void* zStorage = YRMemory::Allocate(target::DEPTH_BUFFER_OBJECT_BYTES);
        void* aStorage = YRMemory::Allocate(target::DEPTH_BUFFER_OBJECT_BYTES);
        if (!zStorage || !aStorage)
        {
            if (aStorage)
                YRMemory::Deallocate(aStorage);
            if (zStorage)
                YRMemory::Deallocate(zStorage);
            return false;
        }
        // Bounds 原点为 0：Blitter 以 y − Bounds.Y 寻址，不使用 Bounds.X；宽高必须恰为 L（环形缓冲按自身宽高填充暴露带）。
        const RectangleStruct bounds{0, 0, size.mWidth, size.mHeight};
        mLogicalZBuffer = new (zStorage) ZBuffer(bounds);
        mLogicalABuffer = new (aStorage) ABuffer(bounds);
        mDepthWidth = size.mWidth;
        mDepthHeight = size.mHeight;
        mWasDepthSwappedIn = false;
        return true;
    }

    void ReleaseDepthBuffers() noexcept
    {
        if (mPhase != WindowPhase::Closed || (!mLogicalZBuffer && !mLogicalABuffer))
            return;
        // 鼠标线程可能在上一帧窗口期取到逻辑 Z/A 并仍在 ShapeBlit 中使用；HideCursor 取得同一互斥后它不可能还在绘制
        // （与原生 Set_View_Dimensions 0x4A89CA/0x4A8B0B 的做法相同）。
        WWMouseClass* mouse = mWasDepthSwappedIn ? WWMouseClass::Instance : nullptr;
        if (mouse)
            mouse->HideCursor();
        if (ZBuffer* zBuffer = std::exchange(mLogicalZBuffer, nullptr))
        {
            zBuffer->ReleaseSurface();
            YRMemory::Deallocate(zBuffer);
        }
        if (ABuffer* aBuffer = std::exchange(mLogicalABuffer, nullptr))
        {
            aBuffer->ReleaseSurface();
            YRMemory::Deallocate(aBuffer);
        }
        if (mouse)
            mouse->ShowCursor();
        mDepthWidth = 0;
        mDepthHeight = 0;
        mWasDepthSwappedIn = false;
    }

    void CapturePhysicalFrame(DSurface* frameSurface) noexcept
    {
        mFrame.mComposite = frameSurface;
        mFrame.mTile = DSurface::Tile;
        mFrame.mTemp = DSurface::Temp;
        mFrame.mZBuffer = ZBuffer::Instance;
        mFrame.mABuffer = ABuffer::Instance;
        mFrame.mViewWidth = DSurface::ViewBounds.Width;
        mFrame.mViewHeight = DSurface::ViewBounds.Height;
    }

    void SwapInLogicalFrame() noexcept
    {
        DSurface::Composite = mLogicalComposite;
        DSurface::Tile = mLogicalTile;
        DSurface::Temp = mLogicalComposite;
        ZBuffer::Instance = mLogicalZBuffer;
        ABuffer::Instance = mLogicalABuffer;
        DSurface::ViewBounds.Width = mTransform.mLogicalWidth;
        DSurface::ViewBounds.Height = mTransform.mLogicalHeight;
        mWasDepthSwappedIn = true;
    }

    void RestorePhysicalFrame() noexcept
    {
        DSurface::Composite = mFrame.mComposite;
        DSurface::Tile = mFrame.mTile;
        DSurface::Temp = mFrame.mTemp;
        ZBuffer::Instance = mFrame.mZBuffer;
        ABuffer::Instance = mFrame.mABuffer;
        DSurface::ViewBounds.Width = mFrame.mViewWidth;
        DSurface::ViewBounds.Height = mFrame.mViewHeight;
    }

    void EnterWindow() noexcept
    {
        CapturePhysicalFrame(DSurface::Composite);
        SwapInLogicalFrame();
        mPhase = WindowPhase::World;
        ++mCounters.mEnteredFrames;
    }

    // 全局里的 Composite/Tile 必须仍是逻辑对（顺序可能已被 Render(0) 交换）；是则以全局为准更新。
    bool TakeLogicalPairFromGlobals() noexcept
    {
        DSurface* composite = DSurface::Composite;
        DSurface* tile = DSurface::Tile;
        const bool isSame = composite == mLogicalComposite && tile == mLogicalTile;
        const bool isSwapped = composite == mLogicalTile && tile == mLogicalComposite;
        if (!isSame && !isSwapped)
            return false;
        mLogicalComposite = composite;
        mLogicalTile = tile;
        return true;
    }

    // H4 缩放：先锁 Pframe 再锁逻辑面，失败时只解锁已锁上的；行距在 Lock 成功后读取。
    // 只写 Pframe 的 [0,Pw)×[0,Ph)，不碰底部 32 行命令条。
    void ScaleToFrame(DSurface* source) noexcept
    {
        DSurface* target = mFrame.mComposite;
        const int physicalWidth = mTransform.mPhysicalWidth;
        const int physicalHeight = mTransform.mPhysicalHeight;
        const int logicalWidth = mTransform.mLogicalWidth;
        const int logicalHeight = mTransform.mLogicalHeight;
        const int rgbMode = GetRgbMode();
        const std::optional<PixelFormat16> format = ToPixelFormat(rgbMode);
        if (!format || !target || target == source || physicalWidth != mFrame.mViewWidth ||
            physicalHeight != mFrame.mViewHeight || source->GetBytesPerPixel() != 2 || target->GetBytesPerPixel() != 2 ||
            target->GetWidth() < physicalWidth || target->GetHeight() < physicalHeight ||
            source->GetWidth() < logicalWidth || source->GetHeight() < logicalHeight)
        {
            Fail(ZoomDisableReason::ScaleFailed, "Battlefield zoom: frame surfaces or pixel format no longer match; zoom disabled");
            return;
        }
        if (target->IsLocked() || source->IsLocked())
        {
            if (mCounters.mPreLockedFrames++ < MAX_REPEATED_LOG_LINES)
                AddLog("Battlefield zoom: a frame surface was already locked before scaling");
        }
        void* targetPixels = target->Lock(0, 0);
        if (!targetPixels)
        {
            ++mCounters.mLockFailures;
            return;
        }
        const void* sourcePixels = source->Lock(0, 0);
        if (!sourcePixels)
        {
            target->Unlock();
            ++mCounters.mLockFailures;
            return;
        }
        const int targetPitch = target->GetPitch();
        const int sourcePitch = source->GetPitch();
        if (!mHasLoggedPixelFormat)
        {
            mHasLoggedPixelFormat = true;
            LogPixelFormat(rgbMode, target, source, targetPitch, sourcePitch);
        }
        const ConstPixelView16 sourceView{sourcePixels, logicalWidth, logicalHeight, sourcePitch};
        const PixelView16 targetView{targetPixels, physicalWidth, physicalHeight, targetPitch};
        const bool isScaled = mScaler.Scale(sourceView, targetView, mTransform, *format);
        source->Unlock();
        target->Unlock();
        if (isScaled)
            ++mCounters.mScaledFrames;
        else
            Fail(ZoomDisableReason::ScaleFailed, "Battlefield zoom: scaler rejected the frame; zoom disabled");
    }

    // 供实机确认像素格式：GetRGBMode 与两面的 DDSURFACEDESC 位数、R/G/B 掩码与行距。
    void LogPixelFormat(int rgbMode, const DSurface* target, const DSurface* source, int targetPitch, int sourcePitch) noexcept
    {
        const auto describe = [](const DSurface* surface, char* text, std::size_t size) noexcept {
            const DDSURFACEDESC2* description = surface->VideoSurfaceDescription;
            if (!description)
            {
                std::snprintf(text, size, "desc=null");
                return;
            }
            const DDPIXELFORMAT& format = description->ddpfPixelFormat;
            std::snprintf(text, size, "bits=%lu masks=%04lX/%04lX/%04lX", static_cast<unsigned long>(format.dwRGBBitCount),
                static_cast<unsigned long>(format.dwRBitMask), static_cast<unsigned long>(format.dwGBitMask),
                static_cast<unsigned long>(format.dwBBitMask));
        };
        char targetText[96];
        char sourceText[96];
        describe(target, targetText, sizeof(targetText));
        describe(source, sourceText, sizeof(sourceText));
        char line[384];
        std::snprintf(line, sizeof(line), "Battlefield zoom pixel format: rgbMode=%d frame{%s pitch=%d} logical{%s pitch=%d}",
            rgbMode, targetText, targetPitch, sourceText, sourcePitch);
        AddLog(line);
    }

    // 生命周期
    bool mIsEnabled = false;
    bool mIsDisablePending = false;
    bool mIsFullRedrawPending = false;
    bool mHasSessionStarted = false;
    ZoomDisableReason mDisableReason = ZoomDisableReason::NotEnabled;
    // 最近一次读取门控的 H1 是否发现门控关闭；只供 GetStatus。
    bool mIsGateClosed = false;
    // 档位
    ZoomLevelPolicy mPolicy{0, 0};
    int mPolicyWidth = 0;
    int mPolicyHeight = 0;
    int mPolicyLimitIndex = -1;
    int mFarthestLimitIndex = rareplay::zoom::FARTHEST_SHRINK_ZOOM_INDEX;
    int mAppliedIndex = IDENTITY_ZOOM_INDEX;
    int mRequestedIndex = IDENTITY_ZOOM_INDEX;
    ZoomStepAnchor mRequestAnchor = ZoomStepAnchor::Center;
    // 已应用档位的映射（P, L）；未激活时为空（不合法）。
    ZoomTransform mTransform;
    // 逻辑资源：Composite/Tile 按允许的最远档一次分配（容量不小于 L），Z/A 恰为当前 L。
    DSurface* mLogicalComposite = nullptr;
    DSurface* mLogicalTile = nullptr;
    int mSurfaceWidth = 0;
    int mSurfaceHeight = 0;
    ZBuffer* mLogicalZBuffer = nullptr;
    ABuffer* mLogicalABuffer = nullptr;
    int mDepthWidth = 0;
    int mDepthHeight = 0;
    bool mWasDepthSwappedIn = false;
    // 窗口
    WindowPhase mPhase = WindowPhase::Closed;
    PhysicalFrame mFrame;
    BattlefieldScaler mScaler;
    // 雷达视框 RF1 保存的 g_ViewBounds 宽高
    bool mIsRadarFrameSwapped = false;
    int mRadarSavedWidth = 0;
    int mRadarSavedHeight = 0;
    // 三个输入入口各自的逻辑坐标副本：原生只在本次调用内读取，各入口不重入。
    Point2D mClickPoint{0, 0};
    Point2D mDragBandStartPoint{0, 0};
    Point2D mDragBandEndPoint{0, 0};
    // 诊断
    Counters mCounters;
    std::uint32_t mGateLogCount = 0;
    bool mHasLoggedPixelFormat = false;
    std::vector<std::string> mLogLines;
};

// 进程级单例，只在游戏线程访问。
BattlefieldZoomAdapter g_Adapter;

// 插件接口里的异常（日志字符串分配等）只停用缩放；窗口内不会调用这些接口。
template <typename TAction>
void RunGuarded(TAction action) noexcept
{
    try
    {
        action();
    }
    catch (...)
    {
        g_Adapter.Fail(ZoomDisableReason::Exception, "Battlefield zoom disabled after an exception; playback continues");
    }
}
}

void Enable() noexcept
{
    RunGuarded([] { g_Adapter.Enable(); });
}

void Disable() noexcept
{
    RunGuarded([] { g_Adapter.Disable(); });
}

void RequestStep(int steps, ZoomStepAnchor anchor) noexcept
{
    RunGuarded([steps, anchor] { g_Adapter.RequestStep(steps, anchor); });
}

void RequestReset() noexcept
{
    RunGuarded([] { g_Adapter.RequestReset(); });
}

BattlefieldZoomStatus GetStatus() noexcept
{
    return g_Adapter.GetStatus();
}

std::vector<std::string> TakeLogLines() noexcept
{
    return g_Adapter.TakeLogLines();
}

rareplay::zoom::ZoomPoint MapViewPointToPhysical(rareplay::zoom::ZoomPoint logicalPoint) noexcept
{
    return g_Adapter.MapViewPointToPhysical(logicalPoint);
}

bool OnDrawEnter() noexcept
{
    return g_Adapter.OnDrawEnter();
}

void OnTileExposed(bool isRedrawRequested) noexcept
{
    g_Adapter.OnTileExposed(isRedrawRequested);
}

void OnTabDrawBegin() noexcept
{
    g_Adapter.OnTabDrawBegin();
}

void OnTabDrawEnd() noexcept
{
    g_Adapter.OnTabDrawEnd();
}

void OnWorldLayerDrawn(const void* renderSurface) noexcept
{
    g_Adapter.FinishWorldLayer(renderSurface, false);
}

void OnDrawTail() noexcept
{
    g_Adapter.FinishWorldLayer(nullptr, true);
}

std::optional<int> GetSimulationViewportWidth() noexcept
{
    return g_Adapter.GetPhysicalViewWidth();
}

std::optional<int> GetSimulationViewportHeight() noexcept
{
    return g_Adapter.GetPhysicalViewHeight();
}

std::optional<int> GetVisibleCellViewWidth() noexcept
{
    return g_Adapter.GetViewportWidth();
}

std::optional<int> GetVisibleCellViewHeight() noexcept
{
    return g_Adapter.GetViewportHeight();
}

std::optional<int> GetCameraClampWidth() noexcept
{
    return g_Adapter.GetCameraClampWidth();
}

std::optional<int> GetCameraClampHeight() noexcept
{
    return g_Adapter.GetCameraClampHeight();
}

void OnRadarFrameBegin() noexcept
{
    g_Adapter.OnRadarFrameBegin();
}

void OnRadarFrameEnd() noexcept
{
    g_Adapter.RestoreRadarFrame();
}

void AdjustSoundFieldHalfExtents(void* halfWidthSlot, void* halfHeightSlot) noexcept
{
    g_Adapter.AdjustSoundFieldHalfExtents(halfWidthSlot, halfHeightSlot);
}

const void* MapClickPoint(const void* physicalPoint) noexcept
{
    return g_Adapter.MapClickPoint(physicalPoint);
}

const void* MapDragBandStart(const void* physicalPoint) noexcept
{
    return g_Adapter.MapDragBandStart(physicalPoint);
}

const void* MapDragBandEnd(const void* physicalPoint) noexcept
{
    return g_Adapter.MapDragBandEnd(physicalPoint);
}
}
