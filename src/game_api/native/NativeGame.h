#pragma once

namespace rareplay::game_api::native
{
bool IsWorldReady();
bool ExecuteDueEvents();
void ProcessPostFrameEvents();
void InvokeFrameParser();
bool WaitForPeers();
void RequestReplayExit();
void RequestViewerOptionsMenu();
void DrawViewerOnTop();
void ProcessGameOver();
bool IsReplayExitRequested();
void UpdatePausedCamera();
}
