#include "ScenarioSessionSnapshot.h"

#include <cstring>

namespace rareplay::game_api::native
{
namespace
{
// GameMode 0：战役。
constexpr std::uint32_t CAMPAIGN_GAME_MODE = 0;

template <typename T>
void AppendDifference(std::string& text, const char* name, T expected, T actual)
{
    if (expected == actual)
        return;
    if (!text.empty())
        text += ' ';
    text += name;
    text += '=';
    text += std::to_string(expected);
    text += '/';
    text += std::to_string(actual);
}
}

std::optional<std::string> CopyScenarioFileName(const char* text, std::size_t capacity)
{
    if (text == nullptr || capacity == 0)
        return std::nullopt;
    // 只在定长数组内找结尾，不用 strlen：未终止的数组不能越界读取。
    const void* terminator = std::memchr(text, '\0', capacity);
    if (terminator == nullptr)
        return std::nullopt;
    const auto length = static_cast<std::size_t>(static_cast<const char*>(terminator) - text);
    if (length == 0)
        return std::nullopt;
    return std::string(text, length);
}

bool CanRestartFromSnapshot(const ScenarioSessionSnapshot& snapshot) noexcept
{
    return snapshot.mGameMode != CAMPAIGN_GAME_MODE && !snapshot.mScenarioFileName.empty();
}

std::string DescribeSnapshotDifferences(const ScenarioSessionSnapshot& expected, const ScenarioSessionSnapshot& actual)
{
    std::string text;
    AppendDifference(text, "gameMode", expected.mGameMode, actual.mGameMode);
    AppendDifference(text, "maxAhead", expected.mMaxAhead, actual.mMaxAhead);
    AppendDifference(text, "frameSendRate", expected.mFrameSendRate, actual.mFrameSendRate);
    AppendDifference(text, "maxMaxAhead", expected.mMaxMaxAhead, actual.mMaxMaxAhead);
    AppendDifference(text, "desiredFrameRate", expected.mDesiredFrameRate, actual.mDesiredFrameRate);
    AppendDifference(text, "specialFlags", expected.mSpecialRulesFlags, actual.mSpecialRulesFlags);
    AppendDifference(text, "multiplayerObserver", expected.mMultiplayerObserver, actual.mMultiplayerObserver);
    if (expected.mScenarioFileName != actual.mScenarioFileName)
    {
        if (!text.empty())
            text += ' ';
        text += "scenario=\"" + expected.mScenarioFileName + "\"/\"" + actual.mScenarioFileName + "\"";
    }
    return text;
}
}
