/* SPDX-License-Identifier: MIT */
/* AVDTP 1.3 signalling. See include/oa2dp/avdtp.h. */
#include "oa2dp/avdtp.h"

#include <string.h>

#define PKT_SINGLE 0
#define PKT_START 1
#define PKT_CONTINUE 2
#define PKT_END 3

/* ---------------------------------------------------------------------- */
/* Names                                                                  */
/* ---------------------------------------------------------------------- */

const char *oa2dp_avdtp_signal_name(uint8_t id)
{
    static const char *const names[] = {
        "?",           "DISCOVER", "GET_CAPABILITIES", "SET_CONFIGURATION",
        "GET_CONFIGURATION", "RECONFIGURE", "OPEN", "START", "CLOSE", "SUSPEND",
        "ABORT", "SECURITY_CONTROL", "GET_ALL_CAPABILITIES", "DELAYREPORT"};
    return id < sizeof(names) / sizeof(names[0]) ? names[id] : "?";
}

const char *oa2dp_sep_state_name(oa2dp_sep_state st)
{
    switch (st) {
    case OA2DP_SEP_IDLE: return "IDLE";
    case OA2DP_SEP_CONFIGURED: return "CONFIGURED";
    case OA2DP_SEP_OPEN: return "OPEN";
    case OA2DP_SEP_STREAMING: return "STREAMING";
    case OA2DP_SEP_CLOSING: return "CLOSING";
    case OA2DP_SEP_ABORTING: return "ABORTING";
    }
    return "?";
}

/* ---------------------------------------------------------------------- */
/* Capabilities                                                           */
/* ---------------------------------------------------------------------- */

static int caps_fail(uint8_t *ec, uint8_t *cat, uint8_t code, uint8_t category)
{
    if (ec)
        *ec = code;
    if (cat)
        *cat = category;
    return OA2DP_ERR_PROTO;
}

int oa2dp_avdtp_caps_parse(const uint8_t *buf, size_t len, bool strict,
                           oa2dp_avdtp_caps *c, uint8_t *ec, uint8_t *cat)
{
    memset(c, 0, sizeof(*c));
    size_t i = 0;
    while (i < len) {
        if (len - i < 2)
            return caps_fail(ec, cat, OA2DP_AVDTP_BAD_LENGTH, buf[i]);
        uint8_t category = buf[i], losc = buf[i + 1];
        const uint8_t *v = buf + i + 2;
        if ((size_t)losc > len - i - 2)
            return caps_fail(ec, cat, OA2DP_AVDTP_BAD_LENGTH, category);

        switch (category) {
        case OA2DP_AVDTP_CAT_MEDIA_TRANSPORT:
            if (losc != 0)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_MEDIA_TRANSPORT_FORMAT, category);
            c->media_transport = true;
            break;
        case OA2DP_AVDTP_CAT_REPORTING:
            if (losc != 0)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_PAYLOAD_FORMAT, category);
            c->reporting = true;
            break;
        case OA2DP_AVDTP_CAT_RECOVERY:
            if (losc != 3)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_RECOVERY_FORMAT, category);
            if (v[0] != 0x01)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_RECOVERY_TYPE, category);
            if (v[1] < 0x01 || v[1] > 0x18 || v[2] < 0x01 || v[2] > 0x18)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_RECOVERY_FORMAT, category);
            c->recovery = true;
            c->recovery_type = v[0];
            c->recovery_mrws = v[1];
            c->recovery_mnmp = v[2];
            break;
        case OA2DP_AVDTP_CAT_CONTENT_PROTECTION:
            if (losc < 2 || losc - 2 > OA2DP_AVDTP_CP_MAX)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_CP_FORMAT, category);
            c->content_protection = true;
            c->cp_type = (uint16_t)(v[0] | (v[1] << 8));
            c->cp_value_len = (uint8_t)(losc - 2);
            memcpy(c->cp_value, v + 2, c->cp_value_len);
            break;
        case OA2DP_AVDTP_CAT_HEADER_COMPRESSION:
            if (losc != 1)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_ROHC_FORMAT, category);
            c->header_compression = true;
            c->header_compression_flags = v[0];
            break;
        case OA2DP_AVDTP_CAT_MULTIPLEXING:
            c->multiplexing = true;
            break;
        case OA2DP_AVDTP_CAT_MEDIA_CODEC:
            if (losc < 2)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_PAYLOAD_FORMAT, category);
            if (losc > OA2DP_AVDTP_CODEC_MAX)
                return caps_fail(ec, cat, OA2DP_AVDTP_INVALID_CAPABILITIES, category);
            c->media_codec = true;
            c->codec_len = losc;
            memcpy(c->codec, v, losc);
            break;
        case OA2DP_AVDTP_CAT_DELAY_REPORTING:
            if (losc != 0)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_PAYLOAD_FORMAT, category);
            c->delay_reporting = true;
            break;
        default:
            if (strict)
                return caps_fail(ec, cat, OA2DP_AVDTP_BAD_SERV_CATEGORY, category);
            break;
        }
        i += 2 + (size_t)losc;
    }
    return OA2DP_OK;
}

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool overflow;
} writer;

