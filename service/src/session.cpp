// SPDX-License-Identifier: MIT
#include "session.h"

#include <bluetoothapis.h>

#include <algorithm>
#include <cstring>

#include "log.h"
#include "oa2dp/encoder.h"
#include "oa2dp/media.h"
#include "oa2dp/pcm_ring.h"

namespace oa2dp {
namespace {

constexpr DWORD kPeerConfigGraceMs = 1500;  // let the headset configure us first
constexpr DWORD kIdleSuspendMs = 3000;      // SUSPEND after this much silence
constexpr DWORD kReconnectPollMs = 5000;
constexpr uint32_t kAbrHighWater = 6;       // queued media packets
constexpr uint32_t kAbrLowWater = 2;
constexpr uint32_t kAbrDownHoldMs = 300;
constexpr uint32_t kAbrUpHoldMs = 5000;

bool is_after(DWORD now, DWORD deadline) { return static_cast<LONG>(now - deadline) >= 0; }

oa2dp_avdtp_caps make_config_caps(const oa2dp_codec_caps &cfg, bool delay_reporting)
{
    oa2dp_avdtp_caps c{};
    uint8_t ie[OA2DP_CODEC_IE_MAX];
    int n = oa2dp_codec_build(&cfg, ie, sizeof(ie));
    c.media_transport = true;
    c.delay_reporting = delay_reporting;
    if (n > 0) {
        c.media_codec = true;
        c.codec_len = static_cast<uint8_t>(n);
        std::memcpy(c.codec, ie, static_cast<size_t>(n));
    }
    return c;
}

std::string codec_label(const oa2dp_codec_caps &c) { return oa2dp_codec_name(c.codec); }

}  // namespace

Session::Session(std::wstring transport_path) : path_(std::move(transport_path))
{
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

Session::~Session()
{
    stop();
    if (stop_event_)
        CloseHandle(stop_event_);
}

void Session::start()
{
    main_ = std::thread([this] { main_loop(); });
}

void Session::stop()
{
    if (stopping_.exchange(true))
        return;
    SetEvent(stop_event_);
    cv_.notify_all();
    tr_.cancel_all();
    audio_.cancel_all();
    if (main_.joinable())
        main_.join();
}

void Session::post(const Command &c)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        cmd_q_.push_back(c);
    }
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Helper threads
// ---------------------------------------------------------------------------

void Session::rx_loop()
{
    std::vector<uint8_t> buf(OA2DP_AVDTP_MSG_MAX + 3);
    while (!stopping_) {
        if (!rx_active_) {
            WaitForSingleObject(stop_event_, 50);
            continue;
        }
        int n = tr_.recv(Oa2dpChannelSignaling, buf.data(), buf.size(), 500, stop_event_);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(mu_);
            rx_q_.emplace_back(buf.begin(), buf.begin() + n);
            cv_.notify_all();
        } else if (n < 0 && rx_active_.exchange(false) && !stopping_) {
            post({Cmd::SignalingClosed});
        }
    }
}

void Session::event_loop()
{
    while (!stopping_) {
        OA2DP_EVENT ev{};
        if (!tr_.wait_event(ev, stop_event_)) {
            WaitForSingleObject(stop_event_, 100);
            continue;
        }
        Command c{};
        c.channel = ev.Channel;
        switch (ev.Type) {
        case Oa2dpEventChannelOpened:
            c.cmd = ev.Kind == Oa2dpChannelMedia ? Cmd::MediaOpenedByPeer
                                                 : Cmd::SignalingOpenedByPeer;
            break;
        case Oa2dpEventChannelClosed:
            c.cmd = ev.Kind == Oa2dpChannelMedia ? Cmd::MediaClosed : Cmd::SignalingClosed;
            break;
        case Oa2dpEventDeviceGone:
            c.cmd = Cmd::DeviceGone;
            break;
        default:
            continue;
        }
        post(c);
    }
}

void Session::audio_loop()
{
    while (!stopping_) {
        if (!audio_ready_) {
            WaitForSingleObject(stop_event_, 100);
            continue;
        }
        Command c{};
        c.cmd = Cmd::AudioFormat;
        if (audio_.wait_format(c.format, stop_event_))
            post(c);
        else
            WaitForSingleObject(stop_event_, 100);
    }
}

