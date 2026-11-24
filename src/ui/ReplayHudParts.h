#pragma once

#include "ReplayHud.h"
#include "ReplayHudEconomyModel.h"
#include "ReplayHudLayout.h"
#include "ReplayHudMarkModel.h"
#include "ReplayHudRadarModel.h"

#include <optional>
#include <string>
#include <vector>

namespace rareplay::ui::detail
{
/** HUD 缩放（1 为 100%）：字号 ÷ 基准字号 18；宿主按档位重建字体，所以字号已含缩放。 */
float GetHudUnit();

/** scale 为 HUD 缩放，用于固定尺寸的图表与留白。 */
void DrawFpsCard(const viewer::ReplayViewSnapshot& snapshot, float scale);
/** 总览页：回放信息与全部玩家的经济与统计摘要；点击某行即选择观看信息的玩家。 */
void DrawOverview(const viewer::ReplayViewSnapshot& snapshot, int& selectedHouseIndex);
/** 面板顶部的游戏时间；名义时间附“（名义）”，缺失显示“—”。 */
void DrawGameTimeHeader(const viewer::ReplayViewSnapshot& snapshot);
/**
 * 生产页：兵营与重工两条线的当前状态与停产汇总（总停产时间、停产率）、停产历史（最新在上，可滚动），
 * 其下是原有的产线列表。snapshot 提供当前游戏时间，用来显示进行中的停产已持续多久。
 */
void DrawProductionLines(const viewer::ReplayViewSnapshot& snapshot, const viewer::PlayerSummary* player,
                         const ReplayHud::IconResolver& iconResolver);
/**
 * 部队页：顶部类别子页签（全部与各类，显示该类总数），下方每行最多两张卡片的独立滚动列表。
 * selectedCategory 为空表示“全部”；它是 UI 本地状态，由 ReplayHud 在本次会话内保持。
 */
void DrawUnitInventory(const viewer::PlayerSummary* player, const ReplayHud::IconResolver& iconResolver,
                       std::optional<viewer::UnitCategory>& selectedCategory);
void DrawRecordedGroups(const viewer::ReplayViewSnapshot& snapshot);

/**
 * 经济页：分区标题、“展开大图”开关按钮、当前筛选提示，其下是侧栏小图（全部时间范围，只画可见玩家）
 * 与玩家图例。点击图例选择观看信息的玩家；筛选状态与大窗口共用 state。
 */
void DrawEconomyChart(const viewer::ReplayViewSnapshot& snapshot, int& selectedHouseIndex, EconomyUiState& state);

/**
 * 经济大窗口（资金曲线）：state.mWindow.mIsOpen 为假时不绘制。窗口位置按 layout 与拖动状态确定，
 * 绘制时把窗口矩形追加到 inputRegions（拦截输入）；origin 为主视口工作区原点。
 * 返回窗口矩形（客户区坐标）；未绘制时为空。
 */
std::optional<UiRect> DrawEconomyWindow(const viewer::ReplayViewSnapshot& snapshot, int selectedHouseIndex,
                                        EconomyUiState& state, const EconomyWindowLayoutInput& layout,
                                        const ImVec2& origin, std::vector<UiRect>& inputRegions);

/** 颜色的不透明度乘以 opacity（0 到 1）；绘制列表只接受已含透明度的 ImU32。 */
inline ImU32 WithOpacity(ImU32 color, float opacity)
{
    const float alpha = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * (opacity < 0.0f ? 0.0f : opacity > 1.0f ? 1.0f : opacity);
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
}

/**
 * 在背景绘制层（所有 HUD 窗口之下）绘制超武落点图标；位置来自游戏线程每个主循环迭代发布的落点通道，
 * 不是约 100 ms 一次的快照。落点在战术视图之外时不画，图标被裁剪在战术视图内。
 * origin 与 extent 为主视口工作区的起点与尺寸，scale 为 HUD 缩放。返回实际绘制的落点数。
 */
std::size_t DrawSuperWeaponMarks(const viewer::ReplayViewSnapshot& snapshot, viewer::ReplayViewExchange& exchange,
                                 const GameImagePlacement& image, const ImVec2& origin, const ImVec2& extent,
                                 float scale, const ReplayHud::IconResolver& iconResolver);

/** 通用的超武标记（圆环加准星），没有超武图标时用；矩形为屏幕坐标，thickness 为线宽。 */
void DrawSuperWeaponGlyph(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 color, float thickness);

/**
 * 雷达事件提示栏：左侧中部的小窗口，标题行带折叠按钮与“历史”开关；默认只显示最近 5 条（30 秒游戏时间，末段淡出，
 * 悬停暂停淡出），历史模式显示可滚动的全部事件（至多 200 条）。点击带位置的事件向游戏线程提交“镜头居中”命令。
 * 快照没有事件列表（功能未启用）时不绘制并返回空；否则窗口矩形（客户区坐标，含标题行）整块加入 inputRegions。
 * layout 提供画面、地图区与缩放，其中折叠、历史与条数由 state 与事件决定。
 */
std::optional<UiRect> DrawRadarBar(const viewer::ReplayViewSnapshot& snapshot, RadarBarState& state,
                                   const RadarBarLayoutInput& layout, const ImVec2& origin,
                                   const ReplayHud::IconResolver& iconResolver, viewer::ReplayViewExchange& exchange,
                                   std::vector<UiRect>& inputRegions);

/** 有资金曲线的玩家的房屋下标，按快照中的玩家顺序；筛选的候选列表。 */
std::vector<int> CollectEconomyHouseIndices(const viewer::ReplayViewSnapshot& snapshot);

/** 一个“标签 数值”小块；mHasCheck 时数值前画一个对勾（字体里没有对勾字形，用线条绘制）。 */
struct StatChip
{
    std::string mLabel;
    std::string mValue;
    ImU32 mValueColor = IM_COL32(236, 235, 219, 255);
    bool mHasCheck = false;
};
/** 按当前字体测量并排版小块，返回占用的总高度；availableWidth 为排版区域宽度。 */
float MeasureStatChips(const std::vector<StatChip>& chips, float availableWidth);
/** 在 origin 起、宽 availableWidth 的区域内逐行绘制小块，超出 clip 的文字被裁剪；返回占用的总高度。 */
float DrawStatChips(const std::vector<StatChip>& chips, const ImVec2& origin, float availableWidth,
                    const ImVec4& clip);
/** 总览卡片里的统计小块：部队、建造、摧毁、损失、战损比、分数、操作、APM、兵营升级、重工升级。 */
std::vector<StatChip> BuildPlayerStatChips(const viewer::PlayerSummary& player);
/** 统计的完整说明（悬停提示用），没有统计时为空。 */
std::string BuildPlayerStatTooltip(const viewer::PlayerSummary& player);
/** 底栏内容区宽度小于“730 × 缩放”时用窄布局：进度条不与帧号、速度同行。 */
bool IsFooterCompact(float contentWidth, float scale) noexcept;

/** 窄布局下可交互的进度条单独占一行，底栏要为它加高一行（只读进度条在窄布局下不画，与原来相同）。 */
inline bool NeedsSeekBarRow(const SeekBarModel& model, bool isCompact) noexcept
{
    return isCompact && model.mMode == SeekBarMode::Interactive;
}

struct FrameControlRects
{
    /** DrawBattlefieldZoomControls 的结果。 */
    std::optional<ZoomControlRects> mZoom;
    SeekBarRects mSeek;
};

/**
 * 底栏内容：第一行暂停/重新播放/退出；第二行帧号（快进中为状态文字与“取消”）、进度条与速度；
 * 第三行半透明黑幕开关及其后的战局缩放控件。窄布局（isCompact，由宿主按 IsFooterCompact 判定并据此加高底栏）下
 * 可交互的进度条在第二行之后单独占一行。进度条在松开时才提交一次跳转命令；快进中禁用暂停、速度、半黑幕与缩放。
 * seekModel 由 BuildSeekBarModel(snapshot) 得出；seekBar 是进度条拖动的界面本地状态。
 */
FrameControlRects DrawFrameControls(const viewer::ReplayViewSnapshot& snapshot, const SeekBarModel& seekModel,
                                    viewer::ReplayViewExchange& exchange, std::vector<UiRect>& inputRegions,
                                    SeekBarState& seekBar, bool isCompact, float scale);
/**
 * 战局缩放控件，接在当前行上一个控件之后（SameLine）。Controls 模式为“缩放 N%”与 －、＋、复原 三个小按钮，
 * 各提交一条观看命令（以战术区中心为锚点的 ±1 档，或复原），文字占固定宽度，换档时按钮不移动；Unavailable 模式为一行灰色说明；
 * Hidden 时不绘制。同一行放不下完整文字时改用短文字（完整文字移入悬停提示），仍放不下时与底栏其他控件一样被窗口裁剪。
 * 快进中按钮全部不可用。返回本次绘制的矩形（主视口客户区坐标）；不绘制时为空。
 */
std::optional<ZoomControlRects> DrawBattlefieldZoomControls(const viewer::ReplayViewSnapshot& snapshot,
                                                            viewer::ReplayViewExchange& exchange, float scale);
/**
 * “界面缩放”下拉框：显示当前设置，自动档附注实际百分比；选择后只向 exchange 提交命令，
 * 由游戏线程保存并在下一份快照生效。打开的下拉窗口记入 inputRegions。
 */
void DrawHudScaleCombo(const viewer::ReplayViewSnapshot& snapshot, int effectivePercent,
                       viewer::ReplayViewExchange& exchange, std::vector<UiRect>& inputRegions);
/** 六个档位的可选项列表，供下拉框与齿轮弹窗共用。 */
void DrawHudScaleOptions(const viewer::ReplayViewSnapshot& snapshot, int effectivePercent,
                         viewer::ReplayViewExchange& exchange);
/** 设置当前（自动档附实际百分比）的显示文字，写入 buffer。 */
const char* FormatHudScaleLabel(viewer::HudScaleSetting setting, int effectivePercent, char* buffer, std::size_t size);
UiRect DrawIcon(const std::shared_ptr<const viewer::IconImage>& icon,
              const ReplayHud::IconResolver& iconResolver, const ImVec2& size);
/**
 * 按当前窗口内容区换行、比内容区窄 1 像素的文字。ImGui 1.89 的折行结果在宽度向上取整后可能比
 * 换行宽度多出 1 像素，面板内的换行文字统一用它，保证不越出面板。
 */
void TextWrappedFit(const char* format, ...) IM_FMTARGS(1);
/** 灰色的 TextWrappedFit；窄面板中替代不换行的 TextDisabled。 */
void TextWrappedDisabled(const char* format, ...) IM_FMTARGS(1);
ImVec4 GetPlayerColor(std::uint32_t rgb);
const viewer::PlayerSummary* FindPlayer(const viewer::ReplayViewSnapshot& snapshot, int houseIndex);

// ---- 卡片样式（所有页面共用，参数与颜色见 ReplayHudCard.cpp）----

enum class CardState { Normal, Hovered, Selected };

/** 侧栏面板、底栏与大窗口的不透明背景色（与 0.090、0.106、0.098 的样式色相同）；卡片相对它拉开明度差。 */
constexpr ImU32 PANEL_BACKGROUND_COLOR = IM_COL32(23, 27, 25, 255);

struct CardColors
{
    ImU32 mFill = 0;
    ImU32 mBorder = 0;
};
/** 各状态下卡片的填充与边框色；公开它们是为了让测试核对与面板背景的明度差。 */
CardColors GetCardColors(CardState state);

/** 卡片几何，均按 HUD 缩放换算成整数像素（边框与圆角随缩放，100% 档边框 1 像素）。 */
struct CardMetrics
{
    /** 相邻卡片之间的水平与垂直间距。 */
    float mGap = 6.0f;
    /** 卡片内边距。 */
    float mPadding = 5.0f;
    /** 边框宽度；选中卡片为其两倍。 */
    float mBorder = 1.0f;
    float mRounding = 3.0f;
};
CardMetrics GetCardMetrics();

/** 选中为 Selected，否则按上一个控件是否被悬停给出 Hovered 或 Normal；用于紧跟 InvisibleButton 之后。 */
CardState GetCardState(bool isSelected);

/** 画卡片背景与浅色边框，min/max 为屏幕坐标；背景与面板背景有明显明度差，选中时琥珀色高亮。 */
void DrawCardBackground(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, CardState state);

/** 作用域内把项目间距设为卡片间距（水平与垂直），使一列或一网格卡片之间留出统一空隙。 */
class CardListSpacing final
{
public:
    CardListSpacing();
    ~CardListSpacing();
    CardListSpacing(const CardListSpacing&) = delete;
    CardListSpacing& operator=(const CardListSpacing&) = delete;
};

/**
 * 内容为普通控件的卡片：构造时记下起点并把光标移入内边距，析构时按内容高度补画背景并让布局前进。
 * 作用域内的 ImGui::Text 在卡片内边距处换行；需要宽度时用 GetInnerWidth。不能嵌套，也不要在作用域内开子窗口。
 */
class CardScope final
{
public:
    explicit CardScope(bool isSelected = false);
    ~CardScope();
    CardScope(const CardScope&) = delete;
    CardScope& operator=(const CardScope&) = delete;

    /** 内容区宽度（卡片宽度扣除两侧内边距）。 */
    float GetInnerWidth() const noexcept;

private:
    ImDrawList* mDrawList = nullptr;
    ImVec2 mStart;
    float mWidth = 1.0f;
    float mPadding = 0.0f;
    bool mIsSelected = false;
};

/** 分区标题：强调色文字，下方一条与内容区等宽的分隔线。 */
void DrawSectionTitle(const char* format, ...) IM_FMTARGS(1);
}
