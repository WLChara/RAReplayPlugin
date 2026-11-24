#include "ReplayHudParts.h"

#include "ReplayHudFormat.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rareplay::ui::detail
{
namespace
{
constexpr ImU32 OUTAGE_COLOR = IM_COL32(241, 165, 70, 255);
constexpr ImU32 STALL_COLOR = IM_COL32(232, 98, 86, 255);
constexpr ImU32 NOMINAL_COLOR = IM_COL32(245, 181, 89, 255);
// 停产历史列表最多显示的行数（按小块行高估算），超过后列表内部滚动。
constexpr float HISTORY_MAX_LINES = 9.0f;

struct StateStyle
{
    const char* mText;
    ImU32 mColor;
};

StateStyle GetStateStyle(viewer::ProductionLineState state)
{
    switch (state)
    {
    case viewer::ProductionLineState::Producing: return {"生产中", IM_COL32(120, 205, 130, 255)};
    case viewer::ProductionLineState::Paused: return {"暂停", IM_COL32(140, 170, 205, 255)};
    case viewer::ProductionLineState::PowerLoss: return {"断电/不可用", OUTAGE_COLOR};
    case viewer::ProductionLineState::NoFunds: return {"缺钱", IM_COL32(232, 205, 90, 255)};
    case viewer::ProductionLineState::Stalled: return {"停产", STALL_COLOR};
    case viewer::ProductionLineState::NoFactory: break;
    }
    return {"无工厂", IM_COL32(130, 136, 128, 255)};
}

const char* GetLineName(viewer::ProductionLineKind kind)
{
    return kind == viewer::ProductionLineKind::Barracks ? "兵营" : "重工";
}

// 一行小块，占满当前可用宽度；文字超出时被裁剪，高度随换行增加。
void DrawChipRow(const std::vector<StatChip>& chips)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = MeasureStatChips(chips, width);
    ImGui::Dummy(ImVec2(width, height));
    DrawStatChips(chips, origin, width, ImVec4(origin.x, origin.y, origin.x + width, origin.y + height));
}

std::vector<StatChip> BuildStatusChips(viewer::ProductionLineKind kind, const viewer::ProductionLineStatus& status,
                                       const viewer::ReplayViewSnapshot& snapshot)
{
    const StateStyle style = GetStateStyle(status.mState);
    std::vector<StatChip> chips;
    chips.push_back({GetLineName(kind), style.mText, style.mColor});
    if (status.mState == viewer::ProductionLineState::Stalled)
    {
        if (snapshot.mGameTime)
            chips.push_back({"已持续", FormatGameClock(snapshot.mGameTime->mMilliseconds - status.mStalledSinceMs),
                             STALL_COLOR});
        if (status.mStallHadPowerOutage)
            chips.push_back({"", "断电", OUTAGE_COLOR});
    }
    return chips;
}

// 停产汇总：总停产时间与停产率（总停产时间 ÷ 当前游戏时间）。游戏时间为名义时间时，停产率旁标注“（名义）”。
std::vector<StatChip> BuildSummaryChips(const viewer::ProductionLineStatus& status,
                                        const viewer::ReplayViewSnapshot& snapshot)
{
    std::vector<StatChip> chips;
    chips.push_back({"总停产", FormatGameClock(status.mTotalStalledMs),
                     status.mTotalStalledMs > 0 ? STALL_COLOR : IM_COL32(236, 235, 219, 255)});
    const float* rate = status.mStallRatePercent ? &*status.mStallRatePercent : nullptr;
    chips.push_back({"停产率", FormatPercent(rate)});
    if (snapshot.mGameTime && snapshot.mGameTime->mIsNominal)
        chips.push_back({"", "（名义）", NOMINAL_COLOR});
    return chips;
}

struct HistoryEntry
{
    viewer::ProductionLineKind mKind = viewer::ProductionLineKind::Barracks;
    std::int64_t mStartMs = 0;
    std::int64_t mEndMs = 0;
    bool mHadOutage = false;
    bool mIsOngoing = false;
};

std::vector<HistoryEntry> CollectHistory(const viewer::PlayerSummary& player, const viewer::ReplayViewSnapshot& snapshot)
{
    std::vector<HistoryEntry> entries;
    const auto collect = [&](viewer::ProductionLineKind kind, const std::optional<viewer::ProductionLineStatus>& line) {
        if (!line)
            return;
        if (line->mHistory)
        {
            for (const viewer::StallRecord& record : *line->mHistory)
                entries.push_back({kind, record.mStartMs, record.mEndMs, record.mHadPowerOutage, false});
        }
        if (line->mState == viewer::ProductionLineState::Stalled)
        {
            const std::int64_t end = snapshot.mGameTime ? snapshot.mGameTime->mMilliseconds : line->mStalledSinceMs;
            entries.push_back({kind, line->mStalledSinceMs, std::max(end, line->mStalledSinceMs),
                               line->mStallHadPowerOutage, true});
        }
    };
    collect(viewer::ProductionLineKind::Barracks, player.mBarracksLine);
    collect(viewer::ProductionLineKind::WarFactory, player.mWarFactoryLine);
    // 最新的排在最上面；同一开始时间时进行中的在前。
    std::stable_sort(entries.begin(), entries.end(), [](const HistoryEntry& left, const HistoryEntry& right) {
        if (left.mStartMs != right.mStartMs)
            return left.mStartMs > right.mStartMs;
        return left.mIsOngoing && !right.mIsOngoing;
    });
    return entries;
}

std::vector<StatChip> BuildHistoryChips(const HistoryEntry& entry)
{
    std::vector<StatChip> chips;
    chips.push_back({GetLineName(entry.mKind), FormatGameClock(entry.mStartMs)});
    chips.push_back({"时长", FormatGameClock(entry.mEndMs - entry.mStartMs), STALL_COLOR});
    if (entry.mIsOngoing)
        chips.push_back({"", "进行中", STALL_COLOR});
    if (entry.mHadOutage)
        chips.push_back({"", "断电", OUTAGE_COLOR});
    return chips;
}

// 悬停说明总停产时间与停产率的口径。
constexpr char SUMMARY_TOOLTIP[] =
    "总停产时间：已结束的停产时长之和，加上当前已持续满 2 秒的停产；不足 2 秒的空档不计。\n"
    "停产率：总停产时间 ÷ 当前游戏时间（含尚无工厂的时间）。";

// 一条线一张卡片：第一行是当前状态，第二行是总停产时间与停产率；窄面板中小块自动换行。
void DrawLineStatusCard(viewer::ProductionLineKind kind, const viewer::ProductionLineStatus& status,
                        const viewer::ReplayViewSnapshot& snapshot)
{
    const CardMetrics card = GetCardMetrics();
    const std::vector<StatChip> stateChips = BuildStatusChips(kind, status, snapshot);
    const std::vector<StatChip> summaryChips = BuildSummaryChips(status, snapshot);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float innerWidth = std::max(1.0f, width - card.mPadding * 2.0f);
    const float rowGap = std::max(1.0f, std::round(ImGui::GetTextLineHeight() * 0.12f));
    const float stateHeight = MeasureStatChips(stateChips, innerWidth);
    const float summaryHeight = MeasureStatChips(summaryChips, innerWidth);
    const float height = card.mPadding * 2.0f + stateHeight + rowGap + summaryHeight;
    ImGui::PushID(static_cast<int>(kind));
    ImGui::InvisibleButton("产线状态", ImVec2(width, height));
    const ImVec2 end(start.x + width, start.y + height);
    DrawCardBackground(ImGui::GetWindowDrawList(), start, end, GetCardState(false));
    const ImVec2 inner(start.x + card.mPadding, start.y + card.mPadding);
    const ImVec4 clip(inner.x, start.y, inner.x + innerWidth, end.y);
    DrawStatChips(stateChips, inner, innerWidth, clip);
    DrawStatChips(summaryChips, ImVec2(inner.x, inner.y + stateHeight + rowGap), innerWidth, clip);
    ImGui::SetItemTooltip("%s", SUMMARY_TOOLTIP);
    ImGui::PopID();
}

void DrawStallHistory(const viewer::PlayerSummary& player, const viewer::ReplayViewSnapshot& snapshot)
{
    const std::vector<HistoryEntry> entries = CollectHistory(player, snapshot);
    DrawSectionTitle("停产历史 %zu", entries.size());
    if (entries.empty())
    {
        TextWrappedDisabled("暂无停产记录");
        return;
    }
    // 列表是带浅色边框的凹槽：高度按（扣除可能出现的滚动条与内边距后的）宽度估算，条目少时恰好容纳，
    // 多时封顶并在内部滚动。
    const ImGuiStyle& style = ImGui::GetStyle();
    const CardMetrics card = GetCardMetrics();
    const float inset = card.mPadding + card.mBorder;
    const float contentWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - style.ScrollbarSize - inset * 2.0f);
    const float rowGap = style.ItemSpacing.y;
    float contentHeight = 0.0f;
    for (const HistoryEntry& entry : entries)
        contentHeight += MeasureStatChips(BuildHistoryChips(entry), contentWidth) + rowGap;
    const float maxHeight = ImGui::GetTextLineHeightWithSpacing() * HISTORY_MAX_LINES;
    const float listHeight = std::min(contentHeight + inset * 2.0f, maxHeight);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.075f, 0.068f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.46f, 0.53f, 0.47f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, card.mBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(card.mPadding, card.mPadding));
    if (ImGui::BeginChild("停产历史列表", ImVec2(0.0f, listHeight), true))
    {
        for (const HistoryEntry& entry : entries)
            DrawChipRow(BuildHistoryChips(entry));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void DrawLineStatus(const viewer::ReplayViewSnapshot& snapshot, const viewer::PlayerSummary& player)
{
    DrawSectionTitle("产线状态");
    if (!player.mBarracksLine && !player.mWarFactoryLine)
    {
        TextWrappedDisabled("暂无产线状态数据");
        return;
    }
    {
        CardListSpacing spacing;
        if (player.mBarracksLine)
            DrawLineStatusCard(viewer::ProductionLineKind::Barracks, *player.mBarracksLine, snapshot);
        if (player.mWarFactoryLine)
            DrawLineStatusCard(viewer::ProductionLineKind::WarFactory, *player.mWarFactoryLine, snapshot);
    }
    DrawStallHistory(player, snapshot);
}
}

