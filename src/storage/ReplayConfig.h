#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rareplay::storage
{
enum class ReplayMode : std::uint32_t { Record = 1, Play = 2 };

struct ReplayRequest
{
    ReplayMode mMode = ReplayMode::Record;
    std::int64_t mCreatedUnixMilliseconds = 0;
    std::array<std::uint8_t, 16> mRequestId{};
    std::filesystem::path mReplayPath;
    std::vector<std::uint8_t> mManifest;
};

/** 严格解析 RPRT V2；nowUnixMilliseconds 为当前 UTC Unix 毫秒。 */
bool ParseReplayRequest(std::span<const std::uint8_t> bytes,
    std::int64_t nowUnixMilliseconds, ReplayRequest& outRequest, std::string& outError);

/** 从游戏目录读取标记；缺失也视为非错误的 false。 */
bool ReadReplayRequest(const std::filesystem::path& markerPath,
    ReplayRequest& outRequest, std::vector<std::uint8_t>& outRawBytes,
    std::string& outError);

/** 只删除字节内容仍与验证时完全相同的标记。 */
bool ConsumeReplayRequest(const std::filesystem::path& markerPath,
    std::span<const std::uint8_t> expectedBytes, std::string& outError);

/** 播放请求可以不携带清单；非空副本必须与容器内权威清单一致。 */
bool MatchesArchiveManifest(const ReplayRequest& request,
    std::span<const std::uint8_t> archiveManifest);
}
