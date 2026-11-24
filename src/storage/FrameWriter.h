#pragma once

#include "ReplayArchive.h"
#include "../replay/ReplayPorts.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rareplay::storage
{
/**
 * 创建独占 .partial 并写入帧流（容器 v3，帧流头与每条帧记录各是一个不可拆分的写入单元）；仅 Finish 可发布最终文件。
 * policy 原样转交 ArchiveWriter，默认为生产切块策略。
 */
class FrameWriter final : public replay::IReplaySink
{
public:
    explicit FrameWriter(const ArchiveBlockPolicy& policy = ArchiveBlockPolicy{});

    bool Open(const std::wstring& path, const std::array<std::uint8_t, 32>& targetHash,
        const std::vector<std::uint8_t>& metadata, const replay::ReplaySessionInfo& sessionInfo,
        std::string& error);
    bool AppendFrame(const replay::ReplayFrame& frame, std::string& error) override;
    bool Finish(std::string& error) override;
    void Abort() noexcept override;

    // 以下统计只读，Finish 成功后保持到下次 Open 或 Abort，供录制完成日志核对压缩率。
    /** 已写入的帧数。 */
    std::uint64_t GetFrameCount() const { return mNextFrame - mFirstFrame; }
    /** 逻辑帧流字节数（帧流头加帧记录，含尚未写出的待写块）。 */
    std::uint64_t GetLogicalStreamBytes() const;
    /** 已写出的帧流段磁盘字节数。 */
    std::uint64_t GetStoredStreamBytes() const;

private:
    ArchiveWriter mArchive;
    std::uint64_t mFirstFrame = 0;
    std::uint64_t mNextFrame = 0;
    std::uint32_t mStreamVersion = 2;
    // 帧流头的 gameMode；决定写入的帧是否必须或不得携带游戏速度。
    std::uint32_t mGameMode = 0;
    bool mIsOpen = false;
    bool mHasFailed = false;
};
}