static void put_cat(writer *w, uint8_t cat, const uint8_t *v, uint8_t n)
{
    if (w->len + 2 + n > w->cap) {
        w->overflow = true;
        return;
    }
    w->buf[w->len++] = cat;
    w->buf[w->len++] = n;
    if (n)
        memcpy(w->buf + w->len, v, n);
    w->len += n;
}

int oa2dp_avdtp_caps_build(const oa2dp_avdtp_caps *c, bool basic_only,
                           uint8_t *buf, size_t cap)
{
    writer w = {buf, cap, 0, false};
    if (c->media_transport)
        put_cat(&w, OA2DP_AVDTP_CAT_MEDIA_TRANSPORT, NULL, 0);
    if (c->reporting)
        put_cat(&w, OA2DP_AVDTP_CAT_REPORTING, NULL, 0);
    if (c->recovery) {
        uint8_t v[3] = {c->recovery_type, c->recovery_mrws, c->recovery_mnmp};
        put_cat(&w, OA2DP_AVDTP_CAT_RECOVERY, v, 3);
    }
    if (c->content_protection) {
        uint8_t v[2 + OA2DP_AVDTP_CP_MAX];
        uint8_t n = c->cp_value_len > OA2DP_AVDTP_CP_MAX ? OA2DP_AVDTP_CP_MAX : c->cp_value_len;
        v[0] = (uint8_t)c->cp_type;
        v[1] = (uint8_t)(c->cp_type >> 8);
        memcpy(v + 2, c->cp_value, n);
        put_cat(&w, OA2DP_AVDTP_CAT_CONTENT_PROTECTION, v, (uint8_t)(2 + n));
    }
    if (c->header_compression)
        put_cat(&w, OA2DP_AVDTP_CAT_HEADER_COMPRESSION, &c->header_compression_flags, 1);
    if (c->media_codec)
        put_cat(&w, OA2DP_AVDTP_CAT_MEDIA_CODEC, c->codec, c->codec_len);
    if (c->delay_reporting && !basic_only)
        put_cat(&w, OA2DP_AVDTP_CAT_DELAY_REPORTING, NULL, 0);
    return w.overflow ? OA2DP_ERR_NOSPC : (int)w.len;
}

/* ---------------------------------------------------------------------- */
/* Setup                                                                  */
/* ---------------------------------------------------------------------- */

void oa2dp_avdtp_init(oa2dp_avdtp *av, const oa2dp_avdtp_callbacks *cb,
                      void *user, uint16_t out_mtu)
{
    memset(av, 0, sizeof(*av));
    av->cb = *cb;
    av->user = user;
    av->out_mtu = out_mtu < 48 ? 48 : out_mtu; /* L2CAP minimum MTU */
}

int oa2dp_avdtp_add_sep(oa2dp_avdtp *av, uint8_t tsep, uint8_t media_type,
                        const oa2dp_avdtp_caps *caps)
{
    if (av->sep_count >= OA2DP_AVDTP_MAX_LOCAL_SEPS)
        return OA2DP_ERR_NOSPC;
    oa2dp_avdtp_sep *s = &av->seps[av->sep_count];
    memset(s, 0, sizeof(*s));
    s->seid = (uint8_t)(av->sep_count + 1);
    s->tsep = tsep & 1;
    s->media_type = media_type & 0x0F;
    s->caps = *caps;
    av->sep_count++;
    return s->seid;
}

oa2dp_avdtp_sep *oa2dp_avdtp_sep_get(oa2dp_avdtp *av, uint8_t seid)
{
    for (uint8_t i = 0; i < av->sep_count; i++)
        if (av->seps[i].seid == seid)
            return &av->seps[i];
    return NULL;
}

static void emit(oa2dp_avdtp *av, const oa2dp_avdtp_event *ev)
{
    if (av->cb.event)
        av->cb.event(av->user, ev);
}

static void set_state(oa2dp_avdtp_sep *s, oa2dp_sep_state st)
{
    s->state = st;
    if (st == OA2DP_SEP_IDLE) {
        memset(&s->config, 0, sizeof(s->config));
        s->remote_seid = 0;
        s->initiator = false;
    }
}

/* ---------------------------------------------------------------------- */
/* Transmit                                                               */
/* ---------------------------------------------------------------------- */

static int send_msg(oa2dp_avdtp *av, uint8_t label, uint8_t msg_type,
                    uint8_t signal_id, const uint8_t *payload, size_t len)
{
    uint8_t pkt[OA2DP_AVDTP_MSG_MAX + 3];
    size_t mtu = av->out_mtu < sizeof(pkt) ? av->out_mtu : sizeof(pkt);
    uint8_t hdr = (uint8_t)(((label & 0x0F) << 4) | (msg_type & 0x03));

    if (len > OA2DP_AVDTP_MSG_MAX)
        return OA2DP_ERR_NOSPC;

    if (len + 2 <= mtu) {
        pkt[0] = hdr | (PKT_SINGLE << 2);
        pkt[1] = signal_id & 0x3F;
        if (len)
            memcpy(pkt + 2, payload, len);
        return av->cb.send(av->user, pkt, len + 2) ? OA2DP_ERR_IO : OA2DP_OK;
    }

    size_t first = mtu - 3, rest = len - first;
    size_t nosp = 1 + (rest + (mtu - 1) - 1) / (mtu - 1);
    if (nosp > 255)
        return OA2DP_ERR_NOSPC;

    pkt[0] = hdr | (PKT_START << 2);
    pkt[1] = (uint8_t)nosp;
    pkt[2] = signal_id & 0x3F;
    memcpy(pkt + 3, payload, first);
    if (av->cb.send(av->user, pkt, mtu))
        return OA2DP_ERR_IO;

    size_t off = first;
    while (off < len) {
        size_t n = len - off;
        bool last = n <= mtu - 1;
        if (!last)
            n = mtu - 1;
        pkt[0] = (uint8_t)(hdr | ((last ? PKT_END : PKT_CONTINUE) << 2));
        memcpy(pkt + 1, payload + off, n);
        if (av->cb.send(av->user, pkt, n + 1))
            return OA2DP_ERR_IO;
        off += n;
    }
    return OA2DP_OK;
}

