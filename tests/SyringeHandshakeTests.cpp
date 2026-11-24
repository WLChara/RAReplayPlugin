#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Syringe.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using HandshakeFunction = HRESULT(__cdecl*)(SyringeHandshakeInfo*);

constexpr DWORD TARGET_FILE_SIZE = 4'816'864;
constexpr DWORD TARGET_TIMESTAMP = 0x3BDF544E;
constexpr DWORD TARGET_CRC32 = 0x1B499086;

void AddU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (int index = 0; index < 4; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
}

void AddU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (int index = 0; index < 8; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
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

bool WriteRequest(const std::filesystem::path& markerPath,
    const std::filesystem::path& replayPath, std::int64_t createdMs)
{
    const std::wstring wide = replayPath.wstring();
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return false;
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr) != size)
        return false;

    std::vector<std::uint8_t> bytes;
    AddU32(bytes, 0x54525052);
    AddU32(bytes, 2);
    AddU32(bytes, 2);
    AddU32(bytes, 0);
    AddU64(bytes, static_cast<std::uint64_t>(createdMs));
    for (int index = 0; index < 16; ++index)
        bytes.push_back(static_cast<std::uint8_t>(index + 1));
    AddU32(bytes, static_cast<std::uint32_t>(utf8.size()));
    AddU32(bytes, 0);
    bytes.insert(bytes.end(), utf8.begin(), utf8.end());
    std::ofstream output(markerPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return static_cast<bool>(output);
}

bool Expect(HRESULT actual, HRESULT expected, const char* description)
{
    if (actual == expected)
        return true;
    std::cerr << description << " returned 0x" << std::hex << actual
        << ", expected 0x" << expected << '\n';
    return false;
}
}

int wmain(int argc, wchar_t** argv)
{
    static_assert(sizeof(void*) == 4, "Syringe handshake host must be x86");
    if (argc != 2)
        return 2;
    const auto markerPath = std::filesystem::current_path() / L"rareplay-runtime.bin";
    const auto replayPath = std::filesystem::current_path() / L"smoke.rpl";
    if (!std::filesystem::exists(std::filesystem::current_path() / L"gamemd.exe") ||
        std::filesystem::exists(markerPath) || std::filesystem::exists(replayPath))
        return 3;

    HMODULE module = LoadLibraryW(argv[1]);
    if (!module)
    {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 4;
    }
    const auto handshake = reinterpret_cast<HandshakeFunction>(
        GetProcAddress(module, "SyringeHandshake"));
    if (!handshake)
        return 5;

    std::array<char, 256> message{};
    SyringeHandshakeInfo info{};
    info.cbSize = sizeof(info);
    info.num_hooks = 30;
    info.exeFilesize = TARGET_FILE_SIZE;
    info.exeTimestamp = TARGET_TIMESTAMP;
    info.exeCRC = TARGET_CRC32;
    info.cchMessage = static_cast<int>(message.size());
    info.Message = message.data();

    if (!Expect(handshake(&info), S_FALSE, "absent marker"))
        return 6;
    if (!WriteRequest(markerPath, replayPath, NowUnixMilliseconds() - 301'000))
        return 7;
    if (!Expect(handshake(&info), S_FALSE, "expired request") ||
        !std::filesystem::exists(markerPath))
        return 8;
    if (!WriteRequest(markerPath, replayPath, NowUnixMilliseconds()))
        return 9;
    info.exeCRC ^= 1;
    if (!Expect(handshake(&info), S_FALSE, "wrong host identity"))
        return 10;
    info.exeCRC = TARGET_CRC32;
    if (!Expect(handshake(&info), S_OK, "valid request") ||
        !std::filesystem::exists(markerPath) || std::filesystem::exists(replayPath))
        return 11;
    FreeLibrary(module);
    std::cout << "SyringeHandshake host boundary passed\n";
    return 0;
}
