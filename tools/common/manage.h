// SPDX-License-Identifier: MIT
// Shared by oa2dpctl and oa2dpgui: find paired A2DP headsets, switch their
// driver, and read/write the registry contract of the service
// (HKLM\SOFTWARE\OpenA2DP\Devices\<addr>\Settings, ...\Status\<addr>).
#ifndef OA2DP_TOOLS_MANAGE_H
#define OA2DP_TOOLS_MANAGE_H

#include <windows.h>
#include <setupapi.h>

#include <string>
#include <vector>

extern "C" {
#include "oa2dp/settings.h"
}

namespace oa2dp::manage {

struct Headset {
    SP_DEVINFO_DATA dev{};
    std::wstring instance;
    std::wstring name;
    std::wstring service;
    unsigned long long address = 0;
    bool ours() const;
};

// "001A7DDA7113"
std::wstring addr_str(unsigned long long a);

// Paired devices that expose the A2DP sink service.
class DeviceList {
public:
    DeviceList();
    ~DeviceList();
    DeviceList(const DeviceList &) = delete;
    DeviceList &operator=(const DeviceList &) = delete;

    HDEVINFO set() const { return set_; }
    // A list index or a Bluetooth address (with or without colons).
    Headset *find(const std::wstring &arg);

    std::vector<Headset> items;

private:
    HDEVINFO set_ = INVALID_HANDLE_VALUE;
};

// Whether a driver of that kind (ours, or Windows' own) is installed on this PC
// and matches the device. Works without administrator rights.
bool driver_available(HDEVINFO set, SP_DEVINFO_DATA &dev, bool ours);

// Install our driver (ours = true) or Windows' own on the device. Needs
// administrator rights. On failure *err says why.
bool install_driver(HDEVINFO set, SP_DEVINFO_DATA &dev, bool ours, std::wstring *err,
                    bool *reboot);

// Settings stored for the headset, or the defaults.
oa2dp_settings load_settings(unsigned long long addr);
// Win32 error code (ERROR_ACCESS_DENIED without administrator rights).
LSTATUS save_settings(unsigned long long addr, const oa2dp_settings &s);
LSTATUS reset_settings(unsigned long long addr);

// What the service publishes while a headset is connected through OpenA2DP.
struct LiveStatus {
    std::wstring state;
    std::wstring codec;
    DWORD sample_rate = 0;
    DWORD channels = 0;
    DWORD bits = 0;
    DWORD bitrate = 0;  // bit/s
    DWORD abr_level = 0;
    DWORD delay_us = 0;
};
// false when the headset is not connected through OpenA2DP.
bool read_status(unsigned long long addr, LiveStatus *st);

// One SBC configuration worked out from settings-level values: channel mode
// (oa2dp_chmode, AUTO = joint stereo), bitpool (0 = the automatic choice for
// this rate), blocks and subbands (0 = 16 / 8).
struct SbcFrame {
    unsigned bitpool = 0;      // effective bitpool
    unsigned max_bitpool = 0;  // most the frame format allows for this layout
    unsigned frame_len = 0;    // bytes
    unsigned bitrate = 0;      // bit/s
};
bool sbc_rate(oa2dp_chmode mode, unsigned bitpool, unsigned rate, unsigned blocks,
              unsigned subbands, SbcFrame *out);

// Lossy UTF-16 -> ASCII (settings text is ASCII).
std::string ascii(const wchar_t *s);
std::wstring widen(const char *s);

}  // namespace oa2dp::manage

#endif