// ---------------------------------------------------------------------------
// AVDTP plumbing (main thread)
// ---------------------------------------------------------------------------

int Session::cb_send(void *user, const uint8_t *pdu, size_t len)
{
    auto *s = static_cast<Session *>(user);
    return s->tr_.send(Oa2dpChannelSignaling, pdu, len) ? 0 : -1;
}

uint32_t Session::cb_now(void *) { return GetTickCount(); }

void Session::cb_event(void *user, const oa2dp_avdtp_event *ev)
{
    static_cast<Session *>(user)->on_avdtp_event(*ev);
}

uint8_t Session::cb_check(void *user, uint8_t seid, const oa2dp_avdtp_caps *cfg, uint8_t *cat)
{
    auto *s = static_cast<Session *>(user);
    *cat = OA2DP_AVDTP_CAT_MEDIA_CODEC;
    auto it = s->sep_codec_.find(seid);
    oa2dp_codec_caps want{}, local{};
    if (it == s->sep_codec_.end() || oa2dp_codec_parse(cfg->codec, cfg->codec_len, &want) != OA2DP_OK)
        return OA2DP_A2DP_NOT_SUPPORTED_CODEC_TYPE;
    oa2dp_codec_local_caps(it->second, &local);
    return oa2dp_codec_check_config(&local, &want);
}

bool Session::pump(DWORD timeout_ms)
{
    std::vector<uint8_t> pdu;
    {
        std::unique_lock<std::mutex> lock(mu_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return !rx_q_.empty() || stopping_; }))
            return false;
        if (stopping_ || rx_q_.empty())
            return false;
        pdu = std::move(rx_q_.front());
        rx_q_.pop_front();
    }
    oa2dp_avdtp_input(&av_, pdu.data(), pdu.size());
    return true;
}

bool Session::request(int label, oa2dp_avdtp_event &out, DWORD timeout_ms)
{
    if (label < 0)
        return false;
    DWORD deadline = GetTickCount() + timeout_ms;
    while (!stopping_ && sig_open_) {
        auto it = responses_.find(label);
        if (it != responses_.end()) {
            out = it->second;
            responses_.erase(it);
            return out.msg_type == OA2DP_AVDTP_MSG_ACCEPT;
        }
        if (is_after(GetTickCount(), deadline)) {
            oa2dp_avdtp_check_timeouts(&av_);
            out = {};
            out.timed_out = true;
            return false;
        }
        pump(50);
    }
    return false;
}

