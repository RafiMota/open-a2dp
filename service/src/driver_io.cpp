// SPDX-License-Identifier: MIT
#include <windows.h>
#include <winioctl.h>
// After the system headers, so only the GUIDs from oa2dp_ioctl.h are
// instantiated in this translation unit.
#include <initguid.h>

#include "driver_io.h"

#include <cfgmgr32.h>

#include <algorithm>
#include <cwctype>

#include "log.h"
#include "settings_store.h"

namespace oa2dp {

std::vector<std::wstring> enumerate_interfaces(const GUID &guid)
{
    std::vector<std::wstring> result;
    for (int attempt = 0; attempt < 3; attempt++) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID *>(&guid), nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
            CR_SUCCESS)
            return result;
        std::vector<wchar_t> buf(len + 1, L'\0');
        CONFIGRET cr = CM_Get_Device_Interface_ListW(const_cast<GUID *>(&guid), nullptr,
                                                     buf.data(), len,
                                                     CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_BUFFER_SMALL)
            continue;  // device arrived between the two calls
        if (cr != CR_SUCCESS)
            return result;
        for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1)
            result.emplace_back(p);
        return result;
    }
    return result;
}

// --------------------------------------------------------------------------

bool DriverHandle::open(const std::wstring &path)
{
    close();
    h_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                     FILE_FLAG_OVERLAPPED, nullptr);
    return h_ != INVALID_HANDLE_VALUE;
}

void DriverHandle::close()
{
    if (h_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(h_, nullptr);
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
}

void DriverHandle::cancel_all()
{
    if (h_ != INVALID_HANDLE_VALUE)
        CancelIoEx(h_, nullptr);
}

DWORD DriverHandle::ioctl(DWORD code, const void *in, DWORD in_len, void *out, DWORD out_len,
                          DWORD *bytes, DWORD timeout_ms, HANDLE cancel)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return GetLastError();
    DWORD n = 0, err = ERROR_SUCCESS;
    if (!DeviceIoControl(h_, code, const_cast<void *>(in), in_len, out, out_len, nullptr, &ov)) {
        err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            HANDLE waits[2] = {ov.hEvent, cancel};
            DWORD w = WaitForMultipleObjects(cancel ? 2 : 1, waits, FALSE, timeout_ms);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &n, TRUE);  // wait for the cancel to land
                CloseHandle(ov.hEvent);
                return w == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_CANCELLED;
            }
            err = ERROR_SUCCESS;
        }
    }
    if (err == ERROR_SUCCESS && !GetOverlappedResult(h_, &ov, &n, FALSE))
        err = GetLastError();
    CloseHandle(ov.hEvent);
    if (bytes)
        *bytes = n;
    return err;
}

// --------------------------------------------------------------------------

bool Transport::open(const std::wstring &path)
{
    if (!dev_.open(path))
        return false;
    for (auto &s : slots_) {
        s.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!s.event)
            return false;
    }
    return true;
}

void Transport::close()
{
    drain_media();
    dev_.close();
    for (auto &s : slots_) {
        if (s.event)
            CloseHandle(s.event);
        s.event = nullptr;
        s.busy = false;
    }
}

bool Transport::get_info(OA2DP_DEVICE_INFO &out)
{
    DWORD n = 0;
    return dev_.ioctl(IOCTL_OA2DP_GET_INFO, nullptr, 0, &out, sizeof(out), &n, 5000) ==
               ERROR_SUCCESS &&
           n == sizeof(out);
}

bool Transport::open_channel(OA2DP_CHANNEL_KIND kind, OA2DP_CHANNEL_INFO &out)
{
    OA2DP_OPEN_CHANNEL_IN in{};
    in.Kind = kind;
    DWORD n = 0;
    // L2CAP connection setup can take a few seconds (paging, auth).
    DWORD err = dev_.ioctl(IOCTL_OA2DP_OPEN_CHANNEL, &in, sizeof(in), &out, sizeof(out), &n, 15000);
    if (err != ERROR_SUCCESS)
        LOGW("open %s channel failed: %lu", kind == Oa2dpChannelMedia ? "media" : "signalling",
             err);
    return err == ERROR_SUCCESS && n == sizeof(out);
}

bool Transport::close_channel(OA2DP_CHANNEL_KIND kind)
{
    OA2DP_CLOSE_CHANNEL_IN in{};
    in.Kind = kind;
    return dev_.ioctl(IOCTL_OA2DP_CLOSE_CHANNEL, &in, sizeof(in), nullptr, 0, nullptr, 5000) ==
           ERROR_SUCCESS;
}

bool Transport::send(OA2DP_CHANNEL_KIND kind, const uint8_t *data, size_t len)
{
    OA2DP_SEND_IN in{};
    in.Kind = kind;
    return dev_.ioctl(IOCTL_OA2DP_SEND, &in, sizeof(in), const_cast<uint8_t *>(data),
                      static_cast<DWORD>(len), nullptr, 5000) == ERROR_SUCCESS;
}

int Transport::recv(OA2DP_CHANNEL_KIND kind, uint8_t *buf, size_t cap, DWORD timeout_ms,
                    HANDLE cancel)
{
    OA2DP_RECV_IN in{};
    in.Kind = kind;
    DWORD n = 0;
    DWORD err = dev_.ioctl(IOCTL_OA2DP_RECV, &in, sizeof(in), buf, static_cast<DWORD>(cap), &n,
                           timeout_ms, cancel);
    if (err == WAIT_TIMEOUT)
        return 0;
    if (err != ERROR_SUCCESS)
        return -1;
    return static_cast<int>(n);
}

