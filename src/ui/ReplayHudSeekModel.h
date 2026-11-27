#pragma once

#include "../viewer/ReplayViewModel.h"

#include <cstdint>
#include <string>

/**
 * 底栏进度条与快进状态的显示内容（不含 ImGui 的纯逻辑），只由快照决定。
 * 进度条横轴按帧号均分：最左是第 0 帧，最右是最后一帧（总帧数 − 1）；帧号与快照 mFrameNumber 同一单位。
 * 控件只提交观看命令，目标是否被接受、是否在快进中都以游戏线程发布的快照为准。
 */
namespace rareplay::ui
{
/** 当前位置之前的区域不可点（进程内重开不可用，例如战役录像）时的悬停说明。 */
inline constexpr char SEEK_BACKWARD_TOOLTIP[] = "此录像不能向后跳转";
/** 快进中，当前位置与目标之间的区域不可点时的悬停说明；当前位置之前与目标之后都可以点。 */
inline constexpr char SEEK_BEFORE_TARGET_TOOLTIP[] = "这一段正在快进，要停在这里请先取消";
/** 可以点、但要经进程内重开才能到达的位置（当前位置之前，或重开已请求时的任何位置）的悬停说明。 */
inline constexpr char SEEK_RELOAD_TOOLTIP[] = "需要重新载入";

/** 进度条上的比例（0 到 1，超出时夹取）对应的帧号，四舍五入到最近的帧；总帧数为 0 时为 0。 */
std::uint32_t GetFrameAtBarFraction(float fraction, std::uint32_t totalFrames) noexcept;

/** 帧号在进度条上的比例：frame ÷ (总帧数 − 1)，夹在 [0, 1]；总帧数不超过 1 时为 1。 */
float GetBarFractionOfFrame(std::uint32_t frame, std::uint32_t totalFrames) noexcept;

enum class SeekBarMode
{
    /** 总帧数未知或为 0：不画进度条，显示“总帧未知”。 */
    Hidden,
    /** 只读：与原来的进度条相同，不响应点击；窄布局下不画。 */
    ReadOnly,
    /** 可交互：在可跳转的位置点击，或拖动后松开，提交一次跳转。 */
    Interactive
};

struct SeekBarModel
{
    SeekBarMode mMode = SeekBarMode::Hidden;
    std::uint32_t mTotalFrames = 0;
    /** 当前位置（最后完成帧）。 */
    std::uint32_t mCurrentFrame = 0;
    /** 可以向前跳转（快照 mCanSeek）：大于 mClickableAfterFrame 的帧可作目标。 */
    bool mCanSeekForward = false;
    /** 可以向后跳转（快照 mCanSeekBackward）：当前位置之前的帧可作目标，需要重新载入。 */
    bool mCanSeekBackward = false;
    /** 向前跳转时只有大于它的帧可以作为目标：快进中为当前目标，否则为当前位置。 */
    std::uint32_t mClickableAfterFrame = 0;
    /** 快进中（含进程内重开已请求、尚未执行）：底栏显示状态文字与“取消”，禁用速度、暂停、半黑幕与缩放控件。 */
    bool mIsSeeking = false;
    /** 进程内重开已请求、尚未执行：除待执行的目标外，任何位置都可改作重开后的目标；进度为 0。 */
    bool mIsReloading = false;
    std::uint32_t mSeekStartFrame = 0;
    std::uint32_t mSeekTargetFrame = 0;
    /** 快进进度的整数百分比（向下取整）；不在快进中或重开尚未执行时为 0。 */
    int mSeekPercent = 0;
    /**
     * 快进中底栏的状态文字，如“正在跳转到 12:34 … 45%”；目标时间是名义值时附“（名义）”，没有时间轴时为
     * “正在跳转到帧 N … 45%”。不在快进中时为空。
     */
    std::string mSeekStatus;
};

/**
 * 按快照得出进度条的模式与快进状态：快照激活、向前或向后跳转的能力标志至少一个为真且总帧数至少 2 时可交互；
 * 总帧数已知但不能跳转（回放结束且不能向后跳转、已请求退出、运行时尚未提供跳转）时只读。快进状态只在快照激活时显示。
 */
SeekBarModel BuildSeekBarModel(const viewer::ReplayViewSnapshot& snapshot);

/**
 * 帧能否作为跳转目标：可交互且不超过最后一帧，并且满足其一：
 * 重开已请求时不是待执行的目标；当前位置之前且可以向后跳转；可以向前跳转且在可点区域之后。
 */
bool CanSeekToFrame(const SeekBarModel& model, std::uint32_t frame) noexcept;

/**
 * 进度条上一点的说明：有时间轴时为“mm:ss（帧 N）”，该帧时间是名义值时再附“（名义）”；
 * 没有时间轴或帧号超出时间轴时为“帧 N”。时间格式同 FormatGameClock。
 */
std::string FormatSeekPointLabel(const viewer::GameTimeline* timeline, std::uint32_t frame);

/**
 * 悬停或拖动到某帧时的提示：上一行是该点的说明。下一行：要经进程内重开才能到达的目标为 SEEK_RELOAD_TOOLTIP；
 * 不能作为目标时说明原因（不能向后跳转时当前位置及之前为 SEEK_BACKWARD_TOOLTIP，快进中当前位置与目标之间为
 * SEEK_BEFORE_TARGET_TOOLTIP）；其余没有下一行。
 */
std::string BuildSeekBarTooltip(const SeekBarModel& model, const viewer::GameTimeline* timeline, std::uint32_t frame);
}
