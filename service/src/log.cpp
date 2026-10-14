// SPDX-License-Identifier: MIT
// Log to %ProgramData%\OpenA2DP\oa2dpsvc.log (rotated at 4 MB), the
// debugger, and the console in --console mode (F6.4).
#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace oa2dp {
namespace {

std::mutex g_mutex;
FILE *g_file = nullptr;
bool g_console = false;
std::wstring g_path;
constexpr long kMaxBytes = 4 * 1024 * 1024;

void open_file()
{
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base)))
        return;
    std::wstring dir = std::wstring(base) + L"\\OpenA2DP";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    g_path = dir + L"\\oa2dpsvc.log";
    g_file = _wfopen(g_path.c_str(), L"a");
}

void rotate_if_needed()
{
    if (!g_file || ftell(g_file) < kMaxBytes)
        return;
    fclose(g_file);
    std::wstring old = g_path + L".1";
    MoveFileExW(g_path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfopen(g_path.c_str(), L"a");
}

}  // namespace

void log_init(bool console)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_console = console;
    if (!g_file)
        open_file();
}

void log_write(LogLevel level, const char *fmt, ...)
{
    static const char *const names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s %s\n", t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             GetCurrentThreadId(), names[static_cast<int>(level)], msg);

    std::lock_guard<std::mutex> lock(g_mutex);
    OutputDebugStringA(line);
    if (g_console)
        fputs(line, stderr);
    if (g_file && level != LogLevel::Debug) {
        rotate_if_needed();
        if (g_file) {
            fputs(line, g_file);
            fflush(g_file);
        }
    }
}

}  // namespace oa2dp
