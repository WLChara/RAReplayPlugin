#pragma once

#include "ReplayPorts.h"

#include <array>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace rareplay::replay
{
class FrameCodec
{
public:
    static constexpr std::size_t HEADER_BYTES = 32;
    static constexpr std::uint32_t MAX_EVENTS_PER_FRAME = 16'384;
    static constexpr std::uint32_t MAX_INPUT_EFFECTS_PER_FRAME = 4'096;

    static bool EncodeHeader(const ReplaySessionInfo& info,
        std::array<std::uint8_t, HEADER_BYTES>& outBytes, std::string& error);
    static bool DecodeHeader(std::istream& stream, ReplaySessionInfo& outInfo, std::string& error);
    static bool EncodeFrame(const ReplayFrame& frame, std::vector<std::uint8_t>& outBytes,
        std::string& error, std::uint32_t streamVersion = 2);
    static ReplayReadResult ReadFrame(std::istream& stream, ReplayFrame& outFrame,
        std::string& error, std::uint32_t streamVersion = 2);
};
}
