// SPDX-License-Identifier: MIT
// oa2dpctl — manage OpenA2DP from the command line (SPEC F1, F3, F6.2).
//
//   oa2dpctl list                         paired A2DP headsets and their driver
//   oa2dpctl use <dev>                    switch <dev> to OpenA2DP      (admin)
//   oa2dpctl restore <dev>|all            back to the Windows driver    (admin)
//   oa2dpctl get <dev>                    show codec settings
//   oa2dpctl set <dev> key=value...       change codec settings         (admin)
//   oa2dpctl reset <dev>                  default settings              (admin)
//   oa2dpctl status [<dev>]               live codec / bitrate
//   oa2dpctl sbc-bitrate <mode> <bitpool> [rate] [blocks] [subbands]
//
// <dev> is a list index or a Bluetooth address (001A7DDA7113 or 00:1A:7D:DA:71:13).
// Settings take effect when the headset reconnects. The device and registry
// logic lives in tools/common/manage.cpp, shared with oa2dpgui.
#include "../common/manage.h"

#include <cstdio>
#include <string>

using namespace oa2dp::manage;

namespace {

bool switch_driver(HDEVINFO set, SP_DEVINFO_DATA &dev, bool ours)
{
    std::wstring err;
    bool reboot = false;
    if (!install_driver(set, dev, ours, &err, &reboot)) {
        fwprintf(stderr, L"%ls\n", err.c_str());
        return false;
    }
    if (reboot)
        wprintf(L"A restart is required to finish.\n");
    return true;
}

bool report(LSTATUS st)
{
    if (st != ERROR_SUCCESS)
        fwprintf(stderr, L"cannot write settings (%ld)%ls\n", st,
                 st == ERROR_ACCESS_DENIED ? L": run as administrator" : L"");
    return st == ERROR_SUCCESS;
}

void print_status(unsigned long long addr)
{
    LiveStatus st;
    if (!read_status(addr, &st)) {
        wprintf(L"  %ls: not connected through OpenA2DP\n", addr_str(addr).c_str());
        return;
    }
    wprintf(L"  %ls: %ls, %ls %lu Hz %lu ch %lu bit, %lu kbit/s, quality level %lu, headset delay "
            L"%lu ms\n",
            addr_str(addr).c_str(), st.state.c_str(), st.codec.empty() ? L"-" : st.codec.c_str(),
            st.sample_rate, st.channels, st.bits, st.bitrate / 1000, st.abr_level,
            st.delay_us / 1000);
}

int cmd_sbc_bitrate(int argc, wchar_t **argv)
{
    if (argc < 4)
        return 2;
    std::wstring mode = argv[2];
    oa2dp_chmode m = mode == L"mono"     ? OA2DP_CHMODE_MONO
                     : mode == L"dual"   ? OA2DP_CHMODE_DUAL
                     : mode == L"stereo" ? OA2DP_CHMODE_STEREO
                                         : OA2DP_CHMODE_JOINT;
    unsigned bitpool = wcstoul(argv[3], nullptr, 10);
    unsigned rate = argc > 4 ? wcstoul(argv[4], nullptr, 10) : 44100;
    unsigned blocks = argc > 5 ? wcstoul(argv[5], nullptr, 10) : 16;
    unsigned subbands = argc > 6 ? wcstoul(argv[6], nullptr, 10) : 8;
    SbcFrame f;
    sbc_rate(m, bitpool, rate, blocks, subbands, &f);
    wprintf(L"SBC %ls, bitpool %u, %u Hz, %u blocks, %u subbands: frame %u bytes, %.1f kbit/s\n",
            mode.c_str(), bitpool, rate, blocks == 4 || blocks == 8 || blocks == 12 ? blocks : 16,
            subbands == 4 ? 4u : 8u, f.frame_len, f.bitrate / 1000.0);
    return 0;
}

void usage()
{
    fwprintf(stderr,
             L"usage: oa2dpctl list | use <dev> | restore <dev>|all | get <dev> |\n"
             L"                set <dev> key=value... | reset <dev> | status [<dev>] |\n"
             L"                sbc-bitrate <mono|dual|stereo|joint> <bitpool> [rate] [blocks] "
             L"[subbands]\n\n"
             L"settings keys: codecs (e.g. ldac,aptx_hd,aptx,aac,sbc), sample_rate, bits,\n"
             L"  channel_mode (auto|mono|dual|stereo|joint), sbc_bitpool, sbc_blocks,\n"
             L"  sbc_subbands, sbc_allocation (auto|loudness|snr), aac_bitrate, aac_vbr,\n"
             L"  ldac_quality (abr|hq|sq|mq), adaptive_bitrate, follow_source_rate\n");
}

}  // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    std::wstring cmd = argv[1];
    if (cmd == L"sbc-bitrate") {
        int r = cmd_sbc_bitrate(argc, argv);
        if (r)
            usage();
        return r;
    }

    DeviceList devs;
    if (cmd == L"list") {
        if (devs.items.empty())
            wprintf(L"No paired Bluetooth headphones/speakers found.\n");
        for (size_t i = 0; i < devs.items.size(); i++) {
            const Headset &h = devs.items[i];
            wprintf(L"[%zu] %ls  %ls  driver: %ls\n", i, addr_str(h.address).c_str(),
                    h.name.c_str(), h.ours() ? L"OpenA2DP" : L"Windows");
        }
        return 0;
    }
    if (cmd == L"status") {
        for (auto &h : devs.items)
            if (argc < 3 || &h == devs.find(argv[2]))
                print_status(h.address);
        return 0;
    }
    if (cmd == L"restore" && argc >= 3 && std::wstring(argv[2]) == L"all") {
        int rc = 0;
        for (auto &h : devs.items)
            if (h.ours() && !switch_driver(devs.set(), h.dev, false))
                rc = 1;
        return rc;
    }
    if (argc < 3) {
        usage();
        return 2;
    }
    Headset *h = devs.find(argv[2]);
    if (!h) {
        fwprintf(stderr, L"no such device: %ls (see 'oa2dpctl list')\n", argv[2]);
        return 1;
    }

    if (cmd == L"use" || cmd == L"restore") {
        bool ours = cmd == L"use";
        if (!switch_driver(devs.set(), h->dev, ours))
            return 1;
        wprintf(L"%ls now uses the %ls driver.\n", h->name.c_str(), ours ? L"OpenA2DP" : L"Windows");
        return 0;
    }
    if (cmd == L"get") {
        oa2dp_settings s = load_settings(h->address);
        char text[1024];
        oa2dp_settings_format(&s, text, sizeof(text));
        printf("%s", text);
        return 0;
    }
    if (cmd == L"reset")
        return report(reset_settings(h->address)) ? 0 : 1;
    if (cmd == L"set") {
        oa2dp_settings s = load_settings(h->address);
        for (int i = 3; i < argc; i++) {
            int bad = 0;
            if (oa2dp_settings_parse(&s, ascii(argv[i]).c_str(), &bad) != OA2DP_OK) {
                fwprintf(stderr, L"invalid setting: %ls\n", argv[i]);
                return 1;
            }
        }
        if (!report(save_settings(h->address, s)))
            return 1;
        wprintf(L"Saved. Reconnect the headset to apply.\n");
        return 0;
    }
    usage();
    return 2;
}