void DrawProductionLines(const viewer::ReplayViewSnapshot& snapshot, const viewer::PlayerSummary* player,
                         const ReplayHud::IconResolver& iconResolver)
{
    if (player == nullptr)
    {
        TextWrappedDisabled("未选择玩家");
        return;
    }
    DrawLineStatus(snapshot, *player);
    if (!player->mHasProductionData)
    {
        TextWrappedDisabled("该玩家暂无建造数据");
        return;
    }
    if (player->mProductionLines.empty())
    {
        TextWrappedDisabled("当前没有建造项目");
        return;
    }

    // 图标随字号缩放（字号已含 HUD 缩放），并给右侧文字至少留出约四个字宽。
    DrawSectionTitle("产线 %zu", player->mProductionLines.size());
    const float fontSize = ImGui::GetFontSize();
    CardListSpacing spacing;
    for (std::size_t lineIndex = 0; lineIndex < player->mProductionLines.size(); ++lineIndex)
    {
        const auto& line = player->mProductionLines[lineIndex];
        ImGui::PushID(static_cast<int>(line.mId));
        {
            CardScope card;
            const float iconSize = std::max(fontSize * 1.4f, std::min(fontSize * 2.2f, card.GetInnerWidth() - fontSize * 4.0f));
            const char* label = line.mLabel.empty() ? "未命名产线" : line.mLabel.c_str();
            ImGui::Text("%s · 第 %zu 条", label, lineIndex + 1);
            DrawIcon(line.mIcon, iconResolver, ImVec2(iconSize, iconSize));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("%s", line.mProductName.empty() ? "未知建造项目" : line.mProductName.c_str());
            const char* status = line.mReady ? "已就绪" : line.mOnHold ? "已暂停" : "建造中";
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::Text("%s · 队列 %u", status, line.mQueuedCount);
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            const float progress = std::clamp(line.mProgress, 0.0f, 1.0f);
            ImGui::ProgressBar(progress, ImVec2(card.GetInnerWidth(), std::max(3.0f, fontSize * 0.28f)), "");
        }
        ImGui::PopID();
    }
}
}
