#include "../src/storage/ViewerControlChannel.h"

#include <Windows.h>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace
{
using namespace rareplay::storage;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Require(static_cast<bool>(file), "fixture must exist");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size())),
        "fixture write must succeed");
}

void TestProtocol(const std::filesystem::path& fixtures)
{
    const auto ready = Read(fixtures / L"rpvc-ready-shroud50.bin");
    const auto restart = Read(fixtures / L"rpvc-restart-shroud50.bin");
    ViewerControlRecord record;
    Require(ParseViewerControl(ready, record) && record.mAction == ViewerControlAction::Ready &&
        record.mFlags == VIEWER_SHROUD_HALF, "actual C# Ready fixture must parse");
    for (std::size_t index = 0; index < 16; ++index)
        Require(record.mRequestId[index] == index, "raw RequestId byte order must survive");
    record.mAction = ViewerControlAction::Restart;
    std::array<std::uint8_t, VIEWER_CONTROL_BYTES> encoded;
    Require(EncodeViewerControl(record, encoded) &&
        std::equal(encoded.begin(), encoded.end(), restart.begin(), restart.end()),
        "C++ Restart must equal actual C# fixture byte for byte");
    for (const auto offset : {0, 4, 8, 12})
    {
        auto invalid = ready;
        invalid[offset] = 0xFF;
        Require(!ParseViewerControl(invalid, record), "unknown field value must be rejected");
    }
    auto trailing = ready;
    trailing.push_back(0);
    Require(!ParseViewerControl(trailing, record) &&
        !ParseViewerControl(std::span(ready).first(31), record), "only exact 32 bytes accepted");
    record.mFlags = 2;
    Require(!EncodeViewerControl(record, encoded), "unknown flags cannot be encoded");
}

void TestChannel(const std::filesystem::path& fixtures)
{
    const auto directory = std::filesystem::temp_directory_path() /
        (L"rareplay-viewer-test-" + std::to_wstring(GetCurrentProcessId()));
    Require(std::filesystem::create_directory(directory), "test directory must be fresh");
    const auto ready = Read(fixtures / L"rpvc-ready-shroud50.bin");
    const auto restart = Read(fixtures / L"rpvc-restart-shroud50.bin");
    std::array<std::uint8_t, 16> id{};
    for (std::size_t index = 0; index < id.size(); ++index)
        id[index] = static_cast<std::uint8_t>(index);
    ViewerControlChannel channel(directory, id);
    Require(channel.GetPath().filename() == L"rareplay-viewer-000102030405060708090a0b0c0d0e0f.bin" &&
        !channel.Initialize() && !channel.CanRestart(), "absent opt-in cannot restart");
    Write(channel.GetPath(), ready);
    Require(channel.Initialize() && channel.CanRestart() && channel.GetInitialFlags() == 1,
        "matching Ready opts into restart and restores viewing flags");
    std::string error;
    auto wrongOwner = ready;
    wrongOwner[16] ^= 0xFF;
    Write(channel.GetPath(), wrongOwner);
    Require(!channel.RequestRestart(1, error) && !error.empty() && Read(channel.GetPath()) == wrongOwner,
        "changed nonce must never be overwritten");
    Write(channel.GetPath(), ready);
    Require(!channel.RequestRestart(2, error) && Read(channel.GetPath()) == ready,
        "invalid option must keep Ready intact");
    HANDLE lock = CreateFileW(channel.GetPath().c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "replacement lock must open");
    const bool failed = !channel.RequestRestart(1, error);
    CloseHandle(lock);
    Require(failed && !error.empty() && Read(channel.GetPath()) == ready,
        "atomic replacement failure must preserve current Ready");
    Require(channel.RequestRestart(1, error) && error.empty() && Read(channel.GetPath()) == restart,
        "retry after lock release publishes complete Restart atomically");
    Require(channel.RequestRestart(0, error) && Read(channel.GetPath()) == restart,
        "repeat clicks keep the first successful request unchanged");
    ViewerControlChannel reopened(directory, id);
    Require(!reopened.Initialize(), "Restart does not count as launcher Ready");
    std::filesystem::remove(channel.GetPath());
    Require(std::filesystem::is_empty(directory), "failed writes must not leave temporary files");
    std::filesystem::remove(directory);
}
}

int wmain(int argc, wchar_t* argv[])
{
    Require(argc == 2, "fixture directory argument required");
    TestProtocol(argv[1]);
    TestChannel(argv[1]);
}
