#include "ReplayHudParts.h"

#include "../viewer/ReplayViewExchange.h"

#include <algorithm>

namespace rareplay::ui::detail
{
void DrawFrameControls(const viewer::ReplayViewSnapshot& snapshot, viewer::ReplayViewExchange& exchange,
                       std::vector<UiRect>& inputRegions)
{
    constexpr const char* SPEED_LABELS[] = {
        "最快", "很快", "快", "正常", "慢", "很慢", "最慢"};
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const bool compact = availableWidth < 730.0f;

    const auto status = snapshot.mPlaybackControls.mStatus;
    ImGui::BeginDisabled(status == viewer::PlaybackStatus::Ended || !snapshot.mActive);
    if (ImGui::Button(status == viewer::PlaybackStatus::Playing ? "暂停" : "继续"))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::SetPaused,
            status == viewer::PlaybackStatus::Playing ? 1 : 0});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!snapshot.mPlaybackControls.mCanRestart || !snapshot.mActive);
    if (ImGui::Button("重新播放"))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::RestartPlayback, 0});
    ImGui::EndDisabled();
    if (!snapshot.mPlaybackControls.mCanRestart && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("需要更新启动器并重新打开回放，才能重新播放");
    ImGui::SameLine();
    if (ImGui::Button("退出回放"))
        exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::ExitPlayback, 0});
    if (status == viewer::PlaybackStatus::Ended)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("回放结束");
    }
    if (!snapshot.mPlaybackControls.mRestartError.empty())
        ImGui::TextWrapped("%s", snapshot.mPlaybackControls.mRestartError.c_str());

    if (snapshot.mTotalFrames && *snapshot.mTotalFrames > 0)
    {
        const float progress = std::clamp(static_cast<float>(snapshot.mFrameNumber) /
                                          static_cast<float>(*snapshot.mTotalFrames), 0.0f, 1.0f);
        ImGui::Text("帧 %u / %u", snapshot.mFrameNumber, *snapshot.mTotalFrames);
        if (!compact)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(100.0f, availableWidth - 420.0f));
            ImGui::ProgressBar(progress, ImVec2(std::max(100.0f, availableWidth - 420.0f), 12.0f), "");
        }
    }
    else
    {
        ImGui::Text("当前帧 %u · 总帧未知", snapshot.mFrameNumber);
    }

    ImGui::SameLine();
    const int speed = snapshot.mPlaybackSpeedSetting;
    const char* currentLabel = speed >= 0 && speed <= 6 ? SPEED_LABELS[speed] : "未知";
    if (snapshot.mCanChangePlaybackSpeed && snapshot.mActive)
    {
        ImGui::SetNextItemWidth(compact ? 150.0f : 160.0f);
        if (ImGui::BeginCombo("##播放速度", currentLabel))
        {
            for (int setting = 0; setting <= 6; ++setting)
            {
                if (ImGui::Selectable(SPEED_LABELS[setting], speed == setting))
                {
                    exchange.Submit({snapshot.mSessionId, viewer::ViewerCommandKind::SetPlaybackSpeed, setting});
                }
            }
            const ImVec2 popupPosition = ImGui::GetWindowPos();
            const ImVec2 popupSize = ImGui::GetWindowSize();
            inputRegions.push_back({popupPosition.x, popupPosition.y,
                                    popupPosition.x + popupSize.x, popupPosition.y + popupSize.y});
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("速度");
    }
    else
    {
        ImGui::TextDisabled("速度 %s · 不可调", currentLabel);
    }
    if (ImGui::GetContentRegionAvail().x > 85.0f)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("F10 隐藏");
    }
}
}
