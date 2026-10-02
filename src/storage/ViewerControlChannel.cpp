#include "ViewerControlChannel.h"

#include <Windows.h>

namespace rareplay::storage
{
ViewerControlChannel::ViewerControlChannel(const std::filesystem::path& gameDirectory,
    std::array<std::uint8_t, 16> requestId) : mRequestId(requestId)
{
    constexpr wchar_t HEX[] = L"0123456789abcdef";
    std::wstring name = L"rareplay-viewer-";
    for (const auto value : requestId)
    {
        name.push_back(HEX[value >> 4]);
        name.push_back(HEX[value & 15]);
    }
    mPath = gameDirectory / (name + L".bin");
}

bool ViewerControlChannel::ReadOwnedRecord(ViewerControlRecord& outRecord) const
{
    HANDLE file = CreateFileW(mPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    std::array<std::uint8_t, VIEWER_CONTROL_BYTES> bytes{};
    DWORD readBytes = 0;
    const bool read = GetFileSizeEx(file, &size) && size.QuadPart == bytes.size() &&
        ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &readBytes, nullptr) &&
        readBytes == bytes.size();
    CloseHandle(file);
    return read && ParseViewerControl(bytes, outRecord) && outRecord.mRequestId == mRequestId;
}

bool ViewerControlChannel::Initialize()
{
    ViewerControlRecord record;
    mIsReady = ReadOwnedRecord(record) && record.mAction == ViewerControlAction::Ready;
    mInitialFlags = mIsReady ? record.mFlags : 0;
    return mIsReady;
}

bool ViewerControlChannel::RequestRestart(std::uint32_t flags, std::string& outError)
{
    if (mRestartPublished)
    {
        outError.clear();
        return true;
    }
    ViewerControlRecord current;
    if (!mIsReady || !ReadOwnedRecord(current) || current.mAction != ViewerControlAction::Ready)
    {
        outError = "无法重新播放：启动器观看通道不可用，请更新启动器并重新打开回放";
        return false;
    }
    ViewerControlRecord restart{ViewerControlAction::Restart, flags, mRequestId};
    std::array<std::uint8_t, VIEWER_CONTROL_BYTES> bytes{};
    if (!EncodeViewerControl(restart, bytes))
    {
        outError = "无法重新播放：观看选项无效";
        return false;
    }
    // 同目录临时文件保证 ReplaceFile 不跨卷。只有本进程写该 nonce 文件，GUI 不访问它。
    auto temporary = mPath;
    temporary += L".plugin-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        outError = "无法重新播放：不能创建启动器通知文件，当前回放已保留";
        return false;
    }
    DWORD written = 0;
    const bool success = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
        &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    const bool replaced = success && ReplaceFileW(mPath.c_str(), temporary.c_str(), nullptr,
        0, nullptr, nullptr);
    if (!replaced)
    {
        DeleteFileW(temporary.c_str());
        outError = "无法重新播放：启动器通知写入失败，当前回放已保留";
        return false;
    }
    mRestartPublished = true;
    outError.clear();
    return true;
}
}
