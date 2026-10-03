#pragma once

#include "ReplayArchive.h"
#include "../replay/ReplayPorts.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rareplay::storage
{
/** 创建独占 .partial 并写入帧流；仅 Finish 可发布最终文件。 */
class FrameWriter final : public replay::IReplaySink
{
public:
    bool Open(const std::wstring& path, const std::array<std::uint8_t, 32>& targetHash,
        const std::vector<std::uint8_t>& metadata, const replay::ReplaySessionInfo& sessionInfo,
        std::string& error);
    bool AppendFrame(const replay::ReplayFrame& frame, std::string& error) override;
    bool Finish(std::string& error) override;
    void Abort() noexcept override;

private:
    ArchiveWriter mArchive;
    std::uint64_t mNextFrame = 0;
    std::uint32_t mStreamVersion = 2;
    bool mIsOpen = false;
    bool mHasFailed = false;
};
}
