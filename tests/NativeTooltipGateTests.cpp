#include "../src/plugin/NativeTooltipGate.h"

#include <iostream>
#include <optional>
#include <stdexcept>

namespace
{
using rareplay::plugin::NativeTooltipGate;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// 模拟原生 IsActive：门控返回值即本帧对 SetState 的调用。
struct NativeTooltip
{
    bool mIsActive = true;
    int mSetStateCalls = 0;

    void Step(NativeTooltipGate& gate, bool shouldSuppress)
    {
        if (const auto state = gate.Update(shouldSuppress, mIsActive))
        {
            mIsActive = *state;
            ++mSetStateCalls;
        }
    }
};

void CheckEnterHideRestore()
{
    NativeTooltipGate gate;
    NativeTooltip tooltip;
    tooltip.Step(gate, false);
    Require(tooltip.mIsActive && tooltip.mSetStateCalls == 0, "Before the HUD shows nothing is touched");

    tooltip.Step(gate, true);
    Require(!tooltip.mIsActive && tooltip.mSetStateCalls == 1 && gate.IsSuppressing(),
            "Visible HUD during playback turns native tooltips off");
    tooltip.Step(gate, true);
    Require(tooltip.mSetStateCalls == 1, "Staying hidden does not call SetState again");

    // F10 隐藏 HUD：恢复原值。
    tooltip.Step(gate, false);
    Require(tooltip.mIsActive && tooltip.mSetStateCalls == 2 && !gate.IsSuppressing(),
            "Hiding the HUD restores the saved state");
    tooltip.Step(gate, false);
    Require(tooltip.mSetStateCalls == 2, "Restore happens only once");

    // 再次显示又关闭。
    tooltip.Step(gate, true);
    Require(!tooltip.mIsActive && tooltip.mSetStateCalls == 3, "Showing the HUD again turns tooltips off again");
}

void CheckExternalReenable()
{
    NativeTooltipGate gate;
    NativeTooltip tooltip;
    tooltip.Step(gate, true);
    Require(!tooltip.mIsActive, "Tooltips are off while the HUD is visible");
    // 0x48CE5D、0x52DB04 或选项应用 0x4E1FA9 再次调用 SetState(true)。
    tooltip.mIsActive = true;
    tooltip.Step(gate, true);
    Require(!tooltip.mIsActive && tooltip.mSetStateCalls == 2, "External re-enable is turned off again next frame");
    // 回放结束：恢复为启用。
    tooltip.Step(gate, false);
    Require(tooltip.mIsActive && tooltip.mSetStateCalls == 3, "Playback end restores tooltips");
}

void CheckOriginallyDisabled()
{
    NativeTooltipGate gate;
    NativeTooltip tooltip;
    tooltip.mIsActive = false;
    tooltip.Step(gate, true);
    Require(!tooltip.mIsActive && tooltip.mSetStateCalls == 0, "Already-off tooltips need no call");
    tooltip.Step(gate, false);
    Require(!tooltip.mIsActive && tooltip.mSetStateCalls == 0, "Restoring an originally-off state keeps it off");

    // 原本关闭、播放中被外部启用：恢复时沿用这个新值（启用）。
    tooltip.Step(gate, true);
    tooltip.mIsActive = true;
    tooltip.Step(gate, true);
    Require(!tooltip.mIsActive, "External enable while suppressed is overridden");
    tooltip.Step(gate, false);
    Require(tooltip.mIsActive, "Latest external state is restored at the end");
}
}

int main()
{
    try
    {
        CheckEnterHideRestore();
        CheckExternalReenable();
        CheckOriginallyDisabled();
        std::cout << "Native tooltip gate tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
