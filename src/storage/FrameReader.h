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
 * 打开容器（v2 或 v3）并按顺序读取帧；文件句柄由本对象独占。
 *
 * 按容器头 flags 自动分流：完成文件先完整校验；未完成录像（flags==0，通常是遗留的 .partial）
 * 没有内容哈希，打开时先预扫描出从帧 0 起最长的一段完整有效连续帧，ReadFrame 在该部分末尾
 * 返回 End，其后的字节不会被解码。
 * 两种容器逐帧使用同一套有效性规则：FrameCodec::ReadFrame 之后再按帧流头执行 FrameCodec::ValidateGameSpeed。
 * 逻辑流因块解码失败、CRC 不符或读文件失败而停止时一律报告 Error，不会当作正常 EOF。
 */
class FrameReader final : public replay::IReplaySource
{
public:
    /**
     * 打开失败时对象保持关闭状态。未完成录像若没有任何完整有效帧也会失败。
     * 目标可执行文件哈希与清单的核对规则对两种容器相同。
     */
    bool Open(const std::wstring& path, const std::array<std::uint8_t, 32>& expectedTargetHash,
        std::vector<std::uint8_t>& outMetadata, std::string& error);
    bool GetSessionInfo(replay::ReplaySessionInfo& outInfo, std::string& error) override;
    replay::ReplayReadResult ReadFrame(replay::ReplayFrame& outFrame, std::string& error) override;
    void Close();

    /** 已成功打开且容器为未完成录像时为 true；未打开或完成文件为 false。 */
    bool IsUnfinalized() const { return mIsOpen && mIsUnfinalized; }
    /** 未完成录像预扫描得到的可恢复帧数（至少为 1）；完成文件和未打开时为 0。 */
    std::uint32_t GetRecoveredFrameCount() const { return mRecoveredFrameCount; }

    // 以下为打开时容器层报告的只读信息（含义见 ArchiveInfo），未打开时为 0。
    std::uint32_t GetContainerVersion() const { return mContainerVersion; }
    std::uint64_t GetLogicalStreamBytes() const { return mLogicalStreamBytes; }
    std::uint64_t GetStoredStreamBytes() const { return mStoredStreamBytes; }

private:
    bool ScanRecoverableFrames(std::string& error);

    ArchiveReader mArchive;
    replay::ReplaySessionInfo mInfo;
    std::uint64_t mNextFrame = 0;
    std::uint64_t mLogicalStreamBytes = 0;
    std::uint64_t mStoredStreamBytes = 0;
    std::uint32_t mContainerVersion = 0;
    std::uint32_t mRecoveredFrameCount = 0;
    bool mIsOpen = false;
    bool mIsUnfinalized = false;
    bool mHasFailed = false;
};
}
