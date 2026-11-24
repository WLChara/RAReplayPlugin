#include "../src/plugin/BattlefieldZoomViewMapping.h"
#include "../src/zoom/ZoomLevels.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using rareplay::game_api::zoom::BattlefieldZoomStatus;
using rareplay::game_api::zoom::ZoomDisableReason;
using rareplay::plugin::BuildBattlefieldZoomView;
using rareplay::plugin::GetBattlefieldZoomPercent;
using rareplay::zoom::IDENTITY_ZOOM_INDEX;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// 已启用、门控打开、1×、允许 4× 到 1/3 的基准状态。
BattlefieldZoomStatus EnabledStatus()
{
    BattlefieldZoomStatus status;
    status.mIsEnabled = true;
    status.mDisableReason = ZoomDisableReason::None;
    status.mAppliedIndex = IDENTITY_ZOOM_INDEX;
    status.mRequestedIndex = IDENTITY_ZOOM_INDEX;
    status.mMostMagnifiedIndex = rareplay::zoom::MOST_MAGNIFIED_ZOOM_INDEX;
    status.mFarthestShrinkIndex = rareplay::zoom::FARTHEST_SHRINK_ZOOM_INDEX;
    return status;
}

void CheckPercents()
{
    constexpr std::array<int, 12> EXPECTED{400, 300, 250, 200, 150, 125, 100, 80, 67, 50, 40, 33};
    static_assert(EXPECTED.size() == rareplay::zoom::ZOOM_LEVELS.size());
    for (int index = 0; index < static_cast<int>(EXPECTED.size()); ++index)
        Require(GetBattlefieldZoomPercent(index) == EXPECTED[static_cast<std::size_t>(index)],
            "percent of level " + std::to_string(index));
    Require(GetBattlefieldZoomPercent(-1) == 100 && GetBattlefieldZoomPercent(12) == 100,
        "out-of-range levels read as 100%");
}

void CheckReasons()
{
    // 未启用：默认值，界面不显示控件。
    BattlefieldZoomStatus notEnabled;
    auto view = BuildBattlefieldZoomView(notEnabled);
    Require(!view.mIsAvailable && view.mUnavailableReason.empty() && !view.mCanZoomIn && !view.mCanZoomOut,
        "a disabled adapter hides the controls");
    // 门控关闭与未启用同时出现时仍按未启用处理（录制或回放已结束）。
    notEnabled.mIsGateClosed = true;
    Require(BuildBattlefieldZoomView(notEnabled).mUnavailableReason.empty(), "not enabled wins over the gate");

    struct ReasonCase
    {
        ZoomDisableReason mReason;
        const char* mText;
    };
    const ReasonCase cases[] = {
        {ZoomDisableReason::ResourceUnavailable, "内存不足，无法缩放"},
        {ZoomDisableReason::ScaleFailed, "缩放出错，已停用"},
        {ZoomDisableReason::UnexpectedEngineState, "缩放出错，已停用"},
        {ZoomDisableReason::Exception, "缩放出错，已停用"}};
    for (const ReasonCase& reasonCase : cases)
    {
        BattlefieldZoomStatus status = EnabledStatus();
        status.mIsEnabled = false;
        status.mDisableReason = reasonCase.mReason;
        // 失败停用优先于门控。
        status.mIsGateClosed = true;
        view = BuildBattlefieldZoomView(status);
        Require(!view.mIsAvailable && view.mUnavailableReason == reasonCase.mText && !view.mCanZoomIn &&
                !view.mCanZoomOut && !view.mHasPendingChange,
            std::string("failure reason text: ") + reasonCase.mText);
    }

    BattlefieldZoomStatus gateClosed = EnabledStatus();
    gateClosed.mIsGateClosed = true;
    gateClosed.mRequestedIndex = 3;
    view = BuildBattlefieldZoomView(gateClosed);
    Require(!view.mIsAvailable && view.mUnavailableReason == "当前布局不支持缩放" && !view.mHasPendingChange,
        "a closed gate reports the unsupported layout");
}

void CheckAvailable()
{
    auto view = BuildBattlefieldZoomView(EnabledStatus());
    Require(view.mIsAvailable && !view.mIsActive && view.mPercent == 100 && view.mCanZoomIn && view.mCanZoomOut &&
            !view.mHasPendingChange && view.mUnavailableReason.empty(),
        "1x with the full range can zoom both ways");

    // 50% 已应用：激活，两个方向都还能走。
    BattlefieldZoomStatus half = EnabledStatus();
    half.mIsActive = true;
    half.mAppliedIndex = 9;
    half.mRequestedIndex = 9;
    view = BuildBattlefieldZoomView(half);
    Require(view.mIsActive && view.mPercent == 50 && view.mCanZoomIn && view.mCanZoomOut && !view.mHasPendingChange,
        "active 50%");

    // 到头：请求在最大放大档（4×）不能再放大；在最远缩小档不能再缩小。
    BattlefieldZoomStatus most = EnabledStatus();
    most.mIsActive = true;
    most.mAppliedIndex = 0;
    most.mRequestedIndex = 0;
    view = BuildBattlefieldZoomView(most);
    Require(view.mPercent == 400 && !view.mCanZoomIn && view.mCanZoomOut, "4x cannot zoom in further");
    BattlefieldZoomStatus farthest = EnabledStatus();
    farthest.mFarthestShrinkIndex = 9;
    farthest.mIsActive = true;
    farthest.mAppliedIndex = 9;
    farthest.mRequestedIndex = 9;
    view = BuildBattlefieldZoomView(farthest);
    Require(view.mPercent == 50 && view.mCanZoomIn && !view.mCanZoomOut, "the budget limit stops zooming out");

    // 待应用：百分比仍是已应用档位，范围按请求档位判断（下一次换档从请求档位出发）。
    BattlefieldZoomStatus pending = EnabledStatus();
    pending.mAppliedIndex = IDENTITY_ZOOM_INDEX;
    pending.mRequestedIndex = 0;
    view = BuildBattlefieldZoomView(pending);
    Require(view.mPercent == 100 && view.mHasPendingChange && !view.mIsActive && !view.mCanZoomIn && view.mCanZoomOut,
        "a deferred request keeps the applied percent and uses the requested level for the range");

    // 物理尺寸未读到时允许范围只有 1×：两个方向都不能走。
    BattlefieldZoomStatus unknownSize = EnabledStatus();
    unknownSize.mMostMagnifiedIndex = IDENTITY_ZOOM_INDEX;
    unknownSize.mFarthestShrinkIndex = IDENTITY_ZOOM_INDEX;
    view = BuildBattlefieldZoomView(unknownSize);
    Require(view.mIsAvailable && !view.mCanZoomIn && !view.mCanZoomOut, "1x-only range disables both directions");
}
}

int main()
{
    try
    {
        CheckPercents();
        CheckReasons();
        CheckAvailable();
        std::cout << "Battlefield zoom view tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