static uint32_t now_ms(oa2dp_avdtp *av)
{
    return av->cb.now_ms ? av->cb.now_ms(av->user) : 0;
}

static int send_command(oa2dp_avdtp *av, uint8_t signal_id, uint8_t local_seid,
                        uint8_t remote_seid, const oa2dp_avdtp_caps *cfg,
                        const uint8_t *payload, size_t len)
{
    int label = -1;
    for (int i = 0; i < 16; i++) {
        int l = (av->next_label + i) & 0x0F;
        if (!av->pending[l].used) {
            label = l;
            break;
        }
    }
    if (label < 0)
        return OA2DP_ERR_BUSY;

    oa2dp_avdtp_pending *p = &av->pending[label];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->signal_id = signal_id;
    p->local_seid = local_seid;
    p->remote_seid = remote_seid;
    p->sent_ms = now_ms(av);
    if (cfg)
        p->pending_config = *cfg;
    av->next_label = (uint8_t)((label + 1) & 0x0F);

    int r = send_msg(av, (uint8_t)label, OA2DP_AVDTP_MSG_COMMAND, signal_id, payload, len);
    if (r < 0) {
        p->used = false;
        return r;
    }
    return label;
}

/* ---------------------------------------------------------------------- */
/* Initiator API                                                          */
/* ---------------------------------------------------------------------- */

int oa2dp_avdtp_discover(oa2dp_avdtp *av)
{
    return send_command(av, OA2DP_AVDTP_DISCOVER, 0, 0, NULL, NULL, 0);
}

int oa2dp_avdtp_get_capabilities(oa2dp_avdtp *av, uint8_t remote_seid, bool all)
{
    uint8_t p = (uint8_t)(remote_seid << 2);
    return send_command(av, all ? OA2DP_AVDTP_GET_ALL_CAPABILITIES : OA2DP_AVDTP_GET_CAPABILITIES,
                        0, remote_seid, NULL, &p, 1);
}

int oa2dp_avdtp_set_configuration(oa2dp_avdtp *av, uint8_t local_seid,
                                  uint8_t remote_seid, const oa2dp_avdtp_caps *cfg)
{
    uint8_t buf[2 + 128];
    oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(av, local_seid);
    if (!s || !cfg)
        return OA2DP_ERR_INVAL;
    if (s->state != OA2DP_SEP_IDLE)
        return OA2DP_ERR_STATE;
    buf[0] = (uint8_t)(remote_seid << 2);
    buf[1] = (uint8_t)(local_seid << 2);
    int n = oa2dp_avdtp_caps_build(cfg, false, buf + 2, sizeof(buf) - 2);
    if (n < 0)
        return n;
    return send_command(av, OA2DP_AVDTP_SET_CONFIGURATION, local_seid, remote_seid, cfg,
                        buf, (size_t)n + 2);
}

int oa2dp_avdtp_get_configuration(oa2dp_avdtp *av, uint8_t remote_seid)
{
    uint8_t p = (uint8_t)(remote_seid << 2);
    return send_command(av, OA2DP_AVDTP_GET_CONFIGURATION, 0, remote_seid, NULL, &p, 1);
}

int oa2dp_avdtp_reconfigure(oa2dp_avdtp *av, uint8_t local_seid,
                            const oa2dp_avdtp_caps *cfg)
{
    uint8_t buf[1 + 128];
    oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(av, local_seid);
    if (!s || !cfg)
        return OA2DP_ERR_INVAL;
    if (s->state != OA2DP_SEP_OPEN)
        return OA2DP_ERR_STATE;
    /* Only Media Codec and Content Protection may be reconfigured. */
    oa2dp_avdtp_caps only;
    memset(&only, 0, sizeof(only));
    only.media_codec = cfg->media_codec;
    only.codec_len = cfg->codec_len;
    memcpy(only.codec, cfg->codec, sizeof(only.codec));
    only.content_protection = cfg->content_protection;
    only.cp_type = cfg->cp_type;
    only.cp_value_len = cfg->cp_value_len;
    memcpy(only.cp_value, cfg->cp_value, sizeof(only.cp_value));

    buf[0] = (uint8_t)(s->remote_seid << 2);
    int n = oa2dp_avdtp_caps_build(&only, false, buf + 1, sizeof(buf) - 1);
    if (n < 0)
        return n;
    return send_command(av, OA2DP_AVDTP_RECONFIGURE, local_seid, s->remote_seid, &only,
                        buf, (size_t)n + 1);
}

