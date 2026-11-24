#include "ReplayHudParts.h"

#include <algorithm>
#include <vector>

namespace rareplay::ui::detail
{
void DrawFpsCard(const viewer::ReplayViewSnapshot& snapshot, float scale)
{
    ImGui::TextUnformatted("录制帧率");
    if (snapshot.mIsPreview)
    {
        ImGui::TextColored(ImVec4(0.96f, 0.71f, 0.35f, 1.0f), "布局预览 · 示例数据");
    }
    if (snapshot.mRecordedFps.empty())
    {
        ImGui::TextDisabled("录制 FPS：无数据");
        ImGui::Dummy(ImVec2(0.0f, 45.0f * scale));
    }
    else
    {
        constexpr std::size_t MAX_SAMPLES = 90;
        const std::size_t firstSample = snapshot.mRecordedFps.size() > MAX_SAMPLES ?
            snapshot.mRecordedFps.size() - MAX_SAMPLES : 0;
        std::vector<float> values;
        values.reserve(snapshot.mRecordedFps.size() - firstSample);
        float maximumFps = 60.0f;
        for (std::size_t index = firstSample; index < snapshot.mRecordedFps.size(); ++index)
        {
            const float fps = std::max(0.0f, snapshot.mRecordedFps[index].mFps);
            values.push_back(fps);
            maximumFps = std::max(maximumFps, fps);
        }
        ImGui::Text("录制 FPS：%.1f", values.back());
        ImGui::PlotLines("##录制帧率曲线", values.data(), static_cast<int>(values.size()),
                         0, nullptr, 0.0f, maximumFps * 1.1f, ImVec2(-1.0f, 47.0f * scale));
        ImGui::TextDisabled("录像帧 %u–%u", snapshot.mRecordedFps[firstSample].mFrameNumber,
                            snapshot.mRecordedFps.back().mFrameNumber);
    }
    ImGui::Separator();
    ImGui::TextDisabled("播放端 FPS：%.1f", ImGui::GetIO().Framerate);
}
}
