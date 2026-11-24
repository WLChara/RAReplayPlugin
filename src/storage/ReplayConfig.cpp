#include "ReplayConfig.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <fstream>

namespace rareplay::storage
{
namespace
{
constexpr std::uint32_t REQUEST_MAGIC = 0x54525052;
constexpr std::uint32_t REQUEST_VERSION = 2;
constexpr std::size_t REQUEST_HEADER_BYTES = 48;
constexpr std::size_t MAX_REQUEST_BYTES = 17 * 1024 * 1024;
constexpr std::size_t MAX_PATH_BYTES = 32768;
constexpr std::size_t MAX_MANIFEST_BYTES = 16 * 1024 * 1024;
constexpr std::int64_t MAX_REQUEST_AGE_MS = 5 * 60 * 1000;
constexpr std::int64_t MAX_FUTURE_SKEW_MS = 30 * 1000;

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::int64_t ReadI64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8 * index);
    return static_cast<std::int64_t>(value);
}

bool DecodeUtf8(std::span<const std::uint8_t> bytes, std::wstring& outWide)
{
    if (bytes.empty() || std::find(bytes.begin(), bytes.end(), 0) != bytes.end())
        return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
    if (!length)
        return false;
    outWide.resize(length);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
        outWide.data(), length) == length;
}

std::int64_t NowUnixMilliseconds()
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<std::int64_t>(value.QuadPart / 10000ULL) - 11644473600000LL;
}
}

bool ParseReplayRequest(std::span<const std::uint8_t> bytes,
    std::int64_t nowUnixMilliseconds, ReplayRequest& outRequest, std::string& outError)
{
    outRequest = {};
    if (bytes.size() < REQUEST_HEADER_BYTES || bytes.size() > MAX_REQUEST_BYTES ||
        ReadU32(bytes, 0) != REQUEST_MAGIC || ReadU32(bytes, 4) != REQUEST_VERSION ||
        (ReadU32(bytes, 8) != 1 && ReadU32(bytes, 8) != 2) || ReadU32(bytes, 12) != 0)
    {
        outError = "Invalid RPRT V2 header";
        return false;
    }
    const auto created = ReadI64(bytes, 16);
    if (created <= 0 || created > nowUnixMilliseconds + MAX_FUTURE_SKEW_MS ||
        nowUnixMilliseconds - created > MAX_REQUEST_AGE_MS)
    {
        outError = "Replay request expired or has invalid time";
        return false;
    }
    const std::size_t pathBytes = ReadU32(bytes, 40);
    const std::size_t manifestBytes = ReadU32(bytes, 44);
    const bool hasRequestId = std::any_of(bytes.begin() + 24, bytes.begin() + 40,
        [](std::uint8_t value) { return value != 0; });
    if (!hasRequestId || !pathBytes || pathBytes > MAX_PATH_BYTES || manifestBytes > MAX_MANIFEST_BYTES ||
        pathBytes + manifestBytes != bytes.size() - REQUEST_HEADER_BYTES ||
        (ReadU32(bytes, 8) == 1 && manifestBytes == 0))
    {
        outError = "Invalid replay request lengths";
        return false;
    }
    std::wstring path;
    if (!DecodeUtf8(bytes.subspan(REQUEST_HEADER_BYTES, pathBytes), path))
    {
        outError = "Replay path is not valid UTF-8";
        return false;
    }
    const std::filesystem::path replayPath(path);
    if (!replayPath.is_absolute() || replayPath.filename().empty())
    {
        outError = "Replay path must be absolute";
        return false;
    }
    outRequest.mMode = static_cast<ReplayMode>(ReadU32(bytes, 8));
    outRequest.mCreatedUnixMilliseconds = created;
    std::copy_n(bytes.data() + 24, outRequest.mRequestId.size(), outRequest.mRequestId.data());
    outRequest.mReplayPath = replayPath;
    outRequest.mManifest.assign(bytes.begin() + REQUEST_HEADER_BYTES + pathBytes, bytes.end());
    outError.clear();
    return true;
}

bool ReadReplayRequest(const std::filesystem::path& markerPath,
    ReplayRequest& outRequest, std::vector<std::uint8_t>& outRawBytes,
    std::string& outError)
{
    outRawBytes.clear();
    std::ifstream input(markerPath, std::ios::binary | std::ios::ate);
    if (!input)
    {
        outError = "Replay request absent";
        return false;
    }
    const auto length = input.tellg();
    if (length < static_cast<std::streamoff>(REQUEST_HEADER_BYTES) ||
        length > static_cast<std::streamoff>(MAX_REQUEST_BYTES))
    {
        outError = "Replay request size invalid";
        return false;
    }
    outRawBytes.resize(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(outRawBytes.data()), length))
    {
        outError = "Cannot read replay request";
        return false;
    }
    return ParseReplayRequest(outRawBytes, NowUnixMilliseconds(), outRequest, outError);
}

bool ConsumeReplayRequest(const std::filesystem::path& markerPath,
    std::span<const std::uint8_t> expectedBytes, std::string& outError)
{
    HANDLE file = CreateFileW(markerPath.c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        outError = "Cannot open replay request for consumption";
        return false;
    }
    LARGE_INTEGER size{};
    std::vector<std::uint8_t> current;
    DWORD readBytes = 0;
    bool matches = GetFileSizeEx(file, &size) &&
        size.QuadPart == static_cast<LONGLONG>(expectedBytes.size()) &&
        size.QuadPart <= static_cast<LONGLONG>(MAX_REQUEST_BYTES);
    if (matches)
    {
        current.resize(static_cast<std::size_t>(size.QuadPart));
        matches = ReadFile(file, current.data(), static_cast<DWORD>(current.size()),
            &readBytes, nullptr) && readBytes == current.size() &&
            std::equal(current.begin(), current.end(), expectedBytes.begin());
    }
    ReplayRequest request;
    if (!matches || !ParseReplayRequest(current, NowUnixMilliseconds(), request, outError))
    {
        CloseHandle(file);
        outError = "Replay request changed before consumption";
        return false;
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)))
    {
        CloseHandle(file);
        outError = "Cannot consume replay request";
        return false;
    }
    CloseHandle(file);
    outError.clear();
    return true;
}

bool MatchesArchiveManifest(const ReplayRequest& request,
    std::span<const std::uint8_t> archiveManifest)
{
    return request.mManifest.empty() ||
        std::equal(request.mManifest.begin(), request.mManifest.end(),
            archiveManifest.begin(), archiveManifest.end());
}
}
