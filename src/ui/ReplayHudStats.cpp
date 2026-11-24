#include "ReplayHudParts.h"

#include "ReplayHudFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace rareplay::ui::detail
{
namespace
{
// 卡片背景比面板亮，标签与弱化文字相应提亮以保持对比度。
constexpr ImU32 LABEL_COLOR = IM_COL32(176, 196, 176, 255);
constexpr ImU32 MUTED_COLOR = IM_COL32(158, 166, 154, 255);
constexpr ImU32 ACTIVE_COLOR = IM_COL32(120, 205, 130, 255);
constexpr ImVec4 NOMINAL_COLOR(0.96f, 0.71f, 0.35f, 1.0f);

struct ChipMetrics
{
    float mLabelWidth = 0.0f;
    float mLabelAdvance = 0.0f;
    float mCheckWidth = 0.0f;
    float mValueWidth = 0.0f;
    float mSpace = 0.0f;
    float mTotal = 0.0f;
};

// 标签与数值之间留一个字号的 0.3（标签为空时不留），数值前的对勾占 0.7 个字号再加同样的间隔。
ChipMetrics MeasureChip(const StatChip& chip)
{
    ChipMetrics metrics;
    const float fontSize = ImGui::GetFontSize();
    metrics.mSpace = std::max(3.0f, std::round(fontSize * 0.3f));
    metrics.mLabelWidth = ImGui::CalcTextSize(chip.mLabel.c_str()).x;
    metrics.mValueWidth = ImGui::CalcTextSize(chip.mValue.c_str()).x;
    metrics.mCheckWidth = chip.mHasCheck ? std::round(fontSize * 0.7f) + metrics.mSpace : 0.0f;
    metrics.mLabelAdvance = chip.mLabel.empty() ? 0.0f : metrics.mLabelWidth + metrics.mSpace;
    metrics.mTotal = metrics.mLabelAdvance + metrics.mCheckWidth + metrics.mValueWidth;
    return metrics;
}

ChipLayout BuildLayout(const std::vector<StatChip>& chips, float availableWidth)
{
    std::vector<float> widths;
    widths.reserve(chips.size());
    for (const StatChip& chip : chips)
        widths.push_back(MeasureChip(chip).mTotal);
    const float fontSize = ImGui::GetFontSize();
    return LayoutChips(widths, availableWidth, std::max(5.0f, std::round(fontSize * 0.45f)),
                       ImGui::GetTextLineHeight(), std::max(1.0f, std::round(ImGui::GetTextLineHeight() * 0.12f)));
}

void DrawCheck(ImDrawList* drawList, const ImVec2& origin, float size, float lineHeight, ImU32 color)
{
    const float thickness = std::max(1.5f, ImGui::GetFontSize() * 0.1f);
    const float top = origin.y + (lineHeight - size) * 0.5f;
    drawList->AddPolyline(std::array<ImVec2, 3>{ImVec2(origin.x, top + size * 0.55f),
                                                ImVec2(origin.x + size * 0.38f, top + size * 0.9f),
                                                ImVec2(origin.x + size, top + size * 0.15f)}.data(),
                          3, color, ImDrawFlags_None, thickness);
}

std::string FormatCount(std::uint32_t value)
{
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%u", value);
    return buffer;
}

StatChip MakeUpgradeChip(const char* label, const viewer::UpgradeStatus& status)
{
    StatChip chip;
    chip.mLabel = label;
    if (status.mIsActive)
    {
        chip.mValue = FormatGameClock(status.mActivatedAtMs);
        chip.mValueColor = ACTIVE_COLOR;
        chip.mHasCheck = true;
    }
    else
    {
        chip.mValue = "—";
        chip.mValueColor = MUTED_COLOR;
    }
    return chip;
}
}

float MeasureStatChips(const std::vector<StatChip>& chips, float availableWidth)
{
    return BuildLayout(chips, availableWidth).mHeight;
}

float DrawStatChips(const std::vector<StatChip>& chips, const ImVec2& origin, float availableWidth,
                    const ImVec4& clip)
{
    const ChipLayout layout = BuildLayout(chips, availableWidth);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float lineHeight = ImGui::GetTextLineHeight();
    for (std::size_t index = 0; index < chips.size(); ++index)
    {
        const StatChip& chip = chips[index];
        const ChipPlacement& placement = layout.mPlacements[index];
        const ChipMetrics metrics = MeasureChip(chip);
        // 单块比可用宽度宽时只裁剪它自己的区域，不影响其他块。
        const ImVec4 chipClip(std::max(clip.x, origin.x + placement.mX), clip.y,
                              std::min(clip.z, origin.x + placement.mX + placement.mWidth), clip.w);
        const ImVec2 position(origin.x + placement.mX, origin.y + placement.mY);
        drawList->AddText(nullptr, 0.0f, position, LABEL_COLOR, chip.mLabel.c_str(), nullptr, 0.0f, &chipClip);
        float x = position.x + metrics.mLabelAdvance;
        if (chip.mHasCheck)
        {
            DrawCheck(drawList, ImVec2(x, position.y), std::round(ImGui::GetFontSize() * 0.7f), lineHeight,
                      chip.mValueColor);
            x += metrics.mCheckWidth;
        }
        drawList->AddText(nullptr, 0.0f, ImVec2(x, position.y), chip.mValueColor, chip.mValue.c_str(), nullptr,
                          0.0f, &chipClip);
    }
    return layout.mHeight;
}

std::vector<StatChip> BuildPlayerStatChips(const viewer::PlayerSummary& player)
{
    std::vector<StatChip> chips;
    const auto optionalCount = [](const std::optional<std::uint32_t>& value) {
        return value ? FormatCount(*value) : std::string("—");
    };
    chips.push_back({"部队", optionalCount(player.mUnitTotal)});
    if (!player.mStatistics)
    {
        chips.push_back({"统计", "暂无", MUTED_COLOR});
        return chips;
    }
    const viewer::PlayerStatistics& stats = *player.mStatistics;
    chips.push_back({"建造", FormatCount(stats.GetBuiltTotal())});
    if (stats.mCombat)
    {
        const viewer::CombatStatistics& combat = *stats.mCombat;
        chips.push_back({"摧毁", FormatCount(combat.mDestroyedUnits) + "/" + FormatCount(combat.mDestroyedBuildings)});
        chips.push_back({"损失", FormatCount(combat.mLostUnits + combat.mLostBuildings)});
        const float* ratio = combat.mKillLossRatio ? &*combat.mKillLossRatio : nullptr;
        chips.push_back({"战损比", FormatRatio(ratio)});
    }
    else
    {
        chips.push_back({"摧毁", "—", MUTED_COLOR});
        chips.push_back({"损失", "—", MUTED_COLOR});
        chips.push_back({"战损比", "—", MUTED_COLOR});
    }
    chips.push_back({"分数", FormatCount(stats.mScore)});
    // 单机 AI 的决策不经过命令事件，操作与 APM 显示“—”。
    if (stats.mOperationCount)
    {
        chips.push_back({"操作", FormatCount(*stats.mOperationCount)});
        char apm[24];
        if (stats.mApm)
            std::snprintf(apm, sizeof(apm), "%.0f", static_cast<double>(*stats.mApm));
        else
            std::snprintf(apm, sizeof(apm), "—");
        chips.push_back({"APM", apm});
    }
    else
    {
        chips.push_back({"操作", "—", MUTED_COLOR});
    }
    chips.push_back(MakeUpgradeChip("兵营升级", stats.mBarracksUpgrade));
    chips.push_back(MakeUpgradeChip("重工升级", stats.mWarFactoryUpgrade));
    return chips;
}

std::string BuildPlayerStatTooltip(const viewer::PlayerSummary& player)
{
    if (!player.mStatistics)
        return {};
    const viewer::PlayerStatistics& stats = *player.mStatistics;
    const auto number = [](std::uint32_t value) { return std::to_string(value); };
    std::string text;
    if (player.mUnitTotal)
        text += "部队总数 " + number(*player.mUnitTotal) + "（步兵、载具、飞机，含矿车，不含建筑）\n";
    if (stats.mCombat)
    {
        const viewer::CombatStatistics& combat = *stats.mCombat;
        text += "摧毁 单位 " + number(combat.mDestroyedUnits) + "、建筑 " + number(combat.mDestroyedBuildings) +
                "（只计敌方）\n";
        text += "损失 单位 " + number(combat.mLostUnits) + "、建筑 " + number(combat.mLostBuildings) +
                "（各敌方对本方的击杀）\n";
    }
    text += "建造 建筑 " + number(stats.mBuiltBuildings) + "、载具 " + number(stats.mBuiltVehicles) + "、步兵 " +
            number(stats.mBuiltInfantry) + "、飞机 " + number(stats.mBuiltAircraft);
    if (stats.mOperationCount)
    {
        text += "\n操作 " + number(*stats.mOperationCount) + " 次：同一帧内同一玩家、同类型且除执行单位外参数相同的命令合并为一次";
        if (stats.mOperationEventCount)
            text += "（原始命令事件 " + number(*stats.mOperationEventCount) + " 条）";
    }
    return text;
}

void DrawGameTimeHeader(const viewer::ReplayViewSnapshot& snapshot)
{
    if (!snapshot.mGameTime)
    {
        TextWrappedDisabled("游戏时间 —");
        return;
    }
    TextWrappedFit("游戏时间 %s", FormatGameClock(snapshot.mGameTime->mMilliseconds).c_str());
    if (snapshot.mGameTime->mIsNominal)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, NOMINAL_COLOR);
        TextWrappedFit("（名义）");
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("单机速度 0 原生不限速，该帧按 16 毫秒计，所以时间只是名义值");
    }
}
}
