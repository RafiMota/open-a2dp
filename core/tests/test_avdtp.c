/* SPDX-License-Identifier: MIT */
/* Two AVDTP instances (PC source + simulated headset sink) talk through an
 * in-memory queue. */
#include "oa2dp/a2dp_codecs.h"
#include "oa2dp/avdtp.h"
#include "test.h"

#define QMAX 64
#define EVMAX 64

typedef struct {
    uint8_t data[OA2DP_AVDTP_MSG_MAX + 3];
    size_t len;
} msg;

typedef struct {
    oa2dp_avdtp av;
    msg out[QMAX];
    int head, tail;
    int sent_packets;
    oa2dp_avdtp_event ev[EVMAX];
    int nev;
    uint32_t clock;
} peer;

static int on_send(void *user, const uint8_t *pdu, size_t len)
{
    peer *p = user;
    if (p->tail - p->head >= QMAX)
        return -1;
    msg *m = &p->out[p->tail++ % QMAX];
    memcpy(m->data, pdu, len);
    m->len = len;
    p->sent_packets++;
    return 0;
}

static uint32_t on_now(void *user) { return ((peer *)user)->clock; }

static void on_event(void *user, const oa2dp_avdtp_event *ev)
{
    peer *p = user;
    if (p->nev < EVMAX)
        p->ev[p->nev++] = *ev;
}

/* Validate the codec part of a configuration against the SEP capability. */
static uint8_t on_check(void *user, uint8_t seid, const oa2dp_avdtp_caps *cfg, uint8_t *cat)
{
    peer *p = user;
    oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(&p->av, seid);
    oa2dp_codec_caps mine, theirs;
    *cat = OA2DP_AVDTP_CAT_MEDIA_CODEC;
    if (oa2dp_codec_parse(s->caps.codec, s->caps.codec_len, &mine) ||
        oa2dp_codec_parse(cfg->codec, cfg->codec_len, &theirs))
        return OA2DP_A2DP_NOT_SUPPORTED_CODEC_TYPE;
    return oa2dp_codec_check_config(&mine, &theirs);
}

static void peer_init(peer *p)
{
    memset(p, 0, sizeof(*p));
    oa2dp_avdtp_callbacks cb = {on_send, on_now, on_event, on_check, NULL};
    oa2dp_avdtp_init(&p->av, &cb, p, 48);
}

static void pump(peer *a, peer *b)
{
    bool moved = true;
    while (moved) {
        moved = false;
        while (a->head < a->tail) {
            msg *m = &a->out[a->head++ % QMAX];
            oa2dp_avdtp_input(&b->av, m->data, m->len);
            moved = true;
        }
        while (b->head < b->tail) {
            msg *m = &b->out[b->head++ % QMAX];
            oa2dp_avdtp_input(&a->av, m->data, m->len);
            moved = true;
        }
    }
}

static const oa2dp_avdtp_event *last(peer *p) { return p->nev ? &p->ev[p->nev - 1] : NULL; }

static void make_caps(oa2dp_avdtp_caps *c, const uint8_t *codec, size_t n, bool delay)
{
    memset(c, 0, sizeof(*c));
    c->media_transport = true;
    c->media_codec = true;
    c->codec_len = (uint8_t)n;
    memcpy(c->codec, codec, n);
    c->delay_reporting = delay;
}

static void local_caps(oa2dp_avdtp_caps *c, oa2dp_codec_id id)
{
    oa2dp_codec_caps cc;
    uint8_t buf[OA2DP_CODEC_IE_MAX];
    oa2dp_codec_local_caps(id, &cc);
    int n = oa2dp_codec_build(&cc, buf, sizeof(buf));
    make_caps(c, buf, (size_t)n, true);
}

static const uint8_t HS_SBC[] = {0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35};
static const uint8_t HS_AAC[] = {0x00, 0x02, 0x80, 0x01, 0x8C, 0x84, 0xE2, 0x00};
static const uint8_t HS_LDAC[] = {0x00, 0xFF, 0x2D, 0x01, 0, 0, 0xAA, 0x00, 0x3C, 0x07};

