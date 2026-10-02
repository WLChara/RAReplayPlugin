#pragma once

#include "ViewerControlProtocol.h"

#include <filesystem>
#include <string>

namespace rareplay::storage
{
/** 游戏线程拥有的单次观看通信通道；不拥有回放路径，清理与重启由启动器负责。 */
class ViewerControlChannel final
{
public:
    ViewerControlChannel(const std::filesystem::path& gameDirectory,
        std::array<std::uint8_t, 16> requestId);
    /** 只接受当前请求的 Ready，读取失败仅禁用重播，不影响观看。 */
    bool Initialize();
    bool CanRestart() const noexcept { return mIsReady; }
    std::uint32_t GetInitialFlags() const noexcept { return mInitialFlags; }
    const std::filesystem::path& GetPath() const noexcept { return mPath; }
    /** 原子替换成功后才返回 true；重复成功请求幂等。失败保留原有通道与世界。 */
    bool RequestRestart(std::uint32_t flags, std::string& outError);

private:
    bool ReadOwnedRecord(ViewerControlRecord& outRecord) const;

    std::filesystem::path mPath;
    std::array<std::uint8_t, 16> mRequestId;
    std::uint32_t mInitialFlags = 0;
    bool mIsReady = false;
    bool mRestartPublished = false;
};
}
