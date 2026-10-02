#include "ReplayHud.h"

#include "ReplayHudParts.h"
#include "ReplayHudLayout.h"
#include "../viewer/ReplayViewExchange.h"

#include <algorithm>
#include <utility>

namespace rareplay::ui
{
namespace
{
constexpr float EDGE_GAP = 14.0f;
constexpr float PANEL_WIDTH = 324.0f;
constexpr float FPS_WIDTH = 258.0f;
constexpr float FPS_HEIGHT = 180.0f;
constexpr float FOOTER_HEIGHT = 106.0f;

void RecordWindowRegion(std::vector<UiRect>& regions)
{
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    regions.push_back({position.x, position.y, position.x + size.x, position.y + size.y});
}

const viewer::PlayerSummary* FindPlayer(const viewer::ReplayViewSnapshot& snapshot, int houseIndex)
{
    for (const auto& player : snapshot.mPlayers)
    {
        if (player.mHouseIndex == houseIndex)
        {
            return &player;
        }
    }
    return nullptr;
}

void PushHudStyle()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.105f, 0.125f, 0.118f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.135f, 0.155f, 0.140f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.205f, 0.225f, 0.194f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.28f, 0.30f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.32f, 0.19f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.44f, 0.39f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.53f, 0.43f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.39f, 0.79f, 0.67f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.86f, 0.69f, 0.31f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.25f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.34f, 0.35f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.43f, 0.38f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.20f, 0.24f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.40f, 0.37f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.36f, 0.32f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocused, ImVec4(0.20f, 0.24f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, ImVec4(0.36f, 0.32f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.80f, 0.65f, 0.31f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
}

void PopHudStyle()
{
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(18);
}
}

void ReplayHud::Draw(const viewer::ReplayViewSnapshot& snapshot, viewer::ReplayViewExchange& exchange)
{
    mInputRegions.clear();
    mVisibleRegions.clear();
    if (mSessionId != snapshot.mSessionId)
    {
        mSessionId = snapshot.mSessionId;
        mSelectedHouseIndex = -1;
        if (!mSelectPanelPending)
        {
            mSelectedPanel = ReplayPanel::Production;
        }
    }

    if (FindPlayer(snapshot, mSelectedHouseIndex) == nullptr)
    {
        mSelectedHouseIndex = snapshot.mRecordedLocalHouseIndex;
        if (FindPlayer(snapshot, mSelectedHouseIndex) == nullptr && !snapshot.mPlayers.empty())
        {
            mSelectedHouseIndex = snapshot.mPlayers.front().mHouseIndex;
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 extent = viewport->WorkSize;
    if (extent.x < 400.0f || extent.y < 300.0f)
    {
        return;
    }

    PushHudStyle();
    const float footerHeight = snapshot.mPlaybackControls.mRestartError.empty() ? FOOTER_HEIGHT : 144.0f;
    const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

    const float panelWidth = std::min(PANEL_WIDTH, std::max(260.0f, extent.x * 0.32f));
    const float fpsWidth = std::min(FPS_WIDTH, std::max(180.0f, extent.x - panelWidth - EDGE_GAP * 3.0f));
    ImGui::SetNextWindowPos(ImVec2(origin.x + EDGE_GAP, origin.y + EDGE_GAP));
    ImGui::SetNextWindowSize(ImVec2(fpsWidth, FPS_HEIGHT));
    if (ImGui::Begin("录制帧率##ReplayFps", nullptr, fixedFlags | ImGuiWindowFlags_NoInputs))
    {
        RecordWindowRegion(mVisibleRegions);
        detail::DrawFpsCard(snapshot);
    }
    ImGui::End();

    const auto nativePanelTop = GetSidebarPanelTop(snapshot.mNativeUiLayout, extent.x, extent.y, mGameContentRect);
    const float panelTop = nativePanelTop.value_or(extent.y) + EDGE_GAP;
    const float panelHeight = extent.y - footerHeight - EDGE_GAP * 2.0f - panelTop;
    // 原生布局尚未就绪或空间不足时不显示侧栏，不能为了显示 HUD 覆盖雷达。
    if (nativePanelTop && panelHeight >= 120.0f)
    {
        ImGui::SetNextWindowPos(ImVec2(origin.x + extent.x - panelWidth - EDGE_GAP, origin.y + panelTop));
        ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight));
        if (ImGui::Begin("回放信息##ReplayPanel", nullptr, fixedFlags))
        {
            RecordWindowRegion(mInputRegions);
            ImGui::TextUnformatted("回放战术面板");
            if (snapshot.mIsPreview)
            {
                ImGui::TextColored(ImVec4(0.96f, 0.71f, 0.35f, 1.0f), "布局预览 · 示例数据");
            }
            if (!snapshot.mActive)
            {
                ImGui::TextDisabled("当前没有活动回放");
            }
            if (!snapshot.mReplayName.empty())
            {
                ImGui::TextWrapped("%s", snapshot.mReplayName.c_str());
            }
            if (!snapshot.mMapName.empty())
            {
                ImGui::TextDisabled("地图：%s", snapshot.mMapName.c_str());
            }
            ImGui::Separator();
            ImGui::TextUnformatted("玩家与经济");
            const float playerListHeight = std::clamp(panelHeight * 0.28f, 84.0f, 172.0f);
            detail::DrawPlayerList(snapshot, mSelectedHouseIndex, playerListHeight);
            if (!snapshot.mHasPlayerVision)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.71f, 0.40f, 1.0f));
                ImGui::TextWrapped("仅切换信息 · 地图视野未切换");
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextDisabled("地图视野由宿主管理");
            }
            ImGui::Separator();

