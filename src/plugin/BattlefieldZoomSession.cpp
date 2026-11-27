#include "BattlefieldZoomSession.h"

#include "Diagnostics.h"

namespace rareplay::plugin
{
void BattlefieldZoomSession::Begin() noexcept
{
    if (mIsActive)
        return;
    mIsActive = true;
    game_api::zoom::Enable();
    FlushLog();
}

void BattlefieldZoomSession::BeginWithPreviousZoom() noexcept
{
    if (mIsActive)
        return;
    Begin();
    // Enable 把请求复原为 1×。RequestStep 的档数为正时向放大方向、即下标减小的方向移动，
    // 所以从 1× 回到原档位的档数是两者下标之差（不是反过来）。
    const int steps = rareplay::zoom::IDENTITY_ZOOM_INDEX - mRequestedIndexAtStop;
    if (steps != 0)
        StepZoom(steps, game_api::zoom::ZoomStepAnchor::Center);
}

void BattlefieldZoomSession::Stop() noexcept
{
    if (!mIsActive)
        return;
    mRequestedIndexAtStop = game_api::zoom::GetStatus().mRequestedIndex;
    mIsActive = false;
    game_api::zoom::Disable();
    FlushLog();
}

void BattlefieldZoomSession::Update() noexcept
{
    if (mIsActive)
        FlushLog();
}

void BattlefieldZoomSession::StepZoom(int steps, game_api::zoom::ZoomStepAnchor anchor) noexcept
{
    if (mIsActive)
        game_api::zoom::RequestStep(steps, anchor);
}

void BattlefieldZoomSession::ResetZoom() noexcept
{
    if (mIsActive)
        game_api::zoom::RequestReset();
}

game_api::zoom::BattlefieldZoomStatus BattlefieldZoomSession::GetStatus() const noexcept
{
    return game_api::zoom::GetStatus();
}

void BattlefieldZoomSession::FlushLog() noexcept
{
    for (const std::string& line : game_api::zoom::TakeLogLines())
        Log(line.c_str());
}
}
