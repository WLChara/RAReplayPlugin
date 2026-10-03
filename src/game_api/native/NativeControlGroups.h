#pragma once

#include "../../replay/RecordedControlGroups.h"

namespace rareplay::game_api::native
{
/** 在世界就绪的游戏线程采集录制方全量编队；包括存活但暂时离场的成员。 */
bool ReadRecordedControlGroups(replay::RecordedControlGroupSnapshot& outSnapshot,
    std::string& error);
}
