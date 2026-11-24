#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Syringe.h>

#include "../storage/ReplayConfig.h"
#include "../target/TargetImage.h"

#include <filesystem>
#include <string>
#include <vector>

SYRINGE_HANDSHAKE(info)
{
    try
    {
        if (!info || info->cbSize < sizeof(SyringeHandshakeInfo) ||
            info->exeFilesize != rareplay::target::TARGET_FILE_SIZE ||
            info->exeTimestamp != rareplay::target::TARGET_PE_TIMESTAMP ||
            info->exeCRC != rareplay::target::TARGET_CRC32)
            return S_FALSE;

        // 握手运行在 Syringe 宿主进程，只读磁盘；绝不访问游戏虚拟地址。
        const auto gameDirectory = std::filesystem::current_path();
        std::string error;
        if (!rareplay::target::VerifyTargetExecutableFile(gameDirectory / L"gamemd.exe", error))
            return S_FALSE;
        rareplay::storage::ReplayRequest request;
        std::vector<std::uint8_t> rawBytes;
        if (!rareplay::storage::ReadReplayRequest(
                gameDirectory / L"rareplay-runtime.bin", request, rawBytes, error))
            return S_FALSE;
        return S_OK;
    }
    catch (...)
    {
        return S_FALSE;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
