#include "ReplayHudSeekModel.h"

#include "ReplayHudFormat.h"

#include <algorithm>
#include <cmath>

namespace rareplay::ui
{
namespace
{
// 快进状态文字的前半句：有时间轴时为“正在跳转到 mm:ss”（名义值附“（名义）”），否则为“正在跳转到帧 N”。
std::string FormatSeekTarget(const viewer::GameTimeline* timeline, std::uint32_t frame)
{
    const auto time = timeline != nullptr ? timeline->GetTimeAt(frame) : std::nullopt;
    if (!time)
        return "正在跳转到帧 " + std::to_string(frame);
    return "正在跳转到 " + FormatGameClock(time->mMilliseconds) + (time->mIsNominal ? "（名义）" : "");
}
}

std::uint32_t GetFrameAtBarFraction(float fraction, std::uint32_t totalFrames) noexcept
{
    if (totalFrames <= 1 || !(fraction > 0.0f))
        return 0;
    const double lastFrame = static_cast<double>(totalFrames - 1);
    const double frame = std::round(std::min(1.0, static_cast<double>(fraction)) * lastFrame);
    return static_cast<std::uint32_t>(std::clamp(frame, 0.0, lastFrame));
}

float GetBarFractionOfFrame(std::uint32_t frame, std::uint32_t totalFrames) noexcept
{
    if (totalFrames <= 1 || frame >= totalFrames - 1)
        return 1.0f;
    return static_cast<float>(static_cast<double>(frame) / static_cast<double>(totalFrames - 1));
}

SeekBarModel BuildSeekBarModel(const viewer::ReplayViewSnapshot& snapshot)
{
    SeekBarModel model;
    model.mCurrentFrame = snapshot.mFrameNumber;
    model.mClickableAfterFrame = snapshot.mFrameNumber;
    model.mCanSeekForward = snapshot.mPlaybackControls.mCanSeek;
    model.mCanSeekBackward = snapshot.mPlaybackControls.mCanSeekBackward;
    const auto& seek = snapshot.mPlaybackControls.mSeek;
    if (snapshot.mActive && seek.mIsSeeking)
    {
        model.mIsSeeking = true;
        model.mIsReloading = seek.mIsReloading;
        model.mSeekStartFrame = seek.mStartFrame;
        model.mSeekTargetFrame = seek.mTargetFrame;
        model.mClickableAfterFrame = std::max(model.mClickableAfterFrame, seek.mTargetFrame);
        // 重开尚未执行时目标可能在当前位置之前，按公式会得出 100%；世界还没回到开头，进度按 0 显示。
        const float progress = seek.mIsReloading
            ? 0.0f : viewer::GetSeekProgress(seek.mStartFrame, seek.mTargetFrame, snapshot.mFrameNumber);
        model.mSeekPercent = std::clamp(static_cast<int>(std::floor(progress * 100.0f)), 0, 100);
        model.mSeekStatus = FormatSeekTarget(snapshot.mTimeline.get(), seek.mTargetFrame) + " … " +
                            std::to_string(model.mSeekPercent) + "%";
    }
    if (!snapshot.mTotalFrames || *snapshot.mTotalFrames == 0)
        return model;
    model.mTotalFrames = *snapshot.mTotalFrames;
    model.mMode = snapshot.mActive && (model.mCanSeekForward || model.mCanSeekBackward) && model.mTotalFrames >= 2
                      ? SeekBarMode::Interactive
                      : SeekBarMode::ReadOnly;
    return model;
}

bool CanSeekToFrame(const SeekBarModel& model, std::uint32_t frame) noexcept
{
    if (model.mMode != SeekBarMode::Interactive || frame >= model.mTotalFrames)
        return false;
    // 重开已请求：任何其他位置都只是改写重开后的目标（游戏线程的路由规则相同）。
    if (model.mIsReloading)
        return model.mCanSeekBackward && frame != model.mSeekTargetFrame;
    if (frame < model.mCurrentFrame)
        return model.mCanSeekBackward;
    return model.mCanSeekForward && frame > model.mClickableAfterFrame;
}

std::string FormatSeekPointLabel(const viewer::GameTimeline* timeline, std::uint32_t frame)
{
    const auto time = timeline != nullptr ? timeline->GetTimeAt(frame) : std::nullopt;
    if (!time)
        return "帧 " + std::to_string(frame);
    return FormatGameClock(time->mMilliseconds) + "（帧 " + std::to_string(frame) + "）" +
           (time->mIsNominal ? "（名义）" : "");
}

std::string BuildSeekBarTooltip(const SeekBarModel& model, const viewer::GameTimeline* timeline, std::uint32_t frame)
{
    std::string text = FormatSeekPointLabel(timeline, frame);
    if (model.mMode != SeekBarMode::Interactive)
        return text;
    const char* reason = nullptr;
    if (CanSeekToFrame(model, frame))
    {
        if (model.mIsReloading || frame < model.mCurrentFrame)
            reason = SEEK_RELOAD_TOOLTIP;
    }
    else if (model.mIsReloading)
    {
        // 待执行的目标本身：没有需要说明的原因。
    }
    else if (frame <= model.mCurrentFrame)
    {
        // 能向后跳转时，不可点的只剩当前位置本身，不必说明；不能时保持原来的说明。
        if (!model.mCanSeekBackward)
            reason = SEEK_BACKWARD_TOOLTIP;
    }
    else if (model.mIsSeeking)
    {
        reason = SEEK_BEFORE_TARGET_TOOLTIP;
    }
    if (reason != nullptr)
    {
        text += '\n';
        text += reason;
    }
    return text;
}
}
