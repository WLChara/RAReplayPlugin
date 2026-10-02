#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace rareplay::storage
{
enum class ViewerControlAction : std::uint32_t { Ready, Restart };
constexpr std::uint32_t VIEWER_SHROUD_HALF = 1;
constexpr std::size_t VIEWER_CONTROL_BYTES = 32;

struct ViewerControlRecord
{
    ViewerControlAction mAction = ViewerControlAction::Ready;
    std::uint32_t mFlags = 0;
    std::array<std::uint8_t, 16> mRequestId{};
};

/** RPVC V1 固定小端值协议；未知动作、标志及非精确长度均拒绝。 */
bool ParseViewerControl(std::span<const std::uint8_t> bytes, ViewerControlRecord& outRecord) noexcept;
bool EncodeViewerControl(const ViewerControlRecord& record,
    std::array<std::uint8_t, VIEWER_CONTROL_BYTES>& outBytes) noexcept;
}
