#include "ViewerControlProtocol.h"

#include <algorithm>

namespace rareplay::storage
{
namespace
{
constexpr std::uint32_t VIEWER_CONTROL_MAGIC = 0x43565052;
constexpr std::uint32_t VIEWER_CONTROL_VERSION = 1;

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
    return value;
}

void WriteU32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) noexcept
{
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}
}

bool ParseViewerControl(std::span<const std::uint8_t> bytes, ViewerControlRecord& outRecord) noexcept
{
    outRecord = {};
    if (bytes.size() != VIEWER_CONTROL_BYTES || ReadU32(bytes, 0) != VIEWER_CONTROL_MAGIC ||
        ReadU32(bytes, 4) != VIEWER_CONTROL_VERSION || ReadU32(bytes, 8) > 1 ||
        (ReadU32(bytes, 12) & ~VIEWER_SHROUD_HALF) != 0)
        return false;
    outRecord.mAction = static_cast<ViewerControlAction>(ReadU32(bytes, 8));
    outRecord.mFlags = ReadU32(bytes, 12);
    std::copy_n(bytes.begin() + 16, 16, outRecord.mRequestId.begin());
    return true;
}

bool EncodeViewerControl(const ViewerControlRecord& record,
    std::array<std::uint8_t, VIEWER_CONTROL_BYTES>& outBytes) noexcept
{
    outBytes = {};
    if (static_cast<std::uint32_t>(record.mAction) > 1 ||
        (record.mFlags & ~VIEWER_SHROUD_HALF) != 0)
        return false;
    WriteU32(outBytes, 0, VIEWER_CONTROL_MAGIC);
    WriteU32(outBytes, 4, VIEWER_CONTROL_VERSION);
    WriteU32(outBytes, 8, static_cast<std::uint32_t>(record.mAction));
    WriteU32(outBytes, 12, record.mFlags);
    std::copy(record.mRequestId.begin(), record.mRequestId.end(), outBytes.begin() + 16);
    return true;
}
}
