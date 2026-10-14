// SPDX-License-Identifier: MIT
#include "manage.h"

#include <bluetoothapis.h>
#include <newdev.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

extern "C" {
#include "oa2dp/a2dp_codecs.h"
#include "oa2dp/media.h"
}

namespace oa2dp::manage {

namespace {

const wchar_t kA2dpSinkUuid[] = L"{0000110B-0000-1000-8000-00805F9B34FB}";
const wchar_t kOurService[] = L"oa2dp";
const wchar_t kOurProvider[] = L"OpenA2DP Project";
const wchar_t kDevicesKey[] = L"SOFTWARE\\OpenA2DP\\Devices\\";
const wchar_t kStatusKey[] = L"SOFTWARE\\OpenA2DP\\Status\\";

std::wstring upper(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towupper);
    return s;
}

std::wstring reg_string(HDEVINFO set, SP_DEVINFO_DATA &d, DWORD prop)
{
    wchar_t buf[1024] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, prop, nullptr, reinterpret_cast<BYTE *>(buf),
                                           sizeof(buf) - sizeof(wchar_t), nullptr))
        return L"";
    return buf;
}

// Instance IDs of BTHENUM service nodes end in "..._<12 hex address>_C<n>".
unsigned long long parse_address(const std::wstring &instance)
{
    std::wstring u = upper(instance);
    size_t c = u.rfind(L"_C");
    if (c == std::wstring::npos || c < 12)
        return 0;
    std::wstring hex = u.substr(c - 12, 12);
    if (hex.find_first_not_of(L"0123456789ABCDEF") != std::wstring::npos)
        return 0;
    return std::wcstoull(hex.c_str(), nullptr, 16);
}

}  // namespace

bool Headset::ours() const
{
    return _wcsicmp(service.c_str(), kOurService) == 0;
}

std::wstring addr_str(unsigned long long a)
{
    wchar_t b[16];
    swprintf(b, 16, L"%012llX", a);
    return b;
}

std::string ascii(const wchar_t *s)
{
    std::string out;
    for (; *s; s++)
        out.push_back(*s < 128 ? static_cast<char>(*s) : '?');
    return out;
}

std::wstring widen(const char *s)
{
    return std::wstring(s, s + strlen(s));
}

DeviceList::DeviceList()
{
    set_ = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES);
    if (set_ == INVALID_HANDLE_VALUE)
        return;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set_, i, &d); i++) {
        wchar_t ids[2048] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set_, &d, SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<BYTE *>(ids),
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr))
            continue;
        bool sink = false;
        for (const wchar_t *p = ids; *p; p += wcslen(p) + 1)
            if (upper(p).find(kA2dpSinkUuid) != std::wstring::npos)
                sink = true;
        if (!sink)
            continue;
        Headset h;
        h.dev = d;
        wchar_t inst[512] = {};
        SetupDiGetDeviceInstanceIdW(set_, &d, inst, 511, nullptr);
        h.instance = inst;
        h.address = parse_address(h.instance);
        h.service = reg_string(set_, d, SPDRP_SERVICE);
        BLUETOOTH_DEVICE_INFO bdi{};
        bdi.dwSize = sizeof(bdi);
        bdi.Address.ullLong = h.address;
        if (h.address && BluetoothGetDeviceInfo(nullptr, &bdi) == ERROR_SUCCESS)
            h.name = bdi.szName;
        if (h.name.empty())
            h.name = reg_string(set_, d, SPDRP_FRIENDLYNAME);
        items.push_back(h);
    }
}

DeviceList::~DeviceList()
{
    if (set_ != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(set_);
}

Headset *DeviceList::find(const std::wstring &arg)
{
    std::wstring a = upper(arg);
    a.erase(std::remove(a.begin(), a.end(), L':'), a.end());
    if (a.size() <= 3 && a.find_first_not_of(L"0123456789") == std::wstring::npos) {
        size_t idx = std::wcstoul(a.c_str(), nullptr, 10);
        return idx < items.size() ? &items[idx] : nullptr;
    }
    unsigned long long addr = std::wcstoull(a.c_str(), nullptr, 16);
    for (auto &h : items)
        if (h.address == addr)
            return &h;
    return nullptr;
}

namespace {

// Newest compatible driver for the device from our package (ours) or from
// Microsoft. Leaves the driver info list built; the caller destroys it.
bool pick_driver(HDEVINFO set, SP_DEVINFO_DATA &dev, bool ours, SP_DRVINFO_DATA_W *chosen)
{
    SP_DEVINSTALL_PARAMS_W params{};
    params.cbSize = sizeof(params);
    if (!SetupDiGetDeviceInstallParamsW(set, &dev, &params))
        return false;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    SetupDiSetDeviceInstallParamsW(set, &dev, &params);
    if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER))
        return false;

    SP_DRVINFO_DATA_W drv{};
    drv.cbSize = chosen->cbSize = sizeof(drv);
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDriverInfoW(set, &dev, SPDIT_COMPATDRIVER, i, &drv); i++) {
        bool is_ours = _wcsicmp(drv.ProviderName, kOurProvider) == 0;
        bool is_ms = wcsstr(drv.ProviderName, L"Microsoft") != nullptr;
        if ((ours && is_ours) || (!ours && is_ms)) {
            if (!found || CompareFileTime(&drv.DriverDate, &chosen->DriverDate) > 0)
                *chosen = drv;
            found = true;
        }
    }
    return found;
}

}  // namespace

bool driver_available(HDEVINFO set, SP_DEVINFO_DATA &dev, bool ours)
{
    SP_DRVINFO_DATA_W chosen{};
    bool found = pick_driver(set, dev, ours, &chosen);
    SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
    return found;
}

