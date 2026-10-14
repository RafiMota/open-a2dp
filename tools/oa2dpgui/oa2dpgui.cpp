// SPDX-License-Identifier: MIT
// oa2dpgui — graphical manager for OpenA2DP (SPEC F6.1).
//
// Left: paired A2DP headsets and the driver each one uses, with a button to
// switch it. Right: the codec settings of the selected headset and the live
// SBC bitrate. Bottom: live status published by the service.
//
// The window runs without administrator rights. Writes to HKLM and driver
// switches go through oa2dpctl.exe (next to this program) started elevated,
// or are done in-process when the manager itself runs elevated. Device and
// registry logic is shared with oa2dpctl (tools/common/manage.cpp).
#include "../common/manage.h"

extern "C" {
#include "oa2dp/a2dp_codecs.h"
}

#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace oa2dp::manage;

namespace {

enum : int {
    IDC_DEVICES = 100,
    IDC_SWITCH,
    IDC_REFRESH,
    IDC_DRIVER_NOTE,
    IDC_CODECS,
    IDC_UP,
    IDC_DOWN,
    IDC_RATE,
    IDC_BITS,
    IDC_CHMODE,
    IDC_BITPOOL,
    IDC_BITPOOL_SPIN,
    IDC_BITPOOL_AUTO,
    IDC_BLOCKS,
    IDC_SUBBANDS,
    IDC_ALLOC,
    IDC_SBC_RATE,
    IDC_AAC_RATE,
    IDC_AAC_VBR,
    IDC_LDAC,
    IDC_ABR,
    IDC_FOLLOW,
    IDC_DEFAULTS,
    IDC_UNDO,
    IDC_SAVE,
    IDC_STATUS,
};
enum { kTimerStatus = 1, kTimerRescan = 2 };

const wchar_t kTitle[] = L"OpenA2DP Manager";

const wchar_t *const kCodecNames[OA2DP_CODEC_COUNT] = {
    L"SBC", L"AAC", L"aptX", L"aptX HD", L"aptX Low Latency", L"FastStream", L"LDAC"};

// Settings controls, disabled while no headset is selected.
const int kPanel[] = {IDC_CODECS,   IDC_UP,       IDC_DOWN,      IDC_RATE,
                      IDC_BITS,     IDC_CHMODE,   IDC_BITPOOL,   IDC_BITPOOL_SPIN,
                      IDC_BITPOOL_AUTO, IDC_BLOCKS, IDC_SUBBANDS, IDC_ALLOC,
                      IDC_AAC_RATE, IDC_AAC_VBR,  IDC_LDAC,      IDC_ABR,
                      IDC_FOLLOW,   IDC_DEFAULTS};

struct App {
    HWND wnd = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    bool elevated = false;
    std::unique_ptr<DeviceList> devs;
    bool has_sel = false;
    unsigned long long sel_addr = 0;
    oa2dp_settings saved{};  // what the registry holds for the selected headset
    bool loading = false;    // suppress change notifications while filling controls
    std::wstring last_status;
};
App g;

int S(int v)
{
    return MulDiv(v, g.dpi, 96);
}

HWND item(int id)
{
    return GetDlgItem(g.wnd, id);
}

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
          int id, DWORD ex = 0)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), FALSE);
    return c;
}

void label(const wchar_t *text, int x, int y, int w, int h = 18, int id = -1)
{
    make(WC_STATICW, text, SS_LEFT, x, y, w, h, id);
}

void group(const wchar_t *text, int x, int y, int w, int h)
{
    make(WC_BUTTONW, text, BS_GROUPBOX, x, y, w, h, -1);
}

void button(const wchar_t *text, int id, int x, int y, int w, int h = 28)
{
    make(WC_BUTTONW, text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id);
}

void check(const wchar_t *text, int id, int x, int y, int w)
{
    make(WC_BUTTONW, text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, id);
}

void combo(int id, int x, int y, int w)
{
    make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 240, id);
}