static int simple_cmd(oa2dp_avdtp *av, uint8_t signal_id, uint8_t local_seid,
                      unsigned allowed_states)
{
    oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(av, local_seid);
    if (!s)
        return OA2DP_ERR_INVAL;
    if (!(allowed_states & (1u << s->state)))
        return OA2DP_ERR_STATE;
    uint8_t p = (uint8_t)(s->remote_seid << 2);
    return send_command(av, signal_id, local_seid, s->remote_seid, NULL, &p, 1);
}

#define ST(x) (1u << (x))

int oa2dp_avdtp_open(oa2dp_avdtp *av, uint8_t seid)
{
    return simple_cmd(av, OA2DP_AVDTP_OPEN, seid, ST(OA2DP_SEP_CONFIGURED));
}

int oa2dp_avdtp_start(oa2dp_avdtp *av, uint8_t seid)
{
    return simple_cmd(av, OA2DP_AVDTP_START, seid, ST(OA2DP_SEP_OPEN));
}

int oa2dp_avdtp_suspend(oa2dp_avdtp *av, uint8_t seid)
{
    return simple_cmd(av, OA2DP_AVDTP_SUSPEND, seid, ST(OA2DP_SEP_STREAMING));
}

int oa2dp_avdtp_close(oa2dp_avdtp *av, uint8_t seid)
{
    return simple_cmd(av, OA2DP_AVDTP_CLOSE, seid, ST(OA2DP_SEP_OPEN) | ST(OA2DP_SEP_STREAMING));
}

int oa2dp_avdtp_abort(oa2dp_avdtp *av, uint8_t seid)
{
    return simple_cmd(av, OA2DP_AVDTP_ABORT, seid,
                      ST(OA2DP_SEP_CONFIGURED) | ST(OA2DP_SEP_OPEN) |
                          ST(OA2DP_SEP_STREAMING) | ST(OA2DP_SEP_CLOSING));
}

int oa2dp_avdtp_delay_report(oa2dp_avdtp *av, uint8_t seid, uint16_t delay)
{
    oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(av, seid);
    if (!s)
        return OA2DP_ERR_INVAL;
    if (!(ST(s->state) & (ST(OA2DP_SEP_CONFIGURED) | ST(OA2DP_SEP_OPEN) | ST(OA2DP_SEP_STREAMING))))
        return OA2DP_ERR_STATE;
    uint8_t p[3] = {(uint8_t)(s->remote_seid << 2), (uint8_t)(delay >> 8), (uint8_t)delay};
    return send_command(av, OA2DP_AVDTP_DELAYREPORT, seid, s->remote_seid, NULL, p, 3);
}

void oa2dp_avdtp_media_closed(oa2dp_avdtp *av, uint8_t seid)
{
    oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(av, seid);
    if (s && s->state != OA2DP_SEP_IDLE)
        set_state(s, OA2DP_SEP_IDLE);
}

static void fail_pending(oa2dp_avdtp *av, int label)
{
    oa2dp_avdtp_pending *p = &av->pending[label];
    oa2dp_avdtp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.signal_id = p->signal_id;
    ev.timed_out = true;
    ev.label = (uint8_t)label;
    ev.local_seid = p->local_seid;
    ev.remote_seid = p->remote_seid;
    p->used = false;
    emit(av, &ev);
}

void oa2dp_avdtp_reset(oa2dp_avdtp *av)
{
    for (uint8_t i = 0; i < av->sep_count; i++)
        set_state(&av->seps[i], OA2DP_SEP_IDLE);
    av->rx_active = false;
    av->rx_len = 0;
    for (int l = 0; l < 16; l++)
        if (av->pending[l].used)
            fail_pending(av, l);
}

void oa2dp_avdtp_check_timeouts(oa2dp_avdtp *av)
{
    if (!av->cb.now_ms)
        return;
    uint32_t now = now_ms(av);
    for (int l = 0; l < 16; l++)
        if (av->pending[l].used &&
            (uint32_t)(now - av->pending[l].sent_ms) >= OA2DP_AVDTP_TIMEOUT_MS)
            fail_pending(av, l);
}

/* ---------------------------------------------------------------------- */
/* Acceptor: incoming commands                                            */
/* ---------------------------------------------------------------------- */

static void reply(oa2dp_avdtp *av, uint8_t label, uint8_t type, uint8_t sig,
                  const uint8_t *p, size_t n)
{
    (void)send_msg(av, label, type, sig, p, n);
}

static void reject1(oa2dp_avdtp *av, uint8_t label, uint8_t sig, uint8_t err)
{
    reply(av, label, OA2DP_AVDTP_MSG_REJECT, sig, &err, 1);
}

static void reject2(oa2dp_avdtp *av, uint8_t label, uint8_t sig, uint8_t first, uint8_t err)
{
    uint8_t p[2] = {first, err};
    reply(av, label, OA2DP_AVDTP_MSG_REJECT, sig, p, 2);
}

