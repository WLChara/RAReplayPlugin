#pragma once

#include "ReplayHudParts.h"

#include <cstdint>
#include <optional>
#include <vector>

/**
 * 经济曲线图表的共用绘制：侧栏小图与大窗口使用同一套坐标轴、网格、曲线与悬停读数，
 * 只在尺寸、线宽与时间范围上不同。只在渲染线程、ImGui 帧内调用。
 */
namespace rareplay::ui::detail
{
/** 图表中的一条曲线；mIsVisible 为假的曲线仍出现在图例中，但不绘制，也不参与纵轴缩放。 */
struct ChartSeries
{
    const viewer::PlayerSummary* mPlayer = nullptr;
    const viewer::EconomyCurve* mCurve = nullptr;
    bool mIsVisible = true;
};

/** 所有有资金曲线的玩家，按快照顺序；可见性取自筛选。 */
std::vector<ChartSeries> CollectChartSeries(const viewer::ReplayViewSnapshot& snapshot,
                                            const EconomyPlayerFilter& filter);

/** 序列的房屋下标，用作筛选的候选列表。 */
std::vector<int> GetChartHouseIndices(const std::vector<ChartSeries>& series);

struct ChartParams
{
    /** ImGui 控件 ID，同一窗口内唯一。 */
    const char* mId = "资金曲线图";
    EconomyTimeRange mRange = EconomyTimeRange::All;
    /** 观看信息所选的玩家，其曲线更粗并最后绘制。 */
    int mSelectedHouseIndex = -1;
    /** 控件总尺寸，含左侧纵轴标签与下方横轴标签。 */
    float mWidth = 100.0f;
    float mHeight = 100.0f;
    /** 普通曲线与所选玩家曲线的线宽（已含 HUD 缩放）。 */
    float mLineThickness = 2.0f;
    float mSelectedThickness = 3.0f;
    bool mIsNominalTime = false;
};

/**
 * 在当前光标处绘制图表控件并让布局前进。横轴按 params.mRange 与全部曲线的数据范围确定，
 * 纵轴只按窗口内可见曲线缩放；鼠标悬停在绘图区时画竖线、各可见曲线上的取点标记，并弹出列出各玩家资金的提示
 * （取离该时刻最近的样本点）。没有任何点时在绘图区写“尚无采样”。
 */
void DrawEconomyPlot(const std::vector<ChartSeries>& series, const ChartParams& params);

/** 图表上的玩家颜色：过暗的颜色会被提亮到可辨认（EnsureChartLineColor），图例与曲线用同一颜色。 */
ImU32 GetChartColor(std::uint32_t rgb);

const char* GetPlayerDisplayName(const viewer::PlayerSummary& player);

/** 图例里的当前资金：优先取发布时刻的实时资金，缺失时退回曲线最后一个点；都没有时为空。 */
std::optional<std::int64_t> GetCurrentCredits(const ChartSeries& entry);

/** 坐标轴说明：“纵轴 资金 · 横轴 游戏时间”，名义时间时附琥珀色“（名义）”。自动换行，不越出内容区。 */
void DrawEconomyAxisCaption(bool isNominalTime);

/** 筛选状态的简短提示：只看某位时为“只看：名称”，隐藏了若干位时为“已隐藏 N 位”，没有筛选时不输出。 */
void DrawEconomyFilterHint(const std::vector<ChartSeries>& series, const EconomyPlayerFilter& filter);
}
