#pragma once

#include "ReplayHudEconomyModel.h"
#include "ReplayHudRadarModel.h"
#include "ReplayHudSeekModel.h"
#include "ReplayHudZoomModel.h"
#include "../viewer/ReplayViewModel.h"
#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rareplay::viewer
{
class ReplayViewExchange;
}

namespace rareplay::ui
{
struct UiRect
{
    float mLeft = 0.0f;
    float mTop = 0.0f;
    float mRight = 0.0f;
    float mBottom = 0.0f;
};

/** 底栏战局缩放控件本次绘制的位置（主视口客户区坐标），供测试与预览核对。 */
struct ZoomControlRects
{
    ZoomControlMode mMode = ZoomControlMode::Hidden;
    /** 整组（文字与按钮）的外接矩形。 */
    UiRect mGroup;
    /** 三个按钮；只在 Controls 模式下有效。 */
    UiRect mZoomOut;
    UiRect mZoomIn;
    UiRect mReset;
};

/** 底栏进度条与快进控件本次绘制的位置（主视口客户区坐标），供测试与预览核对。 */
struct SeekBarRects
{
    /** 进度条的可点区域（只读时为进度条本身）；总帧数未知、或窄布局下只读时不绘制，为空。 */
    std::optional<UiRect> mBar;
    /** 进度条可交互。 */
    bool mIsInteractive = false;
    /** 进度条是否单独占一行（窄布局）。 */
    bool mIsOnOwnRow = false;
    /** 快进中的“取消”按钮；不在快进中时为空。 */
    std::optional<UiRect> mCancelButton;
};

/** 进度条拖动的界面本地状态，只在渲染线程使用；会话变化或 Reset 时清空。 */
struct SeekBarState
{
    /** 鼠标在可跳转的位置按下、尚未松开。在不可点的位置按下时为假，之后拖到哪里松开都不提交。 */
    bool mIsDragging = false;
};

/** 宿主观察到的游戏图像在主视口中的位置。 */
struct GameImagePlacement
{
    /** 主视口客户区坐标；空矩形表示未知。 */
    UiRect mRect;
    /**
     * true 表示宿主确认 mRect 正是游戏图像所在区域（cnc-ddraw OpenGL 以最终 glViewport 放置图像）；
     * false 时只作保守参考，例如 cnc-ddraw D3D9 用顶点而非视口做等比留边。
     */
    bool mIsImageRect = false;
};

enum class ReplayPanel { Overview, Production, Units, Groups, Economy };

/**
 * 只读回放快照的 ImGui 表示层。宿主拥有实例、纹理和 ImGui 帧生命周期；仅在渲染线程调用。
 * 选择玩家仅改变本界面的信息，不能授予该玩家的游戏控制权或地图视野。
 */
class ReplayHud final
{
public:
    using IconResolver = std::function<ImTextureID(const std::shared_ptr<const viewer::IconImage>&)>;

    /** Draw 要在 ImGui::NewFrame 之后调用；速度请求通过 exchange 送回游戏线程。 */
    void Draw(const viewer::ReplayViewSnapshot& snapshot, viewer::ReplayViewExchange& exchange);
    void Reset();

    /** 回调仅解析已上传纹理；空回调或空纹理会显示中性占位。 */
    void SetIconResolver(IconResolver resolver);

    /** 选择下一帧显示的面板标签，仅修改 UI 本地状态。 */
    void SelectPanel(ReplayPanel panel);

    /** 选择部队页的类别子页签；空表示“全部”。仅修改 UI 本地状态，会话变化或 Reset 时回到“全部”。 */
    void SelectUnitCategory(std::optional<viewer::UnitCategory> category) noexcept
    {
        mSelectedUnitCategory = category;
        mSelectUnitCategoryPending = true;
    }

    /**
     * 打开或关闭经济曲线大窗口，仅修改 UI 本地状态。F10 隐藏 HUD 期间不调用 Draw，状态原样保留，
     * 再次显示时窗口恢复为隐藏前的开合；会话变化或 Reset 时回到关闭。
     */
    void SetEconomyWindowOpen(bool isOpen) noexcept;

    /** 选择经济曲线大窗口的时间范围，仅修改 UI 本地状态。 */
    void SelectEconomyRange(EconomyTimeRange range) noexcept;

    /**
     * 在下一次 Draw 时只显示该玩家的曲线（该玩家没有曲线时改为显示全部），仅修改 UI 本地状态；
     * 与大窗口的“只看”按钮同一状态。
     */
    void ShowOnlyEconomyPlayer(int houseIndex) noexcept;

    /**
     * 折叠或展开雷达事件提示栏、切换最近/历史模式，仅修改 UI 本地状态；会话变化或 Reset 时回到“展开、最近”，
     * 首次 Draw 前预置的值不会被会话开始清空（与 SelectPanel 同理）。供预览与测试使用。
     */
    void SetRadarBarCollapsed(bool isCollapsed) noexcept;
    void SetRadarBarHistory(bool showHistory) noexcept;

