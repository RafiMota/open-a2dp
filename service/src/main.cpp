// SPDX-License-Identifier: MIT
// oa2dpsvc.exe — runs one Session per headset bound to oa2dp.sys.
//
//   oa2dpsvc.exe             started by the Service Control Manager
//   oa2dpsvc.exe --console   run in a console for debugging (Ctrl+C stops)
#include <windows.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "driver_io.h"
#include "log.h"
#include "session.h"

namespace oa2dp {
namespace {

const wchar_t kServiceName[] = L"oa2dpsvc";

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

class DeviceManager {
public:
    bool start()
    {
        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof(filter);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_OA2DP_TRANSPORT;
        if (CM_Register_Notification(&filter, this, &DeviceManager::on_notify, &notify_) !=
            CR_SUCCESS) {
            LOGE("CM_Register_Notification failed");
            return false;
        }
        for (const auto &path : enumerate_interfaces(GUID_DEVINTERFACE_OA2DP_TRANSPORT))
            add(path);
        return true;
    }

    void stop()
    {
        if (notify_) {
            CM_Unregister_Notification(notify_);
            notify_ = nullptr;
        }
        std::vector<std::unique_ptr<Session>> all;
        {
            std::lock_guard<std::mutex> lock(mu_);
            all.swap(sessions_);
        }
        for (auto &s : all)
            s->stop();
    }

private:
    static DWORD CALLBACK on_notify(HCMNOTIFICATION, PVOID ctx, CM_NOTIFY_ACTION action,
                                    PCM_NOTIFY_EVENT_DATA data, DWORD)
    {
        auto *self = static_cast<DeviceManager *>(ctx);
        std::wstring path = data->u.DeviceInterface.SymbolicLink;
        if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL)
            self->add(path);
        else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
            self->remove(path);
        return ERROR_SUCCESS;
    }

    void add(const std::wstring &path)
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto &s : sessions_)
            if (lower(s->path()) == lower(path))
                return;
        LOGI("headset interface arrived: %ls", path.c_str());
        sessions_.push_back(std::make_unique<Session>(path));
        sessions_.back()->start();
    }

    void remove(const std::wstring &path)
    {
        std::unique_ptr<Session> gone;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const auto &s) {
                return lower(s->path()) == lower(path);
            });
            if (it == sessions_.end())
                return;
            gone = std::move(*it);
            sessions_.erase(it);
        }
        LOGI("headset interface removed: %ls", path.c_str());
        // Stopping joins threads: never do it on the notification thread.
        std::thread([s = std::move(gone)]() mutable { s->stop(); }).detach();
    }

    HCMNOTIFICATION notify_ = nullptr;
    std::mutex mu_;
    std::vector<std::unique_ptr<Session>> sessions_;
};

DeviceManager g_manager;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop = nullptr;

void set_state(DWORD state, DWORD exit_code = NO_ERROR)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI control_handler(DWORD ctrl, DWORD, LPVOID, LPVOID)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        set_state(SERVICE_STOP_PENDING);
        SetEvent(g_stop);
        return NO_ERROR;
    }
    return ctrl == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void run_until_stopped()
{
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_manager.start()) {
        CloseHandle(g_stop);
        return;
    }
    if (g_status_handle)
        set_state(SERVICE_RUNNING);
    WaitForSingleObject(g_stop, INFINITE);
    g_manager.stop();
    CloseHandle(g_stop);
}

void WINAPI service_main(DWORD, LPWSTR *)
{
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
    if (!g_status_handle)
        return;
    set_state(SERVICE_START_PENDING);
    log_init(false);
    LOGI("OpenA2DP service starting");
    run_until_stopped();
    LOGI("OpenA2DP service stopped");
    set_state(SERVICE_STOPPED);
}

BOOL WINAPI console_handler(DWORD)
{
    if (g_stop)
        SetEvent(g_stop);
    return TRUE;
}

}  // namespace
}  // namespace oa2dp

int wmain(int argc, wchar_t **argv)
{
    using namespace oa2dp;
    if (argc > 1 && wcscmp(argv[1], L"--console") == 0) {
        log_init(true);
        SetConsoleCtrlHandler(console_handler, TRUE);
        LOGI("OpenA2DP running in console mode; Ctrl+C to stop");
        run_until_stopped();
        return 0;
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), service_main},
                                    {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