static void accepted(oa2dp_avdtp *av, oa2dp_avdtp_event *ev, uint8_t label,
                     uint8_t sig, oa2dp_avdtp_sep *s)
{
    ev->signal_id = sig;
    ev->msg_type = OA2DP_AVDTP_MSG_COMMAND;
    ev->label = label;
    if (s) {
        ev->local_seid = s->seid;
        ev->remote_seid = s->remote_seid;
    }
    emit(av, ev);
}

/* Categories present in `cfg` that our SEP did not advertise. */
static uint8_t unsupported_category(const oa2dp_avdtp_caps *ours, const oa2dp_avdtp_caps *cfg)
{
    if (cfg->reporting && !ours->reporting) return OA2DP_AVDTP_CAT_REPORTING;
    if (cfg->recovery && !ours->recovery) return OA2DP_AVDTP_CAT_RECOVERY;
    if (cfg->content_protection && !ours->content_protection) return OA2DP_AVDTP_CAT_CONTENT_PROTECTION;
    if (cfg->header_compression && !ours->header_compression) return OA2DP_AVDTP_CAT_HEADER_COMPRESSION;
    if (cfg->multiplexing && !ours->multiplexing) return OA2DP_AVDTP_CAT_MULTIPLEXING;
    if (cfg->delay_reporting && !ours->delay_reporting) return OA2DP_AVDTP_CAT_DELAY_REPORTING;
    return 0;
}