void Session::on_avdtp_event(const oa2dp_avdtp_event &ev)
{
    if (ev.msg_type != OA2DP_AVDTP_MSG_COMMAND) {
        responses_[ev.label] = ev;
        return;
    }
    // Commands the headset sent and the core already accepted.
    switch (ev.signal_id) {
    case OA2DP_AVDTP_SET_CONFIGURATION:
        if (oa2dp_codec_parse(ev.caps.codec, ev.caps.codec_len, &config_) == OA2DP_OK) {
            local_seid_ = ev.local_seid;
            have_remote_caps_ = false;
            need_remote_caps_ = true;
            init_deadline_ = 0;
            LOGI("headset configured %s at %u Hz", oa2dp_codec_name(config_.codec),
                 oa2dp_codec_sample_rate(&config_));
            publish("configured");
        }
        break;
    case OA2DP_AVDTP_RECONFIGURE:
        if (ev.caps.media_codec)
            oa2dp_codec_parse(ev.caps.codec, ev.caps.codec_len, &config_);
        break;
    case OA2DP_AVDTP_START:
        begin_streaming();
        break;
    case OA2DP_AVDTP_SUSPEND:
    case OA2DP_AVDTP_CLOSE:
    case OA2DP_AVDTP_ABORT:
        end_streaming();
        break;
    case OA2DP_AVDTP_DELAYREPORT:
        delay_report_us_ = static_cast<uint32_t>(ev.delay) * 100;
        if (audio_ready_)
            audio_.set_latency(0, delay_report_us_);
        LOGI("headset delay report: %u us", delay_report_us_.load());
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Procedures (main thread)
// ---------------------------------------------------------------------------

bool Session::connect_signaling()
{
    OA2DP_CHANNEL_INFO ci{};
    if (!tr_.open_channel(Oa2dpChannelSignaling, ci))
        return false;
    sig_mtu_ = ci.OutMtu;
    av_.out_mtu = sig_mtu_ < 48 ? 48 : sig_mtu_;
    sig_open_ = true;
    rx_active_ = true;
    LOGI("signalling channel open (MTU %u)", sig_mtu_);
    return true;
}

bool Session::open_media_channel()
{
    if (media_open_)
        return true;
    OA2DP_CHANNEL_INFO ci{};
    if (!tr_.open_channel(Oa2dpChannelMedia, ci))
        return false;
    media_mtu_ = ci.OutMtu;
    media_open_ = true;
    LOGI("media channel open (MTU %u)", media_mtu_);
    return true;
}

bool Session::configure_as_initiator()
{
    oa2dp_avdtp_event ev{};
    if (!request(oa2dp_avdtp_discover(&av_), ev)) {
        LOGW("DISCOVER failed");
        return false;
    }

    struct Candidate {
        uint8_t seid;
        oa2dp_codec_caps caps;
        bool delay_reporting;
    };
    std::vector<Candidate> found;
    for (uint8_t i = 0; i < ev.sep_count; i++) {
        const auto &sep = ev.seps[i];
        if (sep.in_use || sep.tsep != OA2DP_AVDTP_TSEP_SNK || sep.media_type != OA2DP_MEDIA_TYPE_AUDIO)
            continue;
        oa2dp_avdtp_event caps{};
        bool ok = request(oa2dp_avdtp_get_capabilities(&av_, sep.seid, true), caps);
        if (!ok && caps.msg_type == OA2DP_AVDTP_MSG_GENERAL_REJECT)  // AVDTP 1.0 peer
            ok = request(oa2dp_avdtp_get_capabilities(&av_, sep.seid, false), caps);
        Candidate c{};
        c.seid = sep.seid;
        c.delay_reporting = caps.caps.delay_reporting;
        if (ok && caps.caps.media_codec &&
            oa2dp_codec_parse(caps.caps.codec, caps.caps.codec_len, &c.caps) == OA2DP_OK) {
            LOGI("headset SEP %u: %s", sep.seid, oa2dp_codec_name(c.caps.codec));
            found.push_back(c);
        }
    }

    // Try codecs in the user's priority order; fall back on rejection.
    for (uint8_t p = 0; p < settings_.priority_count; p++) {
        oa2dp_codec_id want = settings_.priority[p];
        if (!oa2dp_encoder_available(want))
            continue;
        uint8_t local = 0;
        for (const auto &kv : sep_codec_)
            if (kv.second == want)
                local = kv.first;
        for (const auto &c : found) {
            if (c.caps.codec != want || !local)
                continue;
            oa2dp_codec_caps cfg{};
            if (oa2dp_codec_select(nullptr, &c.caps, &settings_, &cfg) != OA2DP_OK)
                continue;
            oa2dp_avdtp_caps avc = make_config_caps(cfg, c.delay_reporting);
            oa2dp_avdtp_event r{};
            if (!request(oa2dp_avdtp_set_configuration(&av_, local, c.seid, &avc), r)) {
                LOGW("%s rejected by headset (error 0x%02X)", oa2dp_codec_name(want), r.error_code);
                continue;
            }
            local_seid_ = local;
            config_ = cfg;
            remote_caps_ = c.caps;
            have_remote_caps_ = true;
            LOGI("configured %s, %u Hz, %u ch", oa2dp_codec_name(want),
                 oa2dp_codec_sample_rate(&cfg), oa2dp_codec_channels(&cfg));
            if (!request(oa2dp_avdtp_open(&av_, local), r)) {
                LOGW("OPEN failed");
                request(oa2dp_avdtp_abort(&av_, local), r);
                return false;
            }
            publish("configured");
            return open_media_channel() && setup_audio_endpoint();
        }
    }
    LOGE("no common codec with the headset");
    return false;
}

bool Session::setup_audio_endpoint()
{
    if (!audio_ready_) {
        bool opened = false;
        for (int i = 0; i < 25 && !opened && !stopping_; i++) {  // child may still be starting
            opened = audio_.open(info_.RemoteAddress);
            if (!opened)
                WaitForSingleObject(stop_event_, 200);
        }
        if (!opened) {
            LOGE("audio endpoint for this headset not found (is oa2dpaudio.sys installed?)");
            return false;
        }
        if (!name_.empty())
            audio_.set_name(name_);
        if (!audio_.map_ring())
            return false;
        audio_ready_ = true;
    }

    // Offer every rate the headset accepts for this codec, so Windows can
    // switch rate and we follow with RECONFIGURE instead of resampling.
    OA2DP_FORMAT_LIST list{};
    const uint32_t current = oa2dp_codec_sample_rate(&config_);
    static const uint32_t rates[] = {44100, 48000, 88200, 96000, 32000, 16000};
    for (uint32_t rate : rates) {
        if (list.Count >= OA2DP_MAX_FORMATS)
            break;
        oa2dp_codec_caps probe = config_;
        bool ok = rate == current ||
                  (have_remote_caps_ && settings_.follow_source_rate &&
                   oa2dp_codec_set_rate(&probe, &remote_caps_, rate) == OA2DP_OK);
        if (!ok)
            continue;
        if (rate == current)
            list.Preferred = list.Count;
        OA2DP_FORMAT &f = list.Formats[list.Count++];
        f.SampleRate = rate;
        f.Channels = oa2dp_codec_channels(&config_);
        f.BitsPerSample = 32;  // float
    }
    audio_.set_formats(list);
    audio_.set_latency(0, delay_report_us_);
    return true;
}

bool Session::start_stream()
{
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (!sep || sep->state != OA2DP_SEP_OPEN || !media_open_)
        return false;
    oa2dp_avdtp_event r{};
    if (!request(oa2dp_avdtp_start(&av_, local_seid_), r)) {
        LOGW("START rejected (0x%02X)", r.error_code);
        return false;
    }
    begin_streaming();
    return true;
}

void Session::suspend_stream()
{
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (!sep || sep->state != OA2DP_SEP_STREAMING)
        return;
    end_streaming();
    oa2dp_avdtp_event r{};
    if (!request(oa2dp_avdtp_suspend(&av_, local_seid_), r))
        LOGW("SUSPEND failed");
    else
        publish("suspended");
}

void Session::reconfigure_rate(uint32_t rate)
{
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (!sep || rate == oa2dp_codec_sample_rate(&config_))
        return;
    if (!have_remote_caps_) {
        oa2dp_avdtp_event caps{};
        if (request(oa2dp_avdtp_get_capabilities(&av_, sep->remote_seid, true), caps) &&
            oa2dp_codec_parse(caps.caps.codec, caps.caps.codec_len, &remote_caps_) == OA2DP_OK)
            have_remote_caps_ = true;
        else
            return;
    }
    oa2dp_codec_caps next = config_;
    if (oa2dp_codec_set_rate(&next, &remote_caps_, rate) != OA2DP_OK)
        return;  // Windows resamples; nothing to do

    bool was_streaming = sep->state == OA2DP_SEP_STREAMING;
    if (was_streaming)
        suspend_stream();
    oa2dp_avdtp_caps avc = make_config_caps(next, false);
    oa2dp_avdtp_event r{};
    if (request(oa2dp_avdtp_reconfigure(&av_, local_seid_, &avc), r)) {
        config_ = next;
        LOGI("reconfigured to %u Hz", rate);
        setup_audio_endpoint();
    } else {
        LOGW("RECONFIGURE to %u Hz rejected (0x%02X)", rate, r.error_code);
    }
    if (was_streaming)
        start_stream();
}

void Session::teardown(const char *why)
{
    LOGI("teardown: %s", why);
    end_streaming();
    if (media_open_)
        tr_.close_channel(Oa2dpChannelMedia);
    media_open_ = false;
    rx_active_ = false;
    if (sig_open_)
        tr_.close_channel(Oa2dpChannelSignaling);
    sig_open_ = false;
    oa2dp_avdtp_reset(&av_);
    responses_.clear();
    local_seid_ = 0;
    init_deadline_ = 0;
    publish("disconnected");
}

void Session::handle(const Command &c)
{
    switch (c.cmd) {
    case Cmd::SignalingOpenedByPeer:
        sig_mtu_ = c.channel.OutMtu;
        av_.out_mtu = sig_mtu_ < 48 ? 48 : sig_mtu_;
        sig_open_ = true;
        rx_active_ = true;
        cv_.notify_all();
        init_deadline_ = GetTickCount() + kPeerConfigGraceMs;
        LOGI("headset opened signalling");
        break;
    case Cmd::MediaOpenedByPeer:
        media_mtu_ = c.channel.OutMtu;
        media_open_ = true;
        setup_audio_endpoint();
        break;
    case Cmd::SignalingClosed:
        teardown("signalling closed");
        break;
    case Cmd::MediaClosed:
        end_streaming();
        media_open_ = false;
        oa2dp_avdtp_media_closed(&av_, local_seid_);
        break;
    case Cmd::DeviceGone:
        stopping_ = true;
        SetEvent(stop_event_);
        break;
    case Cmd::AudioFormat:
        audio_running_ = c.format.Running != 0;
        if (audio_running_) {
            idle_since_ = 0;
            if (settings_.follow_source_rate)
                reconfigure_rate(c.format.Format.SampleRate);
            if (!sig_open_)
                next_reconnect_ = GetTickCount();  // user wants sound: connect now
            else if (!streaming_)
                start_stream();
        } else {
            idle_since_ = GetTickCount();
        }
        break;
    case Cmd::IdleCheck:
        break;
    }
}

void Session::publish(const char *state)
{
    std::lock_guard<std::mutex> lock(status_mu_);
    status_.state = state;
    if (local_seid_) {
        status_.codec = codec_label(config_);
        status_.sample_rate = oa2dp_codec_sample_rate(&config_);
        status_.channels = oa2dp_codec_channels(&config_);
        status_.bits = oa2dp_codec_pcm_bits(&config_, &settings_);
    }
    status_.delay_report_us = delay_report_us_;
    publish_status(info_.RemoteAddress, status_);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void Session::main_loop()
{
    if (!tr_.open(path_)) {
        LOGE("cannot open %ls (%lu)", path_.c_str(), GetLastError());
        return;
    }
    if (!tr_.get_info(info_) || info_.Version != OA2DP_IOCTL_VERSION) {
        LOGE("driver version mismatch or GET_INFO failed");
        tr_.close();
        return;
    }
    settings_ = load_settings(info_.RemoteAddress);

    BLUETOOTH_DEVICE_INFO bdi{};
    bdi.dwSize = sizeof(bdi);
    bdi.Address.ullLong = info_.RemoteAddress;
    if (BluetoothGetDeviceInfo(nullptr, &bdi) == ERROR_SUCCESS)
        name_ = bdi.szName;
    LOGI("session for %ls (%ls)", address_key(info_.RemoteAddress).c_str(), name_.c_str());

    oa2dp_avdtp_callbacks cb{cb_send, cb_now, cb_event, cb_check, nullptr};
    oa2dp_avdtp_init(&av_, &cb, this, 672);
    for (uint8_t i = 0; i < settings_.priority_count; i++) {
        oa2dp_codec_id id = settings_.priority[i];
        if (!oa2dp_encoder_available(id))
            continue;
        oa2dp_codec_caps local{};
        oa2dp_codec_local_caps(id, &local);
        oa2dp_avdtp_caps caps = make_config_caps(local, true);
        int seid = oa2dp_avdtp_add_sep(&av_, OA2DP_AVDTP_TSEP_SRC, OA2DP_MEDIA_TYPE_AUDIO, &caps);
        if (seid > 0)
            sep_codec_[static_cast<uint8_t>(seid)] = id;
    }
    if (sep_codec_.empty()) {
        LOGE("no encoder available");
        tr_.close();
        return;
    }

    rx_ = std::thread([this] { rx_loop(); });
    ev_ = std::thread([this] { event_loop(); });
    audio_thr_ = std::thread([this] { audio_loop(); });
    publish("connecting");
    next_reconnect_ = GetTickCount();

    while (!stopping_) {
        // Connect when the headset's ACL link is up (it was switched on) or
        // when Windows starts playing to our endpoint.
        if (!sig_open_ && is_after(GetTickCount(), next_reconnect_)) {
            next_reconnect_ = GetTickCount() + kReconnectPollMs;
            BLUETOOTH_DEVICE_INFO d{};
            d.dwSize = sizeof(d);
            d.Address.ullLong = info_.RemoteAddress;
            bool connected = BluetoothGetDeviceInfo(nullptr, &d) == ERROR_SUCCESS && d.fConnected;
            if (connected && connect_signaling())
                init_deadline_ = GetTickCount() + kPeerConfigGraceMs;
        }
        if (sig_open_ && init_deadline_ && !local_seid_ && is_after(GetTickCount(), init_deadline_)) {
            init_deadline_ = 0;
            if (!configure_as_initiator())
                teardown("configuration failed");
        }
        if (need_remote_caps_ && local_seid_) {
            need_remote_caps_ = false;
            oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
            oa2dp_avdtp_event caps{};
            if (sep && request(oa2dp_avdtp_get_capabilities(&av_, sep->remote_seid, true), caps) &&
                oa2dp_codec_parse(caps.caps.codec, caps.caps.codec_len, &remote_caps_) == OA2DP_OK)
                have_remote_caps_ = true;
            if (media_open_)
                setup_audio_endpoint();
        }
        if (streaming_ && !audio_running_ && idle_since_ &&
            is_after(GetTickCount(), idle_since_ + kIdleSuspendMs))
            suspend_stream();

        // Process received signalling and queued commands.
        while (pump(0)) {
        }
        std::deque<Command> cmds;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !rx_q_.empty() || !cmd_q_.empty() || stopping_;
            });
            cmds.swap(cmd_q_);
        }
        for (const auto &c : cmds)
            handle(c);
        oa2dp_avdtp_check_timeouts(&av_);
    }

    if (sig_open_)
        teardown("session stopping");
    end_streaming();
    SetEvent(stop_event_);
    tr_.cancel_all();
    audio_.cancel_all();
    for (std::thread *t : {&rx_, &ev_, &audio_thr_})
        if (t->joinable())
            t->join();
    audio_.close();
    tr_.close();
    clear_status(info_.RemoteAddress);
    LOGI("session for %ls ended", address_key(info_.RemoteAddress).c_str());
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void Session::begin_streaming()
{
    if (streaming_ || !media_open_ || !audio_ready_)
        return;
    if (stream_.joinable())  // previous loop exited on its own (encoder error)
        stream_.join();
    streaming_ = true;
    stream_ = std::thread([this] { stream_loop(); });
    publish("streaming");
}

void Session::end_streaming()
{
    if (!streaming_.exchange(false))
        return;
    if (stream_.joinable())
        stream_.join();
    publish("open");
}

void Session::stream_loop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const oa2dp_codec_caps cfg = config_;
    const oa2dp_settings set = settings_;
    oa2dp_pcm_ring *ring = audio_.ring();

    oa2dp_encoder *enc = nullptr;
    int err = oa2dp_encoder_create(&cfg, &set, media_mtu_, &enc);
    if (err != OA2DP_OK) {
        LOGE("encoder for %s failed: %s", oa2dp_codec_name(cfg.codec), oa2dp_strerror(err));
        streaming_ = false;
        return;
    }
    const oa2dp_encoder_info info = enc->info;
    const uint32_t spc = info.samples_per_call;
    const uint16_t ring_ch = ring->channels;
    const uint8_t ch = info.channels;

    std::vector<uint8_t> pkt(media_mtu_), out(info.max_output);
    std::vector<float> fbuf(static_cast<size_t>(spc) * ring_ch);
    std::vector<int32_t> pcm(static_cast<size_t>(spc) * std::max<uint16_t>(ring_ch, 2));
    oa2dp_rtp rtp{};
    oa2dp_rtp_init(&rtp, cfg.codec, pkt.data(), pkt.size(), GetTickCount() ^ 0xA2D9u);

    // Codecs whose encoder already sizes frames for the MTU, or that target
    // low latency, send one encoder output per packet.
    const bool flush_each = cfg.codec == OA2DP_CODEC_AAC || cfg.codec == OA2DP_CODEC_LDAC ||
                            cfg.codec == OA2DP_CODEC_APTX_LL;

    oa2dp_abr abr{};
    const bool use_abr = set.adaptive_bitrate && (info.levels > 1 || info.native_abr);
    oa2dp_abr_init(&abr, info.levels, kAbrHighWater, kAbrLowWater, kAbrDownHoldMs, kAbrUpHoldMs);

    uint64_t bytes_sent = 0;
    DWORD window_start = GetTickCount(), last_abr = window_start;
    LOGI("streaming %s: %u Hz, %u ch, %u samples/call", oa2dp_codec_name(cfg.codec),
         info.sample_rate, ch, spc);

    auto send_packet = [&](size_t len) {
        if (len && tr_.send_media(pkt.data(), len))
            bytes_sent += len;
    };

    while (streaming_) {
        if (oa2dp_ring_readable(ring) < spc) {
            Sleep(2);
            continue;
        }
        // Ring -> int32 24-bit -> codec channel layout.
        if (ring->format == OA2DP_RING_FMT_FLOAT32) {
            oa2dp_ring_read(ring, fbuf.data(), spc);
            oa2dp_pcm_float_to_s24(fbuf.data(), pcm.data(), fbuf.size());
        } else {
            oa2dp_ring_read(ring, pcm.data(), spc);
        }
        if (ch == 1 && ring_ch == 2) {
            oa2dp_pcm_downmix_to_mono(pcm.data(), pcm.data(), spc);
        } else if (ch == 2 && ring_ch == 1) {
            for (size_t i = spc; i-- > 0;)
                pcm[2 * i] = pcm[2 * i + 1] = pcm[i];
        }

        size_t len = 0;
        uint8_t frames = 0;
        if (oa2dp_encoder_encode(enc, pcm.data(), out.data(), out.size(), &len, &frames) != OA2DP_OK) {
            LOGW("encode failed");
            continue;
        }
        if (len == 0 || frames == 0)
            continue;

        int r = oa2dp_rtp_add_frames(&rtp, out.data(), len, frames, spc);
        if (r == 0) {
            send_packet(oa2dp_rtp_flush(&rtp));
            r = oa2dp_rtp_add_frames(&rtp, out.data(), len, frames, spc);
        }
        if (r == OA2DP_ERR_NOSPC) {  // larger than one packet: fragment
            send_packet(oa2dp_rtp_flush(&rtp));
            size_t off = 0;
            while (off < len) {
                int n = oa2dp_rtp_fragment(&rtp, out.data(), len, spc, &off);
                if (n <= 0)
                    break;
                send_packet(static_cast<size_t>(n));
            }
        } else if (flush_each) {
            send_packet(oa2dp_rtp_flush(&rtp));
        }

        DWORD now = GetTickCount();
        if (use_abr && now - last_abr >= 20) {
            last_abr = now;
            OA2DP_TX_STATE tx{};
            if (tr_.tx_state(tx)) {
                if (info.native_abr && enc->ops->queue_feedback) {
                    enc->ops->queue_feedback(enc, tx.QueuedPackets);
                } else {
                    int level = oa2dp_abr_update(&abr, tx.QueuedPackets, now);
                    if (level >= 0) {
                        oa2dp_encoder_set_level(enc, static_cast<uint8_t>(level));
                        LOGI("link quality: level %d of %u", level, info.levels);
                    }
                }
            }
        }
        if (now - window_start >= 1000) {
            std::lock_guard<std::mutex> lock(status_mu_);
            status_.bitrate = static_cast<uint32_t>(bytes_sent * 8000 / (now - window_start));
            status_.abr_level = abr.level;
            publish_status(info_.RemoteAddress, status_);
            bytes_sent = 0;
            window_start = now;
        }
    }
    send_packet(oa2dp_rtp_flush(&rtp));
    tr_.drain_media();
    oa2dp_encoder_destroy(enc);
}

}  // namespace oa2dp