bool Transport::wait_event(OA2DP_EVENT &ev, HANDLE cancel)
{
    DWORD n = 0;
    return dev_.ioctl(IOCTL_OA2DP_WAIT_EVENT, nullptr, 0, &ev, sizeof(ev), &n, INFINITE,
                      cancel) == ERROR_SUCCESS &&
           n == sizeof(ev);
}

bool Transport::tx_state(OA2DP_TX_STATE &out)
{
    DWORD n = 0;
    return dev_.ioctl(IOCTL_OA2DP_GET_TX_STATE, nullptr, 0, &out, sizeof(out), &n, 1000) ==
               ERROR_SUCCESS &&
           n == sizeof(out);
}

bool Transport::finish_slot(Slot &s, DWORD timeout_ms)
{
    if (!s.busy)
        return true;
    if (WaitForSingleObject(s.event, timeout_ms) != WAIT_OBJECT_0) {
        CancelIoEx(dev_.get(), &s.ov);
    }
    DWORD n = 0;
    BOOL ok = GetOverlappedResult(dev_.get(), &s.ov, &n, TRUE);
    s.busy = false;
    return ok != FALSE;
}

bool Transport::send_media(const uint8_t *data, size_t len)
{
    Slot &s = slots_[next_slot_];
    next_slot_ = (next_slot_ + 1) % kSlots;
    // Oldest send must finish before its slot is reused; 1 s is far beyond
    // any healthy link and means the headset is gone.
    if (!finish_slot(s, 1000))
        LOGD("media send completed with error");

    static OA2DP_SEND_IN in = {Oa2dpChannelMedia};
    s.data.assign(data, data + len);
    s.ov = OVERLAPPED{};
    s.ov.hEvent = s.event;
    ResetEvent(s.event);
    if (!DeviceIoControl(dev_.get(), IOCTL_OA2DP_SEND, &in, sizeof(in), s.data.data(),
                         static_cast<DWORD>(len), nullptr, &s.ov)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING)
            return false;
    }
    s.busy = true;
    return true;
}

void Transport::drain_media()
{
    if (!dev_.is_open())
        return;
    for (auto &s : slots_)
        finish_slot(s, 1000);
}

// --------------------------------------------------------------------------

bool AudioEndpoint::open(uint64_t bt_addr)
{
    std::wstring want = address_key(bt_addr);
    for (const auto &path : enumerate_interfaces(GUID_DEVINTERFACE_OA2DP_AUDIO)) {
        std::wstring upper = path;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
        if (upper.find(L"#" + want + L"#") != std::wstring::npos)
            return dev_.open(path);
    }
    return false;
}

void AudioEndpoint::close()
{
    unmap_ring();
    dev_.close();
}

bool AudioEndpoint::set_formats(const OA2DP_FORMAT_LIST &list)
{
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_FORMATS, &list, sizeof(list), nullptr, 0, nullptr,
                      5000) == ERROR_SUCCESS;
}

bool AudioEndpoint::set_latency(uint32_t transport_us, uint32_t device_us)
{
    OA2DP_LATENCY l{transport_us, device_us};
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_LATENCY, &l, sizeof(l), nullptr, 0, nullptr, 5000) ==
           ERROR_SUCCESS;
}

bool AudioEndpoint::set_name(const std::wstring &name)
{
    OA2DP_ENDPOINT_NAME n{};
    wcsncpy(n.Name, name.c_str(), ARRAYSIZE(n.Name) - 1);
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_NAME, &n, sizeof(n), nullptr, 0, nullptr, 5000) ==
           ERROR_SUCCESS;
}

bool AudioEndpoint::map_ring()
{
    OA2DP_RING_MAPPING m{};
    DWORD n = 0;
    if (dev_.ioctl(IOCTL_OA2DP_AUDIO_MAP_RING, nullptr, 0, &m, sizeof(m), &n, 5000) !=
            ERROR_SUCCESS ||
        n != sizeof(m))
        return false;
    auto *r = reinterpret_cast<oa2dp_pcm_ring *>(static_cast<uintptr_t>(m.Address));
    if (oa2dp_ring_validate(r, static_cast<size_t>(m.Bytes)) != OA2DP_OK) {
        LOGE("audio driver returned an invalid PCM ring");
        dev_.ioctl(IOCTL_OA2DP_AUDIO_UNMAP_RING, nullptr, 0, nullptr, 0, nullptr, 5000);
        return false;
    }
    ring_ = r;
    return true;
}

void AudioEndpoint::unmap_ring()
{
    if (ring_ && dev_.is_open())
        dev_.ioctl(IOCTL_OA2DP_AUDIO_UNMAP_RING, nullptr, 0, nullptr, 0, nullptr, 5000);
    ring_ = nullptr;
}

bool AudioEndpoint::wait_format(OA2DP_FORMAT_CHANGE &out, HANDLE cancel)
{
    DWORD n = 0;
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_WAIT_FORMAT, nullptr, 0, &out, sizeof(out), &n, INFINITE,
                      cancel) == ERROR_SUCCESS &&
           n == sizeof(out);
}

}  // namespace oa2dp