bool install_driver(HDEVINFO set, SP_DEVINFO_DATA &dev, bool ours, std::wstring *err,
                    bool *reboot)
{
    auto fail = [&](const std::wstring &msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (reboot)
        *reboot = false;
    SP_DRVINFO_DATA_W chosen{};
    if (!pick_driver(set, dev, ours, &chosen)) {
        SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
        return fail(ours ? L"OpenA2DP driver package is not installed (run the installer)"
                         : L"Windows' own A2DP driver was not found for this device");
    }
    // Resolved at run time: some SDKs ship newdev import libraries without it.
    using DiInstallDeviceFn = BOOL(WINAPI *)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W,
                                             DWORD, PBOOL);
    HMODULE newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto install = newdev ? reinterpret_cast<DiInstallDeviceFn>(
                                reinterpret_cast<void *>(GetProcAddress(newdev, "DiInstallDevice")))
                          : nullptr;
    if (!install) {
        if (newdev)
            FreeLibrary(newdev);
        SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
        return fail(L"newdev.dll!DiInstallDevice not available");
    }
    BOOL need_reboot = FALSE;
    BOOL ok = install(nullptr, set, &dev, &chosen, 0, &need_reboot);
    DWORD e = GetLastError();
    FreeLibrary(newdev);
    SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
    if (!ok)
        return fail(L"DiInstallDevice failed (" + std::to_wstring(e) + L")" +
                    (e == ERROR_ACCESS_DENIED ? L": run as administrator" : L""));
    if (reboot)
        *reboot = need_reboot != FALSE;
    return true;
}

oa2dp_settings load_settings(unsigned long long addr)
{
    oa2dp_settings s;
    oa2dp_settings_default(&s);
    std::wstring path = kDevicesKey + addr_str(addr);
    wchar_t buf[2048] = {};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"Settings", RRF_RT_REG_SZ, nullptr, buf,
                     &bytes) == ERROR_SUCCESS) {
        // Parse into a copy: an invalid line leaves the defaults, as the
        // service does.
        oa2dp_settings parsed = s;
        if (oa2dp_settings_parse(&parsed, ascii(buf).c_str(), nullptr) == OA2DP_OK)
            s = parsed;
    }
    return s;
}

LSTATUS save_settings(unsigned long long addr, const oa2dp_settings &s)
{
    char text[1024];
    if (oa2dp_settings_format(&s, text, sizeof(text)) < 0)
        return ERROR_INSUFFICIENT_BUFFER;
    std::wstring w = widen(text);
    std::wstring path = kDevicesKey + addr_str(addr);
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"Settings", REG_SZ, w.c_str(),
                           static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
}

LSTATUS reset_settings(unsigned long long addr)
{
    std::wstring path = kDevicesKey + addr_str(addr);
    LSTATUS st = RegDeleteKeyW(HKEY_LOCAL_MACHINE, path.c_str());
    return st == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : st;
}

bool read_status(unsigned long long addr, LiveStatus *st)
{
    std::wstring path = kStatusKey + addr_str(addr);
    wchar_t text[64] = {};
    DWORD n = sizeof(text) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"State", RRF_RT_REG_SZ, nullptr, text,
                     &n) != ERROR_SUCCESS)
        return false;
    st->state = text;
    text[0] = 0;
    n = sizeof(text) - sizeof(wchar_t);
    RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"Codec", RRF_RT_REG_SZ, nullptr, text, &n);
    st->codec = text;
    auto dw = [&](const wchar_t *name, DWORD &v) {
        DWORD sz = sizeof(v);
        v = 0;
        RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    };
    dw(L"SampleRate", st->sample_rate);
    dw(L"Channels", st->channels);
    dw(L"Bits", st->bits);
    dw(L"Bitrate", st->bitrate);
    dw(L"AbrLevel", st->abr_level);
    dw(L"DelayReportUs", st->delay_us);
    return true;
}

bool sbc_rate(oa2dp_chmode mode, unsigned bitpool, unsigned rate, unsigned blocks,
              unsigned subbands, SbcFrame *out)
{
    oa2dp_sbc_caps c{};
    c.chmode = mode == OA2DP_CHMODE_MONO     ? OA2DP_SBC_CH_MONO
               : mode == OA2DP_CHMODE_DUAL   ? OA2DP_SBC_CH_DUAL
               : mode == OA2DP_CHMODE_STEREO ? OA2DP_SBC_CH_STEREO
                                             : OA2DP_SBC_CH_JOINT;
    c.blocks = blocks == 4    ? OA2DP_SBC_BLOCKS_4
               : blocks == 8  ? OA2DP_SBC_BLOCKS_8
               : blocks == 12 ? OA2DP_SBC_BLOCKS_12
                              : OA2DP_SBC_BLOCKS_16;
    c.subbands = subbands == 4 ? OA2DP_SBC_SUBBANDS_4 : OA2DP_SBC_SUBBANDS_8;
    c.freq = rate == 16000   ? OA2DP_SBC_FREQ_16000
             : rate == 32000 ? OA2DP_SBC_FREQ_32000
             : rate == 48000 ? OA2DP_SBC_FREQ_48000
                             : OA2DP_SBC_FREQ_44100;
    oa2dp_sbc_layout l;
    if (oa2dp_sbc_layout_from_config(&c, &l) != OA2DP_OK)
        return false;
    out->max_bitpool = oa2dp_sbc_max_bitpool(c.chmode, c.subbands);
    out->bitpool = bitpool ? bitpool : oa2dp_sbc_auto_bitpool(c.chmode, c.freq);
    out->frame_len = oa2dp_sbc_frame_len(&l, out->bitpool);
    out->bitrate = oa2dp_sbc_bitrate(&l, out->bitpool, rate);
    return true;
}

}  // namespace oa2dp::manage
