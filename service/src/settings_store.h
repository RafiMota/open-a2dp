// SPDX-License-Identifier: MIT
// Per-device settings and live status in the registry:
//   HKLM\SOFTWARE\OpenA2DP\Devices\<ADDR>   Settings (REG_SZ, key=value lines)
//   HKLM\SOFTWARE\OpenA2DP\Status\<ADDR>    volatile, written by the service
#pragma once

#include <cstdint>
#include <string>

#include "oa2dp/settings.h"

namespace oa2dp {

std::wstring address_key(uint64_t bt_addr);  // "001A7DDA7113"

// Missing key or invalid text -> defaults (invalid text is logged).
oa2dp_settings load_settings(uint64_t bt_addr);
bool save_settings(uint64_t bt_addr, const oa2dp_settings &s);

struct LiveStatus {
    std::string state;     // "connecting", "configured", "streaming", ...
    std::string codec;     // "LDAC"
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t bits = 0;
    uint32_t bitrate = 0;  // bit/s, measured
    uint32_t abr_level = 0;
    uint32_t delay_report_us = 0;
};

void publish_status(uint64_t bt_addr, const LiveStatus &st);
void clear_status(uint64_t bt_addr);

}  // namespace oa2dp
