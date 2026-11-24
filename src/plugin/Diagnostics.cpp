#include "Diagnostics.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace rareplay::plugin
{
    namespace
    {
        HANDLE g_LogFile = INVALID_HANDLE_VALUE;
        SRWLOCK g_LogLock = SRWLOCK_INIT;
    }

    bool OpenBootstrapDiagnostics(const std::filesystem::path& gameDirectory,
        std::string& outError)
    {
        const auto debugDirectory = gameDirectory / L"debug";
        if (!CreateDirectoryW(debugDirectory.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
        {
            outError = "Cannot create game debug directory";
            return false;
        }
        const auto path = debugDirectory / L"RAReplayPlugin.log";
        AcquireSRWLockExclusive(&g_LogLock);
        if (g_LogFile != INVALID_HANDLE_VALUE)
        {
            ReleaseSRWLockExclusive(&g_LogLock);
            outError.clear();
            return true;
        }
        g_LogFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool success = g_LogFile != INVALID_HANDLE_VALUE;
        ReleaseSRWLockExclusive(&g_LogLock);
        if (!success)
            outError = "Cannot open diagnostic file";
        else
            outError.clear();
        return success;
    }

    void Log(const char* message) noexcept
    {
        if (!message)
            return;
        AcquireSRWLockExclusive(&g_LogLock);
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
        if (g_LogFile != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME time{};
            GetLocalTime(&time);
            char prefix[64]{};
            const int count = std::snprintf(prefix, sizeof(prefix),
                "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu tid=%lu] ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond, time.wMilliseconds, GetCurrentProcessId(),
                GetCurrentThreadId());
            DWORD written = 0;
            if (count > 0)
                WriteFile(g_LogFile, prefix, static_cast<DWORD>(count), &written, nullptr);
            WriteFile(g_LogFile, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
            WriteFile(g_LogFile, "\r\n", 2, &written, nullptr);
        }
        ReleaseSRWLockExclusive(&g_LogLock);
    }

    void CloseDiagnostics() noexcept
    {
        AcquireSRWLockExclusive(&g_LogLock);
        if (g_LogFile != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(g_LogFile);
            CloseHandle(g_LogFile);
            g_LogFile = INVALID_HANDLE_VALUE;
        }
        ReleaseSRWLockExclusive(&g_LogLock);
    }
}