static void handle_command(oa2dp_avdtp *av, uint8_t label, uint8_t sig,
                           const uint8_t *p, size_t n)
{
    oa2dp_avdtp_event ev;
    oa2dp_avdtp_sep *s = NULL;
    uint8_t out[OA2DP_AVDTP_MSG_MAX];
    memset(&ev, 0, sizeof(ev));

    switch (sig) {
    case OA2DP_AVDTP_DISCOVER: {
        size_t o = 0;
        if (av->sep_count == 0) {
            reject1(av, label, sig, OA2DP_AVDTP_NOT_SUPPORTED_COMMAND);
            return;
        }
        for (uint8_t i = 0; i < av->sep_count; i++) {
            const oa2dp_avdtp_sep *x = &av->seps[i];
            out[o++] = (uint8_t)((x->seid << 2) | (x->state != OA2DP_SEP_IDLE ? 0x02 : 0));
            out[o++] = (uint8_t)((x->media_type << 4) | (x->tsep << 3));
        }
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, out, o);
        accepted(av, &ev, label, sig, NULL);
        return;
    }

    case OA2DP_AVDTP_GET_CAPABILITIES:
    case OA2DP_AVDTP_GET_ALL_CAPABILITIES: {
        if (n < 1) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        if (!(s = oa2dp_avdtp_sep_get(av, p[0] >> 2))) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_ACP_SEID);
            return;
        }
        int len = oa2dp_avdtp_caps_build(&s->caps, sig == OA2DP_AVDTP_GET_CAPABILITIES,
                                         out, sizeof(out));
        if (len < 0) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, out, (size_t)len);
        return;
    }

    case OA2DP_AVDTP_SET_CONFIGURATION: {
        uint8_t ec = 0, cat = 0;
        if (n < 2) {
            reject2(av, label, sig, 0, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        if (!(s = oa2dp_avdtp_sep_get(av, p[0] >> 2))) {
            reject2(av, label, sig, 0, OA2DP_AVDTP_BAD_ACP_SEID);
            return;
        }
        if (s->state != OA2DP_SEP_IDLE) {
            reject2(av, label, sig, 0, OA2DP_AVDTP_SEP_IN_USE);
            return;
        }
        if (oa2dp_avdtp_caps_parse(p + 2, n - 2, true, &ev.caps, &ec, &cat) < 0) {
            reject2(av, label, sig, cat, ec);
            return;
        }
        if (!ev.caps.media_transport) {
            reject2(av, label, sig, OA2DP_AVDTP_CAT_MEDIA_TRANSPORT, OA2DP_AVDTP_INVALID_CAPABILITIES);
            return;
        }
        if (!ev.caps.media_codec) {
            reject2(av, label, sig, OA2DP_AVDTP_CAT_MEDIA_CODEC, OA2DP_AVDTP_INVALID_CAPABILITIES);
            return;
        }
        if ((cat = unsupported_category(&s->caps, &ev.caps))) {
            reject2(av, label, sig, cat, OA2DP_AVDTP_UNSUPPORTED_CONFIGURATION);
            return;
        }
        if (av->cb.check_config) {
            cat = OA2DP_AVDTP_CAT_MEDIA_CODEC;
            if ((ec = av->cb.check_config(av->user, s->seid, &ev.caps, &cat))) {
                reject2(av, label, sig, cat, ec);
                return;
            }
        }
        set_state(s, OA2DP_SEP_CONFIGURED);
        s->config = ev.caps;
        s->remote_seid = p[1] >> 2;
        s->initiator = false;
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, NULL, 0);
        accepted(av, &ev, label, sig, s);
        return;
    }

    case OA2DP_AVDTP_GET_CONFIGURATION: {
        if (n < 1) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        if (!(s = oa2dp_avdtp_sep_get(av, p[0] >> 2))) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_ACP_SEID);
            return;
        }
        if (s->state == OA2DP_SEP_IDLE) {
            reject1(av, label, sig, OA2DP_AVDTP_SEP_NOT_IN_USE);
            return;
        }
        int len = oa2dp_avdtp_caps_build(&s->config, false, out, sizeof(out));
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, out, len < 0 ? 0 : (size_t)len);
        return;
    }

    case OA2DP_AVDTP_RECONFIGURE: {
        uint8_t ec = 0, cat = 0;
        if (n < 1) {
            reject2(av, label, sig, 0, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        if (!(s = oa2dp_avdtp_sep_get(av, p[0] >> 2))) {
            reject2(av, label, sig, 0, OA2DP_AVDTP_BAD_ACP_SEID);
            return;
        }
        if (s->state != OA2DP_SEP_OPEN) {
            reject2(av, label, sig, 0, OA2DP_AVDTP_SEP_NOT_IN_USE);
            return;
        }
        if (oa2dp_avdtp_caps_parse(p + 1, n - 1, true, &ev.caps, &ec, &cat) < 0) {
            reject2(av, label, sig, cat, ec);
            return;
        }
        if (ev.caps.media_transport || ev.caps.reporting || ev.caps.recovery ||
            ev.caps.header_compression || ev.caps.multiplexing || ev.caps.delay_reporting) {
            uint8_t bad = ev.caps.media_transport ? OA2DP_AVDTP_CAT_MEDIA_TRANSPORT
                          : ev.caps.reporting     ? OA2DP_AVDTP_CAT_REPORTING
                          : ev.caps.recovery      ? OA2DP_AVDTP_CAT_RECOVERY
                          : ev.caps.header_compression ? OA2DP_AVDTP_CAT_HEADER_COMPRESSION
                          : ev.caps.multiplexing  ? OA2DP_AVDTP_CAT_MULTIPLEXING
                                                  : OA2DP_AVDTP_CAT_DELAY_REPORTING;
            reject2(av, label, sig, bad, OA2DP_AVDTP_INVALID_CAPABILITIES);
            return;
        }
        if (ev.caps.content_protection && !s->caps.content_protection) {
            reject2(av, label, sig, OA2DP_AVDTP_CAT_CONTENT_PROTECTION,
                    OA2DP_AVDTP_UNSUPPORTED_CONFIGURATION);
            return;
        }
        if (ev.caps.media_codec && av->cb.check_config) {
            cat = OA2DP_AVDTP_CAT_MEDIA_CODEC;
            if ((ec = av->cb.check_config(av->user, s->seid, &ev.caps, &cat))) {
                reject2(av, label, sig, cat, ec);
                return;
            }
        }
        if (ev.caps.media_codec) {
            s->config.media_codec = true;
            s->config.codec_len = ev.caps.codec_len;
            memcpy(s->config.codec, ev.caps.codec, sizeof(s->config.codec));
        }
        if (ev.caps.content_protection) {
            s->config.content_protection = true;
            s->config.cp_type = ev.caps.cp_type;
            s->config.cp_value_len = ev.caps.cp_value_len;
            memcpy(s->config.cp_value, ev.caps.cp_value, sizeof(s->config.cp_value));
        }
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, NULL, 0);
        accepted(av, &ev, label, sig, s);
        return;
    }

    case OA2DP_AVDTP_OPEN:
    case OA2DP_AVDTP_CLOSE:
    case OA2DP_AVDTP_ABORT: {
        if (n < 1) {
            if (sig != OA2DP_AVDTP_ABORT)
                reject1(av, label, sig, OA2DP_AVDTP_BAD_LENGTH);
            return; /* ABORT is never rejected; malformed ones are ignored */
        }
        if (!(s = oa2dp_avdtp_sep_get(av, p[0] >> 2))) {
            if (sig != OA2DP_AVDTP_ABORT)
                reject1(av, label, sig, OA2DP_AVDTP_BAD_ACP_SEID);
            return;
        }
        if (sig == OA2DP_AVDTP_ABORT) {
            oa2dp_sep_state prev = s->state;
            uint8_t remote = s->remote_seid;
            set_state(s, (prev == OA2DP_SEP_IDLE || prev == OA2DP_SEP_CONFIGURED)
                             ? OA2DP_SEP_IDLE
                             : OA2DP_SEP_ABORTING);
            reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, NULL, 0);
            ev.remote_seid = remote;
            ev.signal_id = sig;
            ev.msg_type = OA2DP_AVDTP_MSG_COMMAND;
            ev.label = label;
            ev.local_seid = s->seid;
            emit(av, &ev);
            return;
        }
        bool ok = sig == OA2DP_AVDTP_OPEN
                      ? s->state == OA2DP_SEP_CONFIGURED
                      : (s->state == OA2DP_SEP_OPEN || s->state == OA2DP_SEP_STREAMING);
        if (!ok) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_STATE);
            return;
        }
        if (av->cb.approve) {
            uint8_t e = av->cb.approve(av->user, s->seid, sig);
            if (e) {
                reject1(av, label, sig, e);
                return;
            }
        }
        set_state(s, sig == OA2DP_AVDTP_OPEN ? OA2DP_SEP_OPEN : OA2DP_SEP_CLOSING);
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, NULL, 0);
        accepted(av, &ev, label, sig, s);
        return;
    }

    case OA2DP_AVDTP_START:
    case OA2DP_AVDTP_SUSPEND: {
        oa2dp_sep_state need = sig == OA2DP_AVDTP_START ? OA2DP_SEP_OPEN : OA2DP_SEP_STREAMING;
        oa2dp_sep_state next = sig == OA2DP_AVDTP_START ? OA2DP_SEP_STREAMING : OA2DP_SEP_OPEN;
        if (n < 1) {
            reject2(av, label, sig, 0, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        /* Validate every SEID first: the command is atomic. */
        for (size_t i = 0; i < n; i++) {
            oa2dp_avdtp_sep *x = oa2dp_avdtp_sep_get(av, p[i] >> 2);
            if (!x) {
                reject2(av, label, sig, p[i], OA2DP_AVDTP_BAD_ACP_SEID);
                return;
            }
            if (x->state != need) {
                reject2(av, label, sig, p[i], OA2DP_AVDTP_BAD_STATE);
                return;
            }
            if (av->cb.approve) {
                uint8_t e = av->cb.approve(av->user, x->seid, sig);
                if (e) {
                    reject2(av, label, sig, p[i], e);
                    return;
                }
            }
        }
        for (size_t i = 0; i < n; i++)
            set_state(oa2dp_avdtp_sep_get(av, p[i] >> 2), next);
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, NULL, 0);
        for (size_t i = 0; i < n; i++) {
            memset(&ev, 0, sizeof(ev));
            accepted(av, &ev, label, sig, oa2dp_avdtp_sep_get(av, p[i] >> 2));
        }
        return;
    }

    case OA2DP_AVDTP_SECURITY_CONTROL: {
        if (n < 1) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        if (!(s = oa2dp_avdtp_sep_get(av, p[0] >> 2))) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_ACP_SEID);
            return;
        }
        /* No content protection scheme is implemented. */
        reject1(av, label, sig, s->state == OA2DP_SEP_IDLE ? OA2DP_AVDTP_SEP_NOT_IN_USE
                                                           : OA2DP_AVDTP_BAD_CP_FORMAT);
        return;
    }

    case OA2DP_AVDTP_DELAYREPORT: {
        if (n < 3) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_LENGTH);
            return;
        }
        if (!(s = oa2dp_avdtp_sep_get(av, p[0] >> 2))) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_ACP_SEID);
            return;
        }
        if (s->state != OA2DP_SEP_CONFIGURED && s->state != OA2DP_SEP_OPEN &&
            s->state != OA2DP_SEP_STREAMING) {
            reject1(av, label, sig, OA2DP_AVDTP_BAD_STATE);
            return;
        }
        ev.delay = (uint16_t)((p[1] << 8) | p[2]);
        reply(av, label, OA2DP_AVDTP_MSG_ACCEPT, sig, NULL, 0);
        accepted(av, &ev, label, sig, s);
        return;
    }

    default:
        reply(av, label, OA2DP_AVDTP_MSG_GENERAL_REJECT, sig, NULL, 0);
        return;
    }
}

