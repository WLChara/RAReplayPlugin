#include "TargetImage.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

namespace rareplay::target
{
    namespace
    {
        bool HashFileBytes(const std::vector<std::uint8_t>& bytes,
            std::string& outHash)
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                nullptr, 0) < 0)
                return false;

            std::array<std::uint8_t, 32> digest{};
            const bool success = BCryptCreateHash(algorithm, &hash, nullptr, 0,
                nullptr, 0, 0) >= 0 &&
                BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                    static_cast<ULONG>(bytes.size()), 0) >= 0 &&
                BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
            if (hash)
                BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (!success)
                return false;

            constexpr char HEX[] = "0123456789abcdef";
            outHash.clear();
            for (const auto byte : digest)
            {
                outHash.push_back(HEX[byte >> 4]);
                outHash.push_back(HEX[byte & 15]);
            }
            return true;
        }
    }

    bool VerifyTargetExecutableFile(const std::filesystem::path& path, std::string& outError)
    {
        if (_wcsicmp(path.filename().c_str(), L"gamemd.exe") != 0)
        {
            outError = "Target filename differs from gamemd.exe";
            return false;
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != static_cast<std::streamoff>(TARGET_FILE_SIZE))
        {
            outError = "Target executable size differs";
            return false;
        }
        std::vector<std::uint8_t> bytes(TARGET_FILE_SIZE);
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        {
            outError = "Cannot read target executable";
            return false;
        }
        if (bytes.size() < sizeof(IMAGE_DOS_HEADER))
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew < 0 || static_cast<std::size_t>(dos->e_lfanew) >
                bytes.size() - sizeof(IMAGE_NT_HEADERS32))
        {
            outError = "Target executable PE header invalid";
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->FileHeader.TimeDateStamp != TARGET_PE_TIMESTAMP)
        {
            outError = "Target executable PE identity differs";
            return false;
        }
        std::string hash;
        if (!HashFileBytes(bytes, hash) || hash != TARGET_SHA256)
        {
            outError = "Target executable SHA-256 differs";
            return false;
        }
        outError.clear();
        return true;
    }

    bool TargetImage::LoadCurrentProcess(std::string& outError)
    {
        static_assert(sizeof(void*) == 4, "RAReplayPlugin requires x86");
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(),
            static_cast<DWORD>(path.size()));
        if (!length || length >= path.size())
        {
            outError = "Cannot resolve game executable path";
            return false;
        }
        mPath = path.data();
        if (!VerifyTargetExecutableFile(mPath, outError))
            return false;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (base != TARGET_IMAGE_BASE)
        {
            outError = "Unsupported game image base";
            return false;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            outError = "Game image is not a supported PE32 executable";
            return false;
        }
        outError.clear();
        return true;
    }
}