// --- combo boxes holding a numeric value per entry ------------------------

void combo_add(int id, const wchar_t *text, LPARAM value)
{
    HWND c = item(id);
    LRESULT i = SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(c, CB_SETITEMDATA, i, value);
}

void combo_set(int id, LPARAM value)
{
    HWND c = item(id);
    LRESULT n = SendMessageW(c, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < n; i++)
        if (SendMessageW(c, CB_GETITEMDATA, i, 0) == value) {
            SendMessageW(c, CB_SETCURSEL, i, 0);
            return;
        }
    // A value written by hand or by a newer version: show it as it is.
    combo_add(id, std::to_wstring(value).c_str(), value);
    SendMessageW(c, CB_SETCURSEL, n, 0);
}

LPARAM combo_get(int id)
{
    HWND c = item(id);
    LRESULT i = SendMessageW(c, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : SendMessageW(c, CB_GETITEMDATA, i, 0);
}

bool checked(int id)
{
    return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void set_checked(int id, bool on)
{
    SendMessageW(item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

std::wstring khz(unsigned rate)
{
    wchar_t b[32];
    if (rate % 1000 == 0)
        swprintf(b, 32, L"%u kHz", rate / 1000);
    else
        swprintf(b, 32, L"%.1f kHz", rate / 1000.0);
    return b;
}

std::wstring pretty_addr(unsigned long long a)
{
    std::wstring s = addr_str(a), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && i % 2 == 0)
            out.push_back(L':');
        out.push_back(s[i]);
    }
    return out;
}

// --- codec order list ----------------------------------------------------

std::vector<std::pair<oa2dp_codec_id, bool>> codec_rows()
{
    HWND lv = item(IDC_CODECS);
    std::vector<std::pair<oa2dp_codec_id, bool>> rows;
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; i++) {
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(lv, &it);
        rows.emplace_back(static_cast<oa2dp_codec_id>(it.lParam),
                          ListView_GetCheckState(lv, i) != 0);
    }
    return rows;
}

void set_codec_rows(const std::vector<std::pair<oa2dp_codec_id, bool>> &rows, int select)
{
    HWND lv = item(IDC_CODECS);
    bool was_loading = g.loading;
    g.loading = true;
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < rows.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t *>(kCodecNames[rows[i].first]);
        it.lParam = rows[i].first;
        ListView_InsertItem(lv, &it);
        ListView_SetCheckState(lv, static_cast<int>(i), rows[i].second);
    }
    if (select >= 0) {
        ListView_SetItemState(lv, select, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(lv, select, FALSE);
    }
    g.loading = was_loading;
}

void update_move_buttons()
{
    HWND lv = item(IDC_CODECS);
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    int n = ListView_GetItemCount(lv);
    EnableWindow(item(IDC_UP), g.has_sel && sel > 0);
    EnableWindow(item(IDC_DOWN), g.has_sel && sel >= 0 && sel < n - 1);
}

void move_codec(int delta)
{
    HWND lv = item(IDC_CODECS);
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    auto rows = codec_rows();
    int to = sel + delta;
    if (sel < 0 || to < 0 || to >= static_cast<int>(rows.size()))
        return;
    std::swap(rows[sel], rows[to]);
    set_codec_rows(rows, to);
    SetFocus(lv);
}

// --- settings <-> controls ------------------------------------------------

void write_controls(const oa2dp_settings &s)
{
    g.loading = true;
    std::vector<std::pair<oa2dp_codec_id, bool>> rows;
    bool listed[OA2DP_CODEC_COUNT] = {};
    for (uint8_t i = 0; i < s.priority_count; i++) {
        rows.emplace_back(s.priority[i], true);
        listed[s.priority[i]] = true;
    }
    for (int c = OA2DP_CODEC_COUNT - 1; c >= 0; c--)  // disabled ones, best first
        if (!listed[c])
            rows.emplace_back(static_cast<oa2dp_codec_id>(c), false);
    set_codec_rows(rows, 0);

    combo_set(IDC_RATE, s.sample_rate);
    combo_set(IDC_BITS, s.bits);
    combo_set(IDC_CHMODE, s.chmode);
    set_checked(IDC_BITPOOL_AUTO, s.sbc_bitpool == 0);
    SetDlgItemInt(g.wnd, IDC_BITPOOL, s.sbc_bitpool ? s.sbc_bitpool : 53, FALSE);
    combo_set(IDC_BLOCKS, s.sbc_blocks);
    combo_set(IDC_SUBBANDS, s.sbc_subbands);
    combo_set(IDC_ALLOC, s.sbc_alloc);
    combo_set(IDC_AAC_RATE, s.aac_bitrate);
    set_checked(IDC_AAC_VBR, s.aac_vbr);
    combo_set(IDC_LDAC, s.ldac_quality);
    set_checked(IDC_ABR, s.adaptive_bitrate);
    set_checked(IDC_FOLLOW, s.follow_source_rate);
    g.loading = false;
}

unsigned bitpool_field()
{
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(g.wnd, IDC_BITPOOL, &ok, FALSE);
    if (!ok)
        return 53;
    return v < OA2DP_SBC_MIN_BITPOOL ? OA2DP_SBC_MIN_BITPOOL
           : v > OA2DP_SBC_MAX_BITPOOL ? OA2DP_SBC_MAX_BITPOOL
                                       : v;
}

oa2dp_settings read_controls()
{
    oa2dp_settings s;
    oa2dp_settings_default(&s);
    s.priority_count = 0;
    for (auto &r : codec_rows())
        if (r.second)
            s.priority[s.priority_count++] = r.first;
    s.sample_rate = static_cast<uint32_t>(combo_get(IDC_RATE));
    s.bits = static_cast<uint8_t>(combo_get(IDC_BITS));
    s.chmode = static_cast<oa2dp_chmode>(combo_get(IDC_CHMODE));
    s.sbc_bitpool = checked(IDC_BITPOOL_AUTO) ? 0 : static_cast<uint8_t>(bitpool_field());
    s.sbc_blocks = static_cast<uint8_t>(combo_get(IDC_BLOCKS));
    s.sbc_subbands = static_cast<uint8_t>(combo_get(IDC_SUBBANDS));
    s.sbc_alloc = static_cast<oa2dp_sbc_alloc>(combo_get(IDC_ALLOC));
    s.aac_bitrate = static_cast<uint32_t>(combo_get(IDC_AAC_RATE));
    s.aac_vbr = checked(IDC_AAC_VBR);
    s.ldac_quality = static_cast<oa2dp_ldac_quality>(combo_get(IDC_LDAC));
    s.adaptive_bitrate = checked(IDC_ABR);
    s.follow_source_rate = checked(IDC_FOLLOW);
    return s;
}

std::string text_of(const oa2dp_settings &s)
{
    char buf[1024];
    return oa2dp_settings_format(&s, buf, sizeof(buf)) < 0 ? std::string() : std::string(buf);
}

bool dirty()
{
    return g.has_sel && text_of(read_controls()) != text_of(g.saved);
}

// Bitrate and frame size of the SBC choices on screen.
void update_sbc_label()
{
    oa2dp_settings s = read_controls();
    EnableWindow(item(IDC_BITPOOL), g.has_sel && s.sbc_bitpool != 0);
    EnableWindow(item(IDC_BITPOOL_SPIN), g.has_sel && s.sbc_bitpool != 0);
    std::vector<unsigned> rates;
    if (s.sample_rate == 16000 || s.sample_rate == 32000 || s.sample_rate == 44100 ||
        s.sample_rate == 48000)
        rates.push_back(s.sample_rate);
    else
        rates = {44100, 48000};  // SBC tops out at 48 kHz
    std::wstring text;
    SbcFrame f;
    for (unsigned rate : rates) {
        if (!sbc_rate(s.chmode, s.sbc_bitpool, rate, s.sbc_blocks, s.sbc_subbands, &f))
            continue;
        wchar_t b[96];
        swprintf(b, 96, L"\u2248 %u kbit/s at %ls  \u00b7  bitpool %u  \u00b7  %u-byte frames\n",
                 (f.bitrate + 500) / 1000, khz(rate).c_str(), f.bitpool, f.frame_len);
        text += b;
    }
    if (s.chmode == OA2DP_CHMODE_AUTO)
        text += L"Assumes joint stereo. ";
    if (s.sbc_bitpool && s.sbc_bitpool > f.max_bitpool)
        text += L"Capped at " + std::to_wstring(f.max_bitpool) + L", the most this layout allows.";
    else if (s.sbc_bitpool > 53)
        text += L"Many headsets accept at most 53; the headset's own limit wins.";
    else
        text += L"The headset may ask for a lower bitpool.";
    SetDlgItemTextW(g.wnd, IDC_SBC_RATE, text.c_str());
}

void on_edit()
{
    if (g.loading)
        return;
    update_sbc_label();
    bool d = dirty();
    EnableWindow(item(IDC_SAVE), d);
    EnableWindow(item(IDC_UNDO), d);
    update_move_buttons();
}

// --- devices ---------------------------------------------------------------

Headset *selected()
{
    if (!g.has_sel || !g.devs)
        return nullptr;
    for (auto &h : g.devs->items)
        if (h.address == g.sel_addr)
            return &h;
    return nullptr;
}

void update_switch_button()
{
    Headset *h = selected();
    const wchar_t *note =
        L"Switching asks for administrator rights and briefly disconnects the headset. "
        L"The Windows driver can be put back at any time.";
    if (!h) {
        SetDlgItemTextW(g.wnd, IDC_SWITCH, L"Use the OpenA2DP driver");
        EnableWindow(item(IDC_SWITCH), FALSE);
    } else if (h->ours()) {
        SetDlgItemTextW(g.wnd, IDC_SWITCH, L"Go back to the Windows driver");
        EnableWindow(item(IDC_SWITCH), TRUE);
    } else {
        SetDlgItemTextW(g.wnd, IDC_SWITCH, L"Use the OpenA2DP driver");
        bool avail = driver_available(g.devs->set(), h->dev, true);
        EnableWindow(item(IDC_SWITCH), avail);
        if (!avail)
            note = L"The OpenA2DP driver package is not installed on this PC yet. Settings "
                   L"can be prepared now; they are used once the headset runs on OpenA2DP.";
    }
    SetDlgItemTextW(g.wnd, IDC_DRIVER_NOTE, note);
}

void update_status()
{
    std::wstring text;
    Headset *h = selected();
    if (!g.devs || g.devs->items.empty()) {
        text = L"No paired Bluetooth headphones or speakers found.";
    } else if (!h) {
        text = L"Select a headset.";
    } else if (!h->ours()) {
        text = h->name + L": on the Windows driver.";
    } else {
        LiveStatus st;
        if (!read_status(h->address, &st)) {
            text = h->name + L": on the OpenA2DP driver, not connected.";
        } else {
            wchar_t b[256];
            swprintf(b, 256,
                     L"%ls: %ls \u00b7 %ls \u00b7 %ls, %lu-bit, %lu ch \u00b7 %lu kbit/s "
                     L"\u00b7 quality level %lu \u00b7 headset delay %lu ms",
                     h->name.c_str(), st.state.c_str(), st.codec.empty() ? L"-" : st.codec.c_str(),
                     khz(st.sample_rate).c_str(), st.bits, st.channels, st.bitrate / 1000,
                     st.abr_level, st.delay_us / 1000);
            text = b;
        }
    }
    if (text != g.last_status) {
        g.last_status = text;
        SendMessageW(item(IDC_STATUS), SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void enable_panel(bool on)
{
    for (int id : kPanel)
        EnableWindow(item(id), on);
}

bool save();

// Ask before throwing away edits. false = stay where we are.
bool settle_changes(UINT buttons)
{
    if (!dirty())
        return true;
    Headset *h = selected();
    std::wstring q = L"Save the changed settings for " + (h ? h->name : L"this headset") + L"?";
    int r = MessageBoxW(g.wnd, q.c_str(), kTitle, buttons | MB_ICONQUESTION);
    if (r == IDCANCEL)
        return false;
    return r == IDNO || save() || buttons == MB_YESNO;
}

void select_device(int index)
{
    settle_changes(MB_YESNO);
    if (!g.devs || index < 0 || index >= static_cast<int>(g.devs->items.size())) {
        g.has_sel = false;
        oa2dp_settings_default(&g.saved);
        write_controls(g.saved);
    } else {
        g.has_sel = true;
        g.sel_addr = g.devs->items[index].address;
        g.saved = load_settings(g.sel_addr);
        write_controls(g.saved);
    }
    enable_panel(g.has_sel);
    update_switch_button();
    on_edit();
    update_status();
}

void rescan_devices()
{
    g.devs = std::make_unique<DeviceList>();
    HWND lv = item(IDC_DEVICES);
    g.loading = true;
    ListView_DeleteAllItems(lv);
    int sel = -1;
    for (size_t i = 0; i < g.devs->items.size(); i++) {
        const Headset &h = g.devs->items[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t *>(h.name.empty() ? L"(unnamed)" : h.name.c_str());
        ListView_InsertItem(lv, &it);
        std::wstring a = pretty_addr(h.address);
        ListView_SetItemText(lv, static_cast<int>(i), 1, const_cast<wchar_t *>(a.c_str()));
        ListView_SetItemText(lv, static_cast<int>(i), 2,
                             const_cast<wchar_t *>(h.ours() ? L"OpenA2DP" : L"Windows"));
        if (g.has_sel && h.address == g.sel_addr)
            sel = static_cast<int>(i);
    }
    if (sel >= 0)
        ListView_SetItemState(lv, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    g.loading = false;
    if (sel >= 0) {
        update_switch_button();  // the driver may have changed
        update_status();
    } else if (!g.devs->items.empty()) {
        // First start, or the selected headset was unpaired: drop its edits.
        g.has_sel = false;
        ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    } else {
        g.has_sel = false;
        select_device(-1);
    }
}

// --- privileged actions ----------------------------------------------------

bool is_elevated()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

// Run oa2dpctl.exe elevated and wait for it, keeping the window painted.
// Returns false if it could not start (or the user declined the prompt).
bool run_ctl(const std::wstring &args, DWORD *exit_code)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe(path, n);
    exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"oa2dpctl.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g.wnd, L"oa2dpctl.exe was not found next to this program.", kTitle,
                    MB_OK | MB_ICONERROR);
        return false;
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g.wnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        if (GetLastError() != ERROR_CANCELLED)
            MessageBoxW(g.wnd, L"Could not start oa2dpctl.exe as administrator.", kTitle,
                        MB_OK | MB_ICONERROR);
        return false;
    }
    if (!sei.hProcess)
        return false;
    EnableWindow(g.wnd, FALSE);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    while (MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT) ==
           WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    SetCursor(old);
    EnableWindow(g.wnd, TRUE);
    SetForegroundWindow(g.wnd);
    *exit_code = 1;
    GetExitCodeProcess(sei.hProcess, exit_code);
    CloseHandle(sei.hProcess);
    return true;
}

bool save()
{
    oa2dp_settings s = read_controls();
    if (s.priority_count == 0) {
        MessageBoxW(g.wnd, L"Tick at least one codec.", kTitle, MB_OK | MB_ICONWARNING);
        return false;
    }
    std::string text = text_of(s);
    bool ok = false;
    if (g.elevated) {
        LSTATUS st = save_settings(g.sel_addr, s);
        ok = st == ERROR_SUCCESS;
        if (!ok) {
            std::wstring m = L"The settings could not be written (error " + std::to_wstring(st) +
                             L").";
            MessageBoxW(g.wnd, m.c_str(), kTitle, MB_OK | MB_ICONERROR);
        }
    } else {
        // Same key=value lines, one argument each (values never contain spaces).
        std::wstring args = L"set " + addr_str(g.sel_addr);
        for (char c : text)
            args.push_back(c == '\n' ? L' ' : static_cast<wchar_t>(c));
        DWORD code = 1;
        if (!run_ctl(args, &code))
            return false;
        ok = code == 0;
        if (!ok)
            MessageBoxW(g.wnd,
                        L"oa2dpctl could not save the settings. Run \"oa2dpctl set\" in an "
                        L"administrator terminal to see why.",
                        kTitle, MB_OK | MB_ICONERROR);
    }
    if (ok)
        g.saved = load_settings(g.sel_addr);
    on_edit();
    return ok;
}

void switch_driver()
{
    Headset *h = selected();
    if (!h)
        return;
    bool to_ours = !h->ours();
    std::wstring name = h->name;
    if (g.elevated) {
        std::wstring err;
        bool reboot = false;
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        bool ok = install_driver(g.devs->set(), h->dev, to_ours, &err, &reboot);
        SetCursor(old);
        if (!ok)
            MessageBoxW(g.wnd, err.c_str(), kTitle, MB_OK | MB_ICONERROR);
        else if (reboot)
            MessageBoxW(g.wnd, L"Restart Windows to finish switching the driver.", kTitle,
                        MB_OK | MB_ICONINFORMATION);
    } else {
        std::wstring addr = addr_str(h->address);
        DWORD code = 1;
        if (!run_ctl((to_ours ? L"use " : L"restore ") + addr, &code))
            return;
        if (code != 0) {
            std::wstring m = L"Switching " + name + L" failed. Run \"oa2dpctl " +
                             (to_ours ? L"use " : L"restore ") + addr +
                             L"\" in an administrator terminal to see why.";
            MessageBoxW(g.wnd, m.c_str(), kTitle, MB_OK | MB_ICONERROR);
        }
    }
    rescan_devices();
}

// --- window ----------------------------------------------------------------

void fill_choices()
{
    combo_add(IDC_RATE, L"Automatic", 0);
    for (unsigned r : {16000u, 32000u, 44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
        combo_add(IDC_RATE, khz(r).c_str(), r);
    combo_add(IDC_BITS, L"Automatic", 0);
    combo_add(IDC_BITS, L"16-bit", 16);
    combo_add(IDC_BITS, L"24-bit", 24);
    combo_add(IDC_CHMODE, L"Automatic", OA2DP_CHMODE_AUTO);
    combo_add(IDC_CHMODE, L"Stereo", OA2DP_CHMODE_STEREO);
    combo_add(IDC_CHMODE, L"Joint stereo (SBC)", OA2DP_CHMODE_JOINT);
    combo_add(IDC_CHMODE, L"Dual channel (SBC)", OA2DP_CHMODE_DUAL);
    combo_add(IDC_CHMODE, L"Mono", OA2DP_CHMODE_MONO);
    combo_add(IDC_BLOCKS, L"Automatic", 0);
    for (int b : {4, 8, 12, 16})
        combo_add(IDC_BLOCKS, std::to_wstring(b).c_str(), b);
    combo_add(IDC_SUBBANDS, L"Automatic", 0);
    combo_add(IDC_SUBBANDS, L"4", 4);
    combo_add(IDC_SUBBANDS, L"8", 8);
    combo_add(IDC_ALLOC, L"Automatic", OA2DP_SBC_ALLOC_AUTO);
    combo_add(IDC_ALLOC, L"Loudness", OA2DP_SBC_ALLOC_LOUDNESS);
    combo_add(IDC_ALLOC, L"SNR", OA2DP_SBC_ALLOC_SNR);
    combo_add(IDC_AAC_RATE, L"Highest allowed", 0);
    for (unsigned r : {320u, 256u, 192u, 128u})
        combo_add(IDC_AAC_RATE, (std::to_wstring(r) + L" kbit/s").c_str(), r * 1000);
    combo_add(IDC_LDAC, L"Adaptive (follows the link)", OA2DP_LDAC_ABR);
    combo_add(IDC_LDAC, L"High, 990 / 909 kbit/s (HQ)", OA2DP_LDAC_HQ);
    combo_add(IDC_LDAC, L"Medium, 660 / 606 kbit/s (SQ)", OA2DP_LDAC_SQ);
    combo_add(IDC_LDAC, L"Low, 330 / 303 kbit/s (MQ)", OA2DP_LDAC_MQ);
}

void create_controls()
{
    // Left: headsets and driver.
    label(L"Paired headphones and speakers", 12, 12, 360);
    HWND lv = make(WC_LISTVIEWW, L"",
                   LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 12, 32, 360, 438,
                   IDC_DEVICES, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t *title;
        int width;
    } cols[] = {{L"Name", 152}, {L"Address", 122}, {L"Driver", 76}};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t *>(cols[i].title);
        c.cx = S(cols[i].width);
        ListView_InsertColumn(lv, i, &c);
    }
    button(L"Use the OpenA2DP driver", IDC_SWITCH, 12, 480, 230);
    button(L"Refresh", IDC_REFRESH, 252, 480, 120);
    label(L"", 12, 516, 360, 56, IDC_DRIVER_NOTE);

    // Right: settings of the selected headset.
    group(L"Codecs, most preferred first", 390, 8, 528, 172);
    HWND cl = make(WC_LISTVIEWW, L"",
                   LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                       WS_TABSTOP,
                   402, 30, 220, 140, IDC_CODECS, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(cl, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                              LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW c{};
    c.mask = LVCF_WIDTH;
    c.cx = S(196);
    ListView_InsertColumn(cl, 0, &c);
    button(L"Move up", IDC_UP, 634, 30, 100, 26);
    button(L"Move down", IDC_DOWN, 634, 62, 100, 26);
    label(L"Ticked codecs are offered in this order; the first one the headset also "
          L"supports is used. Unticked codecs are never used.",
          634, 96, 272, 52);

    group(L"Audio format", 390, 186, 528, 72);
    label(L"Sample rate", 402, 206, 160);
    combo(IDC_RATE, 402, 224, 160);
    label(L"Bit depth", 574, 206, 160);
    combo(IDC_BITS, 574, 224, 160);
    label(L"Channels", 746, 206, 160);
    combo(IDC_CHMODE, 746, 224, 160);

    group(L"SBC", 390, 264, 528, 118);
    label(L"Bitpool", 402, 284, 120);
    make(WC_EDITW, L"", ES_NUMBER | WS_TABSTOP, 402, 302, 56, 23, IDC_BITPOOL, WS_EX_CLIENTEDGE);
    HWND spin = make(UPDOWN_CLASSW, L"",
                     UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS |
                         UDS_AUTOBUDDY,
                     0, 0, 0, 0, IDC_BITPOOL_SPIN);
    SendMessageW(spin, UDM_SETRANGE32, OA2DP_SBC_MIN_BITPOOL, OA2DP_SBC_MAX_BITPOOL);
    check(L"Auto", IDC_BITPOOL_AUTO, 466, 304, 60);
    label(L"Blocks", 534, 284, 110);
    combo(IDC_BLOCKS, 534, 302, 110);
    label(L"Subbands", 654, 284, 110);
    combo(IDC_SUBBANDS, 654, 302, 110);
    label(L"Allocation", 774, 284, 132);
    combo(IDC_ALLOC, 774, 302, 132);
    label(L"", 402, 330, 504, 48, IDC_SBC_RATE);

    group(L"AAC", 390, 388, 260, 74);
    label(L"Bitrate", 402, 406, 120);
    combo(IDC_AAC_RATE, 402, 424, 120);
    check(L"Variable bitrate", IDC_AAC_VBR, 532, 426, 112);
    group(L"LDAC", 658, 388, 260, 74);
    label(L"Quality", 670, 406, 236);
    combo(IDC_LDAC, 670, 424, 236);

    check(L"Lower the bitrate automatically when the radio link is congested", IDC_ABR, 390,
          470, 528);
    check(L"Follow the Windows output format (reconfigures the stream when it changes)",
          IDC_FOLLOW, 390, 492, 528);

    label(L"Saved settings apply the next time the headset connects.", 390, 524, 200, 40);
    button(L"Defaults", IDC_DEFAULTS, 598, 526, 100);
    button(L"Undo", IDC_UNDO, 706, 526, 100);
    button(L"Save", IDC_SAVE, 814, 526, 104);

    HWND sb = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g.wnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
                              GetModuleHandleW(nullptr), nullptr);
    SendMessageW(sb, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), FALSE);

    fill_choices();
}

// Client area: 930 x 576 at 96 DPI, plus the status bar.
void fit_window()
{
    RECT sb;
    GetWindowRect(item(IDC_STATUS), &sb);
    RECT r{0, 0, S(930), S(576) + (sb.bottom - sb.top)};
    AdjustWindowRect(&r, static_cast<DWORD>(GetWindowLongW(g.wnd, GWL_STYLE)), FALSE);
    SetWindowPos(g.wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(item(IDC_STATUS), WM_SIZE, 0, 0);
}

LRESULT on_notify(NMHDR *nm)
{
    if (nm->idFrom == IDC_DEVICES && nm->code == LVN_ITEMCHANGED) {
        auto *lv = reinterpret_cast<NMLISTVIEW *>(nm);
        if (!g.loading && (lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) &&
            !(lv->uOldState & LVIS_SELECTED))
            select_device(lv->iItem);
    } else if (nm->idFrom == IDC_CODECS && nm->code == LVN_ITEMCHANGED) {
        auto *lv = reinterpret_cast<NMLISTVIEW *>(nm);
        if (lv->uChanged & LVIF_STATE)
            on_edit();
    }
    return 0;
}

void on_command(int id, int code)
{
    switch (id) {
    case IDC_SWITCH: switch_driver(); break;
    case IDC_REFRESH: rescan_devices(); break;
    case IDC_UP: move_codec(-1); break;
    case IDC_DOWN: move_codec(+1); break;
    case IDC_SAVE: save(); break;
    case IDC_UNDO:
        write_controls(g.saved);
        on_edit();
        break;
    case IDC_DEFAULTS: {
        oa2dp_settings d;
        oa2dp_settings_default(&d);
        write_controls(d);
        on_edit();
        break;
    }
    case IDC_BITPOOL:
        if (code == EN_CHANGE)
            on_edit();
        break;
    default:
        if (code == CBN_SELCHANGE || code == BN_CLICKED)
            on_edit();
        break;
    }
}

LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g.wnd = w;
        create_controls();
        fit_window();
        rescan_devices();
        SetTimer(w, kTimerStatus, 1000, nullptr);
        return 0;
    case WM_NOTIFY:
        return on_notify(reinterpret_cast<NMHDR *>(lp));
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_TIMER:
        if (wp == kTimerRescan) {
            KillTimer(w, kTimerRescan);
            rescan_devices();
        } else {
            update_status();
        }
        return 0;
    case WM_DEVICECHANGE:
        // Pairing, unpairing or a driver switch: rescan once things settle.
        if (wp == DBT_DEVNODES_CHANGED)
            SetTimer(w, kTimerRescan, 700, nullptr);
        return TRUE;
    case WM_CLOSE:
        if (settle_changes(MB_YESNOCANCEL))
            DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES |
                                              ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g.dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    g.elevated = is_elevated();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OpenA2DPManager";
    RegisterClassExW(&wc);

    // Sized to its contents in fit_window().
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HWND w = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             S(900), S(600), nullptr, nullptr, inst, nullptr);
    if (!w)
        return 1;
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(g.font);
    return 0;
}
