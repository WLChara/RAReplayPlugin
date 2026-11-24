#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rareplay::ui
{
/** 游戏时间文字：mm:ss；满 1 小时为 h:mm:ss；负值按 0。 */
std::string FormatGameClock(std::int64_t milliseconds);

/**
 * 资金坐标轴刻度：不足 1000 为整数；不足 100 万以 k 为单位，满 100 万以 M 为单位。
 * 整数倍不带小数（5k、2M），否则最多两位小数并去掉末尾的 0（2.5k、1.25M）；负值带负号。
 */
std::string FormatCreditsTick(std::int64_t credits);

/** 百分数保留一位小数并附 %，如 6.6%；缺失或非有限时为“—”。传入的是百分数本身（6.6 表示 6.6%）。 */
std::string FormatPercent(const float* percent);

/** 战损比保留两位小数，如 1.85；比值缺失时为“—”。 */
std::string FormatRatio(const float* ratio);

/**
 * 资金坐标轴的“整齐”刻度间隔：不小于 minStep 的最小 1、2、5 × 10^n。minStep 不是正数时按 1。
 * 刻度从 0 起每隔该间隔一条线。
 */
std::int64_t GetNiceCreditsStep(double minStep) noexcept;

/**
 * 时间坐标轴的刻度间隔（毫秒），取自 10 秒到 2 小时的整齐档位中不小于 minStepMs 的最小者；
 * 超出最大档位时取最大档位。
 */
std::int64_t GetNiceTimeStepMs(std::int64_t minStepMs) noexcept;

struct ChipPlacement
{
    /** 相对排版区域左上角的位置。 */
    float mX = 0.0f;
    float mY = 0.0f;
    /** 实际占用宽度；比可用宽度宽的标签被夹到可用宽度（调用方绘制时裁剪）。 */
    float mWidth = 0.0f;
};

struct ChipLayout
{
    std::vector<ChipPlacement> mPlacements;
    /** 全部行的总高度；没有标签时为 0。 */
    float mHeight = 0.0f;
};

/**
 * 把一串“标签 数值”小块从左到右排版，放不下就换行，保证任何一块都不越过 availableWidth。
 * widths 为各块自身宽度，gap 为同行相邻块的间隔，lineHeight 与 lineGap 为行高与行距。
 */
ChipLayout LayoutChips(const std::vector<float>& widths, float availableWidth, float gap, float lineHeight,
                       float lineGap) noexcept;
}