    const RadarBarState& GetRadarBarState() const noexcept { return mRadar; }

    /** 本次 Draw 中雷达事件提示栏的矩形（主视口客户区坐标）；快照没有事件列表或未绘制时为空。 */
    const std::optional<UiRect>& GetRadarBarRect() const noexcept { return mRadarBarRect; }

    /** 本次 Draw 中底栏战局缩放控件的位置；控件不显示（Hidden）或底栏未绘制时为空。 */
    const std::optional<ZoomControlRects>& GetZoomControlRects() const noexcept { return mZoomControlRects; }

    /** 本次 Draw 中底栏进度条与“取消”按钮的位置；底栏未绘制时两者都为空。 */
    const SeekBarRects& GetSeekBarRects() const noexcept { return mSeekBarRects; }

    /** 本次 Draw 实际绘制的超武落点图标数（屏幕外与已淡出的不计）。 */
    std::size_t GetDrawnMarkCount() const noexcept { return mDrawnMarkCount; }

    /** 侧栏小图与大窗口共用的界面本地状态，供测试与预览核对。 */
    const EconomyUiState& GetEconomyState() const noexcept { return mEconomy; }

    /** 本次 Draw 中经济大窗口的矩形（主视口客户区坐标）；窗口关闭或未绘制时为空。 */
    const std::optional<UiRect>& GetEconomyWindowRect() const noexcept { return mEconomyWindowRect; }

    /** 每帧在 Draw 前设置；未设置时按整个视口及等比留边两种可能保守换算。 */
    void SetGameImagePlacement(GameImagePlacement placement) noexcept { mGameImage = placement; }

    /**
     * 宿主已按此百分比（ResolveHudScalePercent 的结果）重建字体后，在 Draw 前设置。
     * 只影响 HUD 的样式尺寸、固定尺寸常量与侧栏面板宽度；字体由宿主负责。未设置时为 100%。
     * Draw 会按此比例重设当前 ImGui context 的基础样式，宿主不应依赖该 context 的样式。
     */
    void SetScalePercent(int percent) noexcept;

    /** 返回本次 Draw 中可交互窗口的主视口绘制坐标，供宿主做输入路由。 */
    const std::vector<UiRect>& GetInputRegions() const noexcept;

    /** 返回本次 Draw 中可能遮住原生光标的可见窗口，包含只读 FPS 卡。 */
    const std::vector<UiRect>& GetVisibleRegions() const noexcept;

    /**
     * 本次 Draw 是否绘制了侧栏面板，且其位置唯一确定、不透明并完整覆盖原生侧栏区。
     * 宿主只在为真且该帧确实呈现后报告面板心跳；为假时游戏必须继续绘制原生侧栏。
     */
    bool IsSidebarPanelCovering() const noexcept { return mIsSidebarPanelCovering; }

private:
    /** rect 为主视口绘制坐标；面板在其中不透明铺满。 */
    void DrawSidebarPanel(const viewer::ReplayViewSnapshot& snapshot, const UiRect& rect,
                          viewer::ReplayViewExchange& exchange);
    void ApplyStyleScale();

    std::uint64_t mSessionId = 0;
    int mSelectedHouseIndex = -1;
    IconResolver mIconResolver;
    std::vector<UiRect> mInputRegions;
    std::vector<UiRect> mVisibleRegions;
    ReplayPanel mSelectedPanel = ReplayPanel::Overview;
    bool mSelectPanelPending = false;
    std::optional<viewer::UnitCategory> mSelectedUnitCategory;
    bool mSelectUnitCategoryPending = false;
    EconomyUiState mEconomy;
    // 经济状态由预览或测试在首次 Draw 前预置时，会话变化不清空它（与 SelectPanel 同理）。
    bool mEconomyPresetPending = false;
    std::optional<int> mPendingEconomyOnly;
    std::optional<UiRect> mEconomyWindowRect;
    RadarBarState mRadar;
    // 提示栏状态由预览或测试在首次 Draw 前预置时，会话变化不清空它。
    bool mRadarPresetPending = false;
    std::optional<UiRect> mRadarBarRect;
    std::optional<ZoomControlRects> mZoomControlRects;
    SeekBarRects mSeekBarRects;
    SeekBarState mSeekBar;
    std::size_t mDrawnMarkCount = 0;
    GameImagePlacement mGameImage;
    bool mIsSidebarPanelCovering = false;
    int mScalePercent = 100;
    float mScale = 1.0f;
    // 当前 context 基础样式已按哪个比例缩放；0 表示需要重设。
    float mAppliedStyleScale = 0.0f;
};
}