            const viewer::PlayerSummary* selectedPlayer = FindPlayer(snapshot, mSelectedHouseIndex);
            if (ImGui::BeginTabBar("回放详情##ReplayDetails"))
            {
                const ImGuiTabItemFlags productionFlags = mSelectPanelPending && mSelectedPanel == ReplayPanel::Production ?
                    ImGuiTabItemFlags_SetSelected : 0;
                const ImGuiTabItemFlags unitFlags = mSelectPanelPending && mSelectedPanel == ReplayPanel::Units ?
                    ImGuiTabItemFlags_SetSelected : 0;
                const ImGuiTabItemFlags groupFlags = mSelectPanelPending && mSelectedPanel == ReplayPanel::Groups ?
                    ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("建造", nullptr, productionFlags))
                {
                    ImGui::BeginChild("建造内容", ImVec2(0.0f, 0.0f), false);
                    detail::DrawProductionLines(selectedPlayer, mIconResolver);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("单位", nullptr, unitFlags))
                {
                    ImGui::BeginChild("单位内容", ImVec2(0.0f, 0.0f), false);
                    detail::DrawUnitInventory(selectedPlayer, mIconResolver);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("本地编队", nullptr, groupFlags))
                {
                    ImGui::BeginChild("编队内容", ImVec2(0.0f, 0.0f), false);
                    detail::DrawRecordedGroups(snapshot);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
                mSelectPanelPending = false;
            }
        }
        ImGui::End();
        }

    ImGui::SetNextWindowPos(ImVec2(origin.x + EDGE_GAP, origin.y + extent.y - footerHeight - EDGE_GAP));
    ImGui::SetNextWindowSize(ImVec2(extent.x - EDGE_GAP * 2.0f, footerHeight));
    if (ImGui::Begin("播放状态##ReplayFooter", nullptr, fixedFlags))
    {
        RecordWindowRegion(mInputRegions);
        detail::DrawFrameControls(snapshot, exchange, mInputRegions);
    }
    ImGui::End();
    mVisibleRegions.insert(mVisibleRegions.end(), mInputRegions.begin(), mInputRegions.end());
    PopHudStyle();
}

void ReplayHud::Reset()
{
    mSessionId = 0;
    mSelectedHouseIndex = -1;
    mInputRegions.clear();
    mVisibleRegions.clear();
    mSelectedPanel = ReplayPanel::Production;
    mSelectPanelPending = false;
}

void ReplayHud::SetIconResolver(IconResolver resolver)
{
    mIconResolver = std::move(resolver);
}

void ReplayHud::SelectPanel(ReplayPanel panel)
{
    mSelectedPanel = panel;
    mSelectPanelPending = true;
}

const std::vector<UiRect>& ReplayHud::GetInputRegions() const noexcept
{
    return mInputRegions;
}

const std::vector<UiRect>& ReplayHud::GetVisibleRegions() const noexcept
{
    return mVisibleRegions;
}
}
