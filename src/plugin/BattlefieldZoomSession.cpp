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

void BattlefieldZoomSession::Stop() noexcept
{
    if (!mIsActive)
        return;
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
