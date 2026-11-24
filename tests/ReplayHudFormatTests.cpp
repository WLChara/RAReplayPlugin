#include "../src/ui/ReplayHudFormat.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
using namespace rareplay::ui;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void RequireText(const std::string& actual, const std::string& expected)
{
    Require(actual == expected, "expected \"" + expected + "\" but got \"" + actual + "\"");
}

// mm:ss；满 1 小时为 h:mm:ss。
void CheckGameClock()
{
    RequireText(FormatGameClock(0), "00:00");
    RequireText(FormatGameClock(999), "00:00");
    RequireText(FormatGameClock(59'999), "00:59");
    RequireText(FormatGameClock(60'000), "01:00");
    RequireText(FormatGameClock(754'000), "12:34");
    RequireText(FormatGameClock(3'599'999), "59:59");
    RequireText(FormatGameClock(3'600'000), "1:00:00");
    RequireText(FormatGameClock(3'725'000), "1:02:05");
    RequireText(FormatGameClock(36'000'000), "10:00:00");
    RequireText(FormatGameClock(-5'000), "00:00");
}

void CheckCreditsTick()
{
    RequireText(FormatCreditsTick(0), "0");
    RequireText(FormatCreditsTick(500), "500");
    RequireText(FormatCreditsTick(999), "999");
    RequireText(FormatCreditsTick(1000), "1k");
    RequireText(FormatCreditsTick(1500), "1.5k");
    RequireText(FormatCreditsTick(20'000), "20k");
    RequireText(FormatCreditsTick(-2000), "-2k");
    RequireText(FormatCreditsTick(-300), "-300");
    RequireText(FormatCreditsTick(2500), "2.5k");
    RequireText(FormatCreditsTick(1250), "1.25k");
    RequireText(FormatCreditsTick(500'000), "500k");
    // 满 100 万改用 M：整数倍不带小数，其余最多两位小数。
    RequireText(FormatCreditsTick(1'000'000), "1M");
    RequireText(FormatCreditsTick(1'500'000), "1.5M");
    RequireText(FormatCreditsTick(2'250'000), "2.25M");
    RequireText(FormatCreditsTick(20'000'000), "20M");
    RequireText(FormatCreditsTick(-3'000'000), "-3M");
    RequireText(FormatCreditsTick(INT64_MIN).substr(0, 1), "-");
}

void CheckPercent()
{
    const float rate = 6.6392f;
    RequireText(FormatPercent(&rate), "6.6%");
    const float zero = 0.0f;
    RequireText(FormatPercent(&zero), "0.0%");
    const float full = 100.0f;
    RequireText(FormatPercent(&full), "100.0%");
    RequireText(FormatPercent(nullptr), "—");
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    RequireText(FormatPercent(&notANumber), "—");
}

void CheckRatio()
{
    const float ratio = 1.8461f;
    RequireText(FormatRatio(&ratio), "1.85");
    RequireText(FormatRatio(nullptr), "—");
    const float zero = 0.0f;
    RequireText(FormatRatio(&zero), "0.00");
}

void CheckNiceSteps()
{
    Require(GetNiceCreditsStep(-3) == 1 && GetNiceCreditsStep(0) == 1 && GetNiceCreditsStep(1.0) == 1, "tiny steps");
    Require(GetNiceCreditsStep(1.5) == 2 && GetNiceCreditsStep(3) == 5 && GetNiceCreditsStep(5) == 5, "1-2-5 sequence");
    Require(GetNiceCreditsStep(5.1) == 10 && GetNiceCreditsStep(999) == 1000 && GetNiceCreditsStep(1000) == 1000,
            "step grows by powers of ten");
    Require(GetNiceCreditsStep(1500) == 2000 && GetNiceCreditsStep(2001) == 5000 && GetNiceCreditsStep(7000) == 10000,
            "thousand-scale steps");
    for (double minimum = 1.0; minimum < 5'000'000.0; minimum *= 1.37)
    {
        const std::int64_t step = GetNiceCreditsStep(minimum);
        Require(static_cast<double>(step) >= minimum - 1e-9, "step is not smaller than requested");
        Require(static_cast<double>(step) < minimum * 2.5 + 1.0, "step is the smallest nice value");
    }
    Require(GetNiceTimeStepMs(0) == 10'000 && GetNiceTimeStepMs(10'000) == 10'000 && GetNiceTimeStepMs(10'001) == 30'000,
            "time steps start at ten seconds");
    Require(GetNiceTimeStepMs(300'000) == 300'000 && GetNiceTimeStepMs(301'000) == 600'000, "five-minute step");
    Require(GetNiceTimeStepMs(3'600'000) == 3'600'000 && GetNiceTimeStepMs(10'000'000) == 14'400'000, "hour steps");
    Require(GetNiceTimeStepMs(INT64_MAX) == 28'800'000, "above the largest step the largest is used");
}

void CheckChipLayout()
{
    Require(LayoutChips({}, 100.0f, 4.0f, 20.0f, 2.0f).mPlacements.empty() &&
                LayoutChips({}, 100.0f, 4.0f, 20.0f, 2.0f).mHeight == 0.0f, "no chips, no height");

    // 同行放得下就紧挨着（间隔 4），放不下换行；行高 20、行距 2。
    auto layout = LayoutChips({60.0f, 60.0f, 60.0f}, 130.0f, 4.0f, 20.0f, 2.0f);
    Require(layout.mPlacements.size() == 3, "one placement per chip");
    Require(layout.mPlacements[0].mX == 0.0f && layout.mPlacements[0].mY == 0.0f, "first chip at the origin");
    Require(layout.mPlacements[1].mX == 64.0f && layout.mPlacements[1].mY == 0.0f, "second chip shares the line");
    Require(layout.mPlacements[2].mX == 0.0f && layout.mPlacements[2].mY == 22.0f, "third chip wraps");
    Require(layout.mHeight == 42.0f, "two lines of height 20 with a 2 gap");

    // 恰好放满一行（不留余量也不溢出）。
    layout = LayoutChips({63.0f, 63.0f}, 130.0f, 4.0f, 20.0f, 2.0f);
    Require(layout.mPlacements[1].mY == 0.0f && layout.mPlacements[1].mX + layout.mPlacements[1].mWidth == 130.0f,
            "exactly filled line");
    layout = LayoutChips({63.0f, 64.0f}, 130.0f, 4.0f, 20.0f, 2.0f);
    Require(layout.mPlacements[1].mY == 22.0f, "one pixel over wraps");

    // 单块比可用宽度还宽：夹到可用宽度并独占一行，其后的块另起一行。
    layout = LayoutChips({30.0f, 500.0f, 10.0f}, 100.0f, 4.0f, 20.0f, 2.0f);
    Require(layout.mPlacements[1].mWidth == 100.0f && layout.mPlacements[1].mX == 0.0f && layout.mPlacements[1].mY == 22.0f,
            "oversized chip is clamped onto its own line");
    Require(layout.mPlacements[2].mY == 44.0f && layout.mHeight == 64.0f, "chip after an oversized chip wraps");

    // 无论输入如何，任何一块都不越过可用宽度。
    for (float available = 20.0f; available <= 300.0f; available += 7.0f)
    {
        const std::vector<float> widths = {68.0f, 60.0f, 86.0f, 54.0f, 97.0f, 79.0f, 130.0f, 41.0f, 160.0f};
        const auto result = LayoutChips(widths, available, 5.0f, 21.0f, 2.0f);
        for (const ChipPlacement& placement : result.mPlacements)
            Require(placement.mX >= 0.0f && placement.mX + placement.mWidth <= available + 0.001f,
                    "chip stays inside the available width " + std::to_string(available));
    }
    // 非正的可用宽度退化为 1，仍不崩溃且高度合理。
    Require(LayoutChips({10.0f, 10.0f}, 0.0f, 4.0f, 20.0f, 2.0f).mHeight == 42.0f, "degenerate width");
}
}

int main()
{
    try
    {
        CheckGameClock();
        CheckCreditsTick();
        CheckPercent();
        CheckRatio();
        CheckNiceSteps();
        CheckChipLayout();
        std::cout << "Replay HUD format tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