void test_avdtp(void)
{
    static peer pc, hs;
    oa2dp_avdtp_caps c;
    peer_init(&pc);
    peer_init(&hs);

    local_caps(&c, OA2DP_CODEC_SBC);
    int pc_sbc = oa2dp_avdtp_add_sep(&pc.av, OA2DP_AVDTP_TSEP_SRC, 0, &c);
    local_caps(&c, OA2DP_CODEC_LDAC);
    int pc_ldac = oa2dp_avdtp_add_sep(&pc.av, OA2DP_AVDTP_TSEP_SRC, 0, &c);
    CHECK_EQ(pc_sbc, 1);
    CHECK_EQ(pc_ldac, 2);

    make_caps(&c, HS_SBC, sizeof(HS_SBC), false);
    oa2dp_avdtp_add_sep(&hs.av, OA2DP_AVDTP_TSEP_SNK, 0, &c);
    make_caps(&c, HS_AAC, sizeof(HS_AAC), false);
    oa2dp_avdtp_add_sep(&hs.av, OA2DP_AVDTP_TSEP_SNK, 0, &c);
    make_caps(&c, HS_LDAC, sizeof(HS_LDAC), true);
    int hs_ldac = oa2dp_avdtp_add_sep(&hs.av, OA2DP_AVDTP_TSEP_SNK, 0, &c);
    /* A big SEP (unknown 30-byte vendor codec, content protection, recovery)
     * whose capabilities exceed the 48-byte signalling MTU. */
    {
        uint8_t big_codec[30] = {0x00, 0xFF, 0x34, 0x12, 0, 0, 0x01, 0x00};
        for (int i = 8; i < 30; i++)
            big_codec[i] = (uint8_t)i;
        make_caps(&c, big_codec, sizeof(big_codec), true);
    }
    c.content_protection = true;
    c.cp_type = 0x0002; /* SCMS-T */
    c.cp_value_len = 14;
    for (int i = 0; i < 14; i++)
        c.cp_value[i] = (uint8_t)(0xA0 + i);
    c.recovery = true;
    c.recovery_type = 1;
    c.recovery_mrws = 2;
    c.recovery_mnmp = 2;
    c.reporting = true;
    int hs_big = oa2dp_avdtp_add_sep(&hs.av, OA2DP_AVDTP_TSEP_SNK, 0, &c);

    /* --- DISCOVER ---------------------------------------------------- */
    CHECK(oa2dp_avdtp_discover(&pc.av) >= 0);
    pump(&pc, &hs);
    const oa2dp_avdtp_event *e = last(&pc);
    CHECK(e && e->signal_id == OA2DP_AVDTP_DISCOVER && e->msg_type == OA2DP_AVDTP_MSG_ACCEPT);
    CHECK_EQ(e->sep_count, 4);
    CHECK_EQ(e->seps[2].seid, hs_ldac);
    CHECK_EQ(e->seps[2].tsep, OA2DP_AVDTP_TSEP_SNK);
    CHECK(!e->seps[2].in_use);

    /* --- GET_ALL_CAPABILITIES for each, then pick the best codec ------ */
    oa2dp_codec_caps remote[3];
    for (int i = 0; i < 3; i++) {
        CHECK(oa2dp_avdtp_get_capabilities(&pc.av, (uint8_t)(i + 1), true) >= 0);
        pump(&pc, &hs);
        e = last(&pc);
        CHECK(e->msg_type == OA2DP_AVDTP_MSG_ACCEPT && e->caps.media_codec);
        CHECK_EQ(oa2dp_codec_parse(e->caps.codec, e->caps.codec_len, &remote[i]), OA2DP_OK);
        if (i == 2)
            CHECK(e->caps.delay_reporting);
    }
    oa2dp_settings s;
    oa2dp_codec_caps chosen;
    oa2dp_settings_default(&s);
    CHECK_EQ(oa2dp_codec_choose_best(remote, 3, &s, &chosen), 2);

    /* Basic GET_CAPABILITIES must not include delay reporting. */
    oa2dp_avdtp_get_capabilities(&pc.av, (uint8_t)hs_ldac, false);
    pump(&pc, &hs);
    CHECK(!last(&pc)->caps.delay_reporting);

    /* Fragmented response */
    int before = hs.sent_packets;
    oa2dp_avdtp_get_capabilities(&pc.av, (uint8_t)hs_big, true);
    pump(&pc, &hs);
    CHECK(hs.sent_packets - before >= 2);
    e = last(&pc);
    CHECK(e->msg_type == OA2DP_AVDTP_MSG_ACCEPT);
    CHECK(e->caps.content_protection && e->caps.cp_value_len == 14 && e->caps.cp_value[13] == 0xAD);
    CHECK(e->caps.recovery && e->caps.reporting && e->caps.delay_reporting);
    CHECK_EQ(e->caps.codec_len, 30);
    CHECK_EQ(e->caps.codec[29], 29);

    /* --- SET_CONFIGURATION ------------------------------------------- */
    oa2dp_avdtp_caps cfg;
    uint8_t ie[OA2DP_CODEC_IE_MAX];
    int n = oa2dp_codec_build(&chosen, ie, sizeof(ie));
    make_caps(&cfg, ie, (size_t)n, true);
    CHECK(oa2dp_avdtp_set_configuration(&pc.av, (uint8_t)pc_ldac, (uint8_t)hs_ldac, &cfg) >= 0);
    pump(&pc, &hs);
    CHECK(last(&pc)->signal_id == OA2DP_AVDTP_SET_CONFIGURATION &&
          last(&pc)->msg_type == OA2DP_AVDTP_MSG_ACCEPT);
    CHECK(last(&hs)->signal_id == OA2DP_AVDTP_SET_CONFIGURATION &&
          last(&hs)->msg_type == OA2DP_AVDTP_MSG_COMMAND);
    CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_ldac)->state, OA2DP_SEP_CONFIGURED);
    CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac)->state, OA2DP_SEP_CONFIGURED);
    CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac)->remote_seid, pc_ldac);

    /* Same remote SEP again: SEP_IN_USE */
    local_caps(&cfg, OA2DP_CODEC_SBC);
    oa2dp_avdtp_set_configuration(&pc.av, (uint8_t)pc_sbc, (uint8_t)hs_ldac, &cfg);
    pump(&pc, &hs);
    CHECK(last(&pc)->msg_type == OA2DP_AVDTP_MSG_REJECT);
    CHECK_EQ(last(&pc)->error_code, OA2DP_AVDTP_SEP_IN_USE);
    CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_sbc)->state, OA2DP_SEP_IDLE);

    /* Codec the headset cannot do (capability mask, not a configuration) */
    local_caps(&cfg, OA2DP_CODEC_SBC);
    cfg.delay_reporting = false;
    oa2dp_avdtp_set_configuration(&pc.av, (uint8_t)pc_sbc, 1, &cfg);
    pump(&pc, &hs);
    CHECK(last(&pc)->msg_type == OA2DP_AVDTP_MSG_REJECT);
    CHECK_EQ(last(&pc)->error_category, OA2DP_AVDTP_CAT_MEDIA_CODEC);
    CHECK_EQ(last(&pc)->error_code, OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY);

    /* Delay reporting requested on a SEP that does not support it */
    {
        oa2dp_codec_caps sbc_cfg;
        oa2dp_codec_select(NULL, &remote[0], &s, &sbc_cfg);
        n = oa2dp_codec_build(&sbc_cfg, ie, sizeof(ie));
        make_caps(&cfg, ie, (size_t)n, true);
        oa2dp_avdtp_set_configuration(&pc.av, (uint8_t)pc_sbc, 1, &cfg);
        pump(&pc, &hs);
        CHECK_EQ(last(&pc)->error_category, OA2DP_AVDTP_CAT_DELAY_REPORTING);
        CHECK_EQ(last(&pc)->error_code, OA2DP_AVDTP_UNSUPPORTED_CONFIGURATION);
    }

    /* --- OPEN / START / DELAYREPORT / SUSPEND -------------------------- */
    CHECK(oa2dp_avdtp_open(&pc.av, (uint8_t)pc_ldac) >= 0);
    pump(&pc, &hs);
    CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_ldac)->state, OA2DP_SEP_OPEN);
    CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac)->state, OA2DP_SEP_OPEN);

    CHECK(oa2dp_avdtp_start(&pc.av, (uint8_t)pc_ldac) >= 0);
    pump(&pc, &hs);
    CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_ldac)->state, OA2DP_SEP_STREAMING);
    CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac)->state, OA2DP_SEP_STREAMING);
    CHECK_EQ(oa2dp_avdtp_start(&pc.av, (uint8_t)pc_ldac), OA2DP_ERR_STATE);

    /* Raw START from the headset for an already-streaming SEP: BAD_STATE */
    {
        uint8_t raw[] = {0x50 | OA2DP_AVDTP_MSG_COMMAND, OA2DP_AVDTP_START, (uint8_t)(pc_ldac << 2)};
        oa2dp_avdtp_input(&pc.av, raw, sizeof(raw));
        msg *m = &pc.out[(pc.tail - 1) % QMAX];
        CHECK_EQ(m->data[0], 0x50 | OA2DP_AVDTP_MSG_REJECT);
        CHECK_EQ(m->data[2], pc_ldac << 2);
        CHECK_EQ(m->data[3], OA2DP_AVDTP_BAD_STATE);
        pc.head = pc.tail; /* drop it: the headset never asked */
    }

    CHECK(oa2dp_avdtp_delay_report(&hs.av, (uint8_t)hs_ldac, 1500) >= 0);
    pump(&pc, &hs);
    CHECK(last(&pc)->signal_id == OA2DP_AVDTP_DELAYREPORT &&
          last(&pc)->msg_type == OA2DP_AVDTP_MSG_COMMAND);
    CHECK_EQ(last(&pc)->delay, 1500);
    CHECK_EQ(last(&pc)->local_seid, pc_ldac);

    CHECK(oa2dp_avdtp_suspend(&pc.av, (uint8_t)pc_ldac) >= 0);
    pump(&pc, &hs);
    CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_ldac)->state, OA2DP_SEP_OPEN);
    CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac)->state, OA2DP_SEP_OPEN);

    /* --- RECONFIGURE to 96 kHz (dynamic sample rate, F4.3) ------------- */
    CHECK_EQ(oa2dp_codec_set_rate(&chosen, &remote[2], 96000), OA2DP_OK);
    n = oa2dp_codec_build(&chosen, ie, sizeof(ie));
    make_caps(&cfg, ie, (size_t)n, false);
    CHECK(oa2dp_avdtp_reconfigure(&pc.av, (uint8_t)pc_ldac, &cfg) >= 0);
    pump(&pc, &hs);
    CHECK(last(&pc)->signal_id == OA2DP_AVDTP_RECONFIGURE &&
          last(&pc)->msg_type == OA2DP_AVDTP_MSG_ACCEPT);
    {
        oa2dp_codec_caps now;
        oa2dp_avdtp_sep *x = oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac);
        CHECK_EQ(oa2dp_codec_parse(x->config.codec, x->config.codec_len, &now), OA2DP_OK);
        CHECK_EQ(oa2dp_codec_sample_rate(&now), 96000);
        CHECK(x->config.media_transport); /* untouched by RECONFIGURE */
        x = oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_ldac);
        CHECK_EQ(oa2dp_codec_parse(x->config.codec, x->config.codec_len, &now), OA2DP_OK);
        CHECK_EQ(oa2dp_codec_sample_rate(&now), 96000);
    }

    /* RECONFIGURE carrying Media Transport is invalid */
    {
        uint8_t raw[16] = {0x60, OA2DP_AVDTP_RECONFIGURE, (uint8_t)(hs_ldac << 2),
                           OA2DP_AVDTP_CAT_MEDIA_TRANSPORT, 0};
        oa2dp_avdtp_input(&hs.av, raw, 5);
        msg *m = &hs.out[(hs.tail - 1) % QMAX];
        CHECK_EQ(m->data[0] & 3, OA2DP_AVDTP_MSG_REJECT);
        CHECK_EQ(m->data[2], OA2DP_AVDTP_CAT_MEDIA_TRANSPORT);
        CHECK_EQ(m->data[3], OA2DP_AVDTP_INVALID_CAPABILITIES);
        hs.head = hs.tail;
    }

    /* --- GET_CONFIGURATION ------------------------------------------- */
    oa2dp_avdtp_get_configuration(&pc.av, (uint8_t)hs_ldac);
    pump(&pc, &hs);
    CHECK(last(&pc)->msg_type == OA2DP_AVDTP_MSG_ACCEPT && last(&pc)->caps.media_codec &&
          last(&pc)->caps.delay_reporting);

    /* --- CLOSE -------------------------------------------------------- */
    CHECK(oa2dp_avdtp_close(&pc.av, (uint8_t)pc_ldac) >= 0);
    pump(&pc, &hs);
    CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_ldac)->state, OA2DP_SEP_CLOSING);
    CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac)->state, OA2DP_SEP_CLOSING);
    oa2dp_avdtp_media_closed(&pc.av, (uint8_t)pc_ldac);
    oa2dp_avdtp_media_closed(&hs.av, (uint8_t)hs_ldac);
    CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_ldac)->state, OA2DP_SEP_IDLE);
    CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, (uint8_t)hs_ldac)->state, OA2DP_SEP_IDLE);

    /* --- headset-initiated SET_CONFIGURATION + ABORT ------------------- */
    {
        oa2dp_codec_caps sbc_cfg;
        oa2dp_codec_select(NULL, &remote[0], &s, &sbc_cfg);
        n = oa2dp_codec_build(&sbc_cfg, ie, sizeof(ie));
        make_caps(&cfg, ie, (size_t)n, false);
        CHECK(oa2dp_avdtp_set_configuration(&hs.av, 1, (uint8_t)pc_sbc, &cfg) >= 0);
        pump(&pc, &hs);
        CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_sbc)->state, OA2DP_SEP_CONFIGURED);
        CHECK(!oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_sbc)->initiator);
        CHECK(last(&pc)->msg_type == OA2DP_AVDTP_MSG_COMMAND &&
              last(&pc)->signal_id == OA2DP_AVDTP_SET_CONFIGURATION);

        CHECK(oa2dp_avdtp_abort(&pc.av, (uint8_t)pc_sbc) >= 0);
        pump(&pc, &hs);
        CHECK_EQ(oa2dp_avdtp_sep_get(&pc.av, (uint8_t)pc_sbc)->state, OA2DP_SEP_IDLE);
        CHECK_EQ(oa2dp_avdtp_sep_get(&hs.av, 1)->state, OA2DP_SEP_IDLE);
    }

    /* --- SET_CONFIGURATION with an unknown service category ------------ */
    {
        uint8_t raw[] = {0x70, OA2DP_AVDTP_SET_CONFIGURATION, (uint8_t)(pc_sbc << 2), 1 << 2,
                         OA2DP_AVDTP_CAT_MEDIA_TRANSPORT, 0, 0x20, 0};
        oa2dp_avdtp_input(&pc.av, raw, sizeof(raw));
        msg *m = &pc.out[(pc.tail - 1) % QMAX];
        CHECK_EQ(m->data[0], 0x70 | OA2DP_AVDTP_MSG_REJECT);
        CHECK_EQ(m->data[2], 0x20);
        CHECK_EQ(m->data[3], OA2DP_AVDTP_BAD_SERV_CATEGORY);
        pc.head = pc.tail;
    }

    /* --- unknown signal: GENERAL_REJECT -------------------------------- */
    {
        uint8_t raw[] = {0x80, 0x3E};
        oa2dp_avdtp_input(&pc.av, raw, sizeof(raw));
        msg *m = &pc.out[(pc.tail - 1) % QMAX];
        CHECK_EQ(m->len, 2);
        CHECK_EQ(m->data[0], 0x80 | OA2DP_AVDTP_MSG_GENERAL_REJECT);
        CHECK_EQ(m->data[1], 0x3E);
        pc.head = pc.tail;
    }

    /* --- unsolicited response is dropped ------------------------------- */
    {
        uint8_t raw[] = {0x92, OA2DP_AVDTP_OPEN};
        CHECK_EQ(oa2dp_avdtp_input(&pc.av, raw, sizeof(raw)), OA2DP_ERR_PROTO);
    }

    /* --- timeout ------------------------------------------------------ */
    int nev = pc.nev;
    int label = oa2dp_avdtp_discover(&pc.av);
    CHECK(label >= 0);
    pc.head = pc.tail; /* lost on air */
    pc.clock += OA2DP_AVDTP_TIMEOUT_MS - 1;
    oa2dp_avdtp_check_timeouts(&pc.av);
    CHECK_EQ(pc.nev, nev);
    pc.clock += 1;
    oa2dp_avdtp_check_timeouts(&pc.av);
    CHECK_EQ(pc.nev, nev + 1);
    CHECK(last(&pc)->timed_out && last(&pc)->label == label);

    /* --- 16 outstanding commands exhaust the labels -------------------- */
    for (int i = 0; i < 16; i++)
        CHECK(oa2dp_avdtp_discover(&pc.av) >= 0);
    CHECK_EQ(oa2dp_avdtp_discover(&pc.av), OA2DP_ERR_BUSY);
    pc.head = pc.tail;
    oa2dp_avdtp_reset(&pc.av);
    CHECK(oa2dp_avdtp_discover(&pc.av) >= 0);
}
