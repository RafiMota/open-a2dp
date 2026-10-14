// SPDX-License-Identifier: MIT
#include "settings_store.h"

#include <windows.h>

#include <cwchar>
#include <vector>

#include "log.h"

namespace oa2dp {
namespace {

const wchar_t kDevicesKey[] = L"SOFTWARE\\OpenA2DP\\Devices\\";
const wchar_t kStatusKey[] = L"SOFTWARE\\OpenA2DP\\Status\\";

std::wstring widen(const std::string &s)
{
    return std::wstring(s.begin(), s.end());  // values are ASCII
}

void set_sz(HKEY k, const wchar_t *name, const std::wstring &v)
{
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(v.c_str()),
                   static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

void set_dw(HKEY k, const wchar_t *name, uint32_t v)
{
    DWORD d = v;
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&d), sizeof(d));
}

}  // namespace

std::wstring address_key(uint64_t a)
{
    wchar_t buf[16];
    swprintf(buf, 16, L"%012llX", static_cast<unsigned long long>(a & 0xFFFFFFFFFFFFull));
    return buf;
}

oa2dp_settings load_settings(uint64_t bt_addr)
{
    oa2dp_settings s;
    oa2dp_settings_default(&s);

    std::wstring path = kDevicesKey + address_key(bt_addr);
    DWORD type = 0, bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"Settings", RRF_RT_REG_SZ, &type,
                     nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
        return s;
    std::vector<wchar_t> w(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"Settings", RRF_RT_REG_SZ, &type,
                     w.data(), &bytes) != ERROR_SUCCESS)
        return s;
    std::string text;
    for (wchar_t c : w) {
        if (!c)
            break;
        text.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    int bad = 0;
    oa2dp_settings parsed = s;
    if (oa2dp_settings_parse(&parsed, text.c_str(), &bad) != OA2DP_OK) {
        LOGW("settings for %ls: invalid line %d, using defaults", address_key(bt_addr).c_str(),
             bad);
        return s;
    }
    return parsed;
}

bool save_settings(uint64_t bt_addr, const oa2dp_settings &s)
{
    char text[1024];
    if (oa2dp_settings_format(&s, text, sizeof(text)) < 0)
        return false;
    std::wstring path = kDevicesKey + address_key(bt_addr);
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &k, nullptr) != ERROR_SUCCESS)
        return false;
    set_sz(k, L"Settings", widen(text));
    RegCloseKey(k);
    return true;
}

void publish_status(uint64_t bt_addr, const LiveStatus &st)
{
    std::wstring path = kStatusKey + address_key(bt_addr);
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    set_sz(k, L"State", widen(st.state));
    set_sz(k, L"Codec", widen(st.codec));
    set_dw(k, L"SampleRate", st.sample_rate);
    set_dw(k, L"Channels", st.channels);
    set_dw(k, L"Bits", st.bits);
    set_dw(k, L"Bitrate", st.bitrate);
    set_dw(k, L"AbrLevel", st.abr_level);
    set_dw(k, L"DelayReportUs", st.delay_report_us);
    RegCloseKey(k);
}

void clear_status(uint64_t bt_addr)
{
    std::wstring path = kStatusKey + address_key(bt_addr);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path.c_str());
}

}  // namespace oa2dp
