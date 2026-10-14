// SPDX-License-Identifier: MIT
// User-mode side of the IOCTL contract in driver/common/oa2dp_ioctl.h.
#pragma once

#include <windows.h>
#include <winioctl.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "oa2dp_ioctl.h"
#include "oa2dp/pcm_ring.h"

namespace oa2dp {

std::vector<std::wstring> enumerate_interfaces(const GUID &guid);

// Overlapped handle with a synchronous-with-timeout IOCTL helper.
class DriverHandle {
public:
    DriverHandle() = default;
    ~DriverHandle() { close(); }
    DriverHandle(const DriverHandle &) = delete;
    DriverHandle &operator=(const DriverHandle &) = delete;

    bool open(const std::wstring &path);
    void close();
    bool is_open() const { return h_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return h_; }
    void cancel_all();

    // Returns ERROR_SUCCESS, WAIT_TIMEOUT, ERROR_CANCELLED or a Win32 error.
    DWORD ioctl(DWORD code, const void *in, DWORD in_len, void *out, DWORD out_len,
                DWORD *bytes, DWORD timeout_ms = INFINITE, HANDLE cancel = nullptr);

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

class Transport {
public:
    bool open(const std::wstring &path);
    void close();
    void cancel_all() { dev_.cancel_all(); }

    bool get_info(OA2DP_DEVICE_INFO &out);
    bool open_channel(OA2DP_CHANNEL_KIND kind, OA2DP_CHANNEL_INFO &out);
    bool close_channel(OA2DP_CHANNEL_KIND kind);
    bool send(OA2DP_CHANNEL_KIND kind, const uint8_t *data, size_t len);
    // > 0 bytes, 0 timeout, < 0 channel closed / error
    int recv(OA2DP_CHANNEL_KIND kind, uint8_t *buf, size_t cap, DWORD timeout_ms,
             HANDLE cancel = nullptr);
    bool wait_event(OA2DP_EVENT &ev, HANDLE cancel);
    bool tx_state(OA2DP_TX_STATE &out);

    // Media: up to kSlots sends in flight; blocks when all are busy.
    bool send_media(const uint8_t *data, size_t len);
    void drain_media();

private:
    static constexpr size_t kSlots = 8;
    struct Slot {
        OVERLAPPED ov{};
        HANDLE event = nullptr;
        std::vector<uint8_t> data;
        bool busy = false;
    };
    bool finish_slot(Slot &s, DWORD timeout_ms);

    DriverHandle dev_;
    std::array<Slot, kSlots> slots_{};
    size_t next_slot_ = 0;
};

class AudioEndpoint {
public:
    ~AudioEndpoint() { close(); }
    bool open(uint64_t bt_addr);
    void close();
    bool set_formats(const OA2DP_FORMAT_LIST &list);
    bool set_latency(uint32_t transport_us, uint32_t device_us);
    bool set_name(const std::wstring &name);
    bool map_ring();
    void unmap_ring();
    oa2dp_pcm_ring *ring() const { return ring_; }
    // true with a new format, false on cancel/error
    bool wait_format(OA2DP_FORMAT_CHANGE &out, HANDLE cancel);
    void cancel_all() { dev_.cancel_all(); }

private:
    DriverHandle dev_;
    oa2dp_pcm_ring *ring_ = nullptr;
};

}  // namespace oa2dp
