#pragma once

#include "ZoomMapping.h"

#include <cstdint>
#include <vector>

namespace rareplay::zoom
{
/** 16 位像素格式；由游戏的 GetRGBMode 决定。555 的最高位不属于任何通道。 */
enum class PixelFormat16
{
    Rgb565,
    Rgb555
};

/**
 * 只读 16 位像素视图，不持有内存。mData 指向第 0 行第 0 个像素，须按 2 字节对齐；
 * mPitchBytes 是相邻两行起点的字节距离，为偶数且不小于 mWidth × 2（可以更大）。
 */
struct ConstPixelView16
{
    const void* mData = nullptr;
    int mWidth = 0;
    int mHeight = 0;
    int mPitchBytes = 0;
};

/** 可写 16 位像素视图；约定同 ConstPixelView16。 */
struct PixelView16
{
    void* mData = nullptr;
    int mWidth = 0;
    int mHeight = 0;
    int mPitchBytes = 0;
};

/**
 * 缩放核的临时行缓冲，由调用方持有并跨帧复用：只在需要的容量增大时（逻辑宽度变大或首次使用）重新分配，
 * 稳定档位下每帧不分配。内容在两次调用之间没有意义。只在一个线程上使用。
 */
class ScaleScratch final
{
public:
    /** 释放缓冲（退出缩放时调用）。 */
    void Release() noexcept;

private:
    friend bool ScaleBattlefield(const ConstPixelView16& source, const PixelView16& target,
        const ZoomTransform& transform, PixelFormat16 format, const SpanTable& columnSpans, const SpanTable& rowSpans,
        ScaleScratch& scratch) noexcept;

    std::vector<std::uint32_t> mSums;
};

/**
 * 把逻辑画面 {0,0,L} 缩放到物理画面 {0,0,P}：目标的每个物理像素是源中该像素区间盒（列区间 × 行区间）内
 * 全部逻辑像素的面积平均。各通道分别求和，按 (sum + count/2) / count 取整；555 的最高位按 0 读入并写回 0。
 * 区间长度为 1 时等价于直接复制，即放大为最近邻。
 * 实现为可分离的两遍：先把同一行区间内的源行逐列纵向累加到临时行，再按列区间横向求和并取平均；
 * 行区间与上一目标行相同时直接复制上一目标行。
 *
 * @param source 源（逻辑）视图，宽高不小于 L；只读取 [0, L) 范围。
 * @param target 目标（物理）视图，宽高不小于 P；只写入 [0, P) 范围，行距多出的字节不碰。源与目标不得重叠。
 * @param transform 必须合法（ZoomTransform::IsValid）。
 * @param columnSpans、rowSpans 必须分别由 BuildSpanTable(Pw, Lw) 与 BuildSpanTable(Ph, Lh) 构建。
 * @param scratch 调用方持有的临时缓冲，容量不足时在此增长。
 * @return 参数不合法（视图为空或未对齐、行距为奇数或小于宽 × 2、源小于 L、目标小于 P、区间表与变换不符、
 *         格式非法）或临时缓冲分配失败时返回 false，此时不写目标。
 */
bool ScaleBattlefield(const ConstPixelView16& source, const PixelView16& target, const ZoomTransform& transform,
    PixelFormat16 format, const SpanTable& columnSpans, const SpanTable& rowSpans, ScaleScratch& scratch) noexcept;

/**
 * 持有两张区间表与临时缓冲的缩放器：(P, L) 变化时重建区间表，否则每帧直接复用。
 * 游戏线程独占；不持有任何视图或表面。
 */
class BattlefieldScaler final
{
public:
    /** 契约同 ScaleBattlefield；区间表构建失败时释放全部缓冲并返回 false，不写目标。 */
    bool Scale(const ConstPixelView16& source, const PixelView16& target, const ZoomTransform& transform,
        PixelFormat16 format) noexcept;
    /** 释放区间表与临时缓冲（退出缩放时调用）；之后仍可再次使用。 */
    void Release() noexcept;

private:
    SpanTable mColumnSpans;
    SpanTable mRowSpans;
    ScaleScratch mScratch;
};
}