/* ---------------------------------------------------------------------- */
/* Initiator: responses                                                   */
/* ---------------------------------------------------------------------- */

static int handle_response(oa2dp_avdtp *av, uint8_t label, uint8_t type,
                           uint8_t sig, const uint8_t *p, size_t n)
{
    oa2dp_avdtp_pending *pend = &av->pending[label];
    if (!pend->used || pend->signal_id != sig)
        return OA2DP_ERR_PROTO; /* unsolicited or stale response */

    oa2dp_avdtp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.signal_id = sig;
    ev.msg_type = type;
    ev.label = label;
    ev.local_seid = pend->local_seid;
    ev.remote_seid = pend->remote_seid;
    oa2dp_avdtp_caps cfg = pend->pending_config;
    pend->used = false;

    oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(av, ev.local_seid);
    int rc = OA2DP_OK;

    if (type == OA2DP_AVDTP_MSG_ACCEPT) {
        uint8_t ec, cat;
        switch (sig) {
        case OA2DP_AVDTP_DISCOVER:
            for (size_t i = 0; i + 1 < n && ev.sep_count < OA2DP_AVDTP_MAX_SEPS; i += 2) {
                oa2dp_avdtp_sep_info *x = &ev.seps[ev.sep_count++];
                x->seid = p[i] >> 2;
                x->in_use = (p[i] & 0x02) != 0;
                x->media_type = p[i + 1] >> 4;
                x->tsep = (p[i + 1] >> 3) & 1;
            }
            break;
        case OA2DP_AVDTP_GET_CAPABILITIES:
        case OA2DP_AVDTP_GET_ALL_CAPABILITIES:
        case OA2DP_AVDTP_GET_CONFIGURATION:
            if (oa2dp_avdtp_caps_parse(p, n, false, &ev.caps, &ec, &cat) < 0)
                rc = OA2DP_ERR_PROTO; /* still reported, with whatever parsed */
            break;
        case OA2DP_AVDTP_SET_CONFIGURATION:
            if (s && s->state == OA2DP_SEP_IDLE) {
                set_state(s, OA2DP_SEP_CONFIGURED);
                s->config = cfg;
                s->remote_seid = ev.remote_seid;
                s->initiator = true;
            }
            ev.caps = cfg;
            break;
        case OA2DP_AVDTP_RECONFIGURE:
            if (s) {
                if (cfg.media_codec) {
                    s->config.media_codec = true;
                    s->config.codec_len = cfg.codec_len;
                    memcpy(s->config.codec, cfg.codec, sizeof(cfg.codec));
                }
                if (cfg.content_protection) {
                    s->config.content_protection = true;
                    s->config.cp_type = cfg.cp_type;
                    s->config.cp_value_len = cfg.cp_value_len;
                    memcpy(s->config.cp_value, cfg.cp_value, sizeof(cfg.cp_value));
                }
            }
            ev.caps = cfg;
            break;
        case OA2DP_AVDTP_OPEN:
            if (s && s->state == OA2DP_SEP_CONFIGURED)
                set_state(s, OA2DP_SEP_OPEN);
            break;
        case OA2DP_AVDTP_START:
            if (s && s->state == OA2DP_SEP_OPEN)
                set_state(s, OA2DP_SEP_STREAMING);
            break;
        case OA2DP_AVDTP_SUSPEND:
            if (s && s->state == OA2DP_SEP_STREAMING)
                set_state(s, OA2DP_SEP_OPEN);
            break;
        case OA2DP_AVDTP_CLOSE:
            if (s)
                set_state(s, OA2DP_SEP_CLOSING);
            break;
        case OA2DP_AVDTP_ABORT:
            if (s)
                set_state(s, s->state == OA2DP_SEP_CONFIGURED ? OA2DP_SEP_IDLE
                                                              : OA2DP_SEP_ABORTING);
            break;
        default:
            break;
        }
    } else if (type == OA2DP_AVDTP_MSG_REJECT) {
        switch (sig) {
        case OA2DP_AVDTP_SET_CONFIGURATION:
        case OA2DP_AVDTP_RECONFIGURE:
            if (n >= 2) {
                ev.error_category = p[0];
                ev.error_code = p[1];
            }
            break;
        case OA2DP_AVDTP_START:
        case OA2DP_AVDTP_SUSPEND:
            if (n >= 2) {
                ev.remote_seid = p[0] >> 2;
                ev.error_code = p[1];
            }
            break;
        default:
            if (n >= 1)
                ev.error_code = p[0];
            break;
        }
    }
    emit(av, &ev);
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */
/* ---------------------------------------------------------------------- */

static int dispatch(oa2dp_avdtp *av, uint8_t label, uint8_t type, uint8_t sig,
                    const uint8_t *p, size_t n)
{
    if (type == OA2DP_AVDTP_MSG_COMMAND) {
        handle_command(av, label, sig, p, n);
        return OA2DP_OK;
    }
    return handle_response(av, label, type, sig, p, n);
}

int oa2dp_avdtp_input(oa2dp_avdtp *av, const uint8_t *pdu, size_t len)
{
    if (!pdu || len < 1)
        return OA2DP_ERR_INVAL;

    uint8_t label = pdu[0] >> 4;
    uint8_t pkt = (pdu[0] >> 2) & 0x03;
    uint8_t type = pdu[0] & 0x03;

    switch (pkt) {
    case PKT_SINGLE:
        if (len < 2)
            return OA2DP_ERR_PROTO;
        av->rx_active = false; /* a single packet aborts any reassembly */
        return dispatch(av, label, type, pdu[1] & 0x3F, pdu + 2, len - 2);

    case PKT_START:
        if (len < 3 || pdu[1] < 2)
            return OA2DP_ERR_PROTO;
        if (len - 3 > sizeof(av->rx_buf))
            return OA2DP_ERR_NOSPC;
        av->rx_active = true;
        av->rx_label = label;
        av->rx_msg_type = type;
        av->rx_signal = pdu[2] & 0x3F;
        av->rx_remaining = (uint8_t)(pdu[1] - 1);
        av->rx_len = len - 3;
        memcpy(av->rx_buf, pdu + 3, av->rx_len);
        return OA2DP_OK;

    default: /* CONTINUE / END */
        if (!av->rx_active || label != av->rx_label || type != av->rx_msg_type)
            return OA2DP_ERR_PROTO;
        if (av->rx_len + (len - 1) > sizeof(av->rx_buf) || av->rx_remaining == 0) {
            av->rx_active = false;
            return OA2DP_ERR_NOSPC;
        }
        memcpy(av->rx_buf + av->rx_len, pdu + 1, len - 1);
        av->rx_len += len - 1;
        av->rx_remaining--;
        if (pkt == PKT_CONTINUE)
            return OA2DP_OK;
        av->rx_active = false;
        if (av->rx_remaining != 0)
            return OA2DP_ERR_PROTO; /* END arrived before all CONTINUE packets */
        {
            /* Copy out: a handler may cause re-entrant input (loopback). */
            uint8_t msg[OA2DP_AVDTP_MSG_MAX];
            size_t n = av->rx_len;
            memcpy(msg, av->rx_buf, n);
            return dispatch(av, label, type, av->rx_signal, msg, n);
        }
    }
}
