/* SPDX-License-Identifier: MIT */
#include "oa2dp/a2dp_codecs.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static bool single_bit(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Sampling-frequency bit tables, one entry per bit, most significant first. */
typedef struct {
    uint32_t bit;
    uint32_t rate;
} rate_bit;

static const rate_bit sbc_rates[] = {
    {OA2DP_SBC_FREQ_16000, 16000}, {OA2DP_SBC_FREQ_32000, 32000},
    {OA2DP_SBC_FREQ_44100, 44100}, {OA2DP_SBC_FREQ_48000, 48000}};

static const rate_bit aac_rates[] = {
    {OA2DP_AAC_FREQ_8000, 8000},   {OA2DP_AAC_FREQ_11025, 11025},
    {OA2DP_AAC_FREQ_12000, 12000}, {OA2DP_AAC_FREQ_16000, 16000},
    {OA2DP_AAC_FREQ_22050, 22050}, {OA2DP_AAC_FREQ_24000, 24000},
    {OA2DP_AAC_FREQ_32000, 32000}, {OA2DP_AAC_FREQ_44100, 44100},
    {OA2DP_AAC_FREQ_48000, 48000}, {OA2DP_AAC_FREQ_64000, 64000},
    {OA2DP_AAC_FREQ_88200, 88200}, {OA2DP_AAC_FREQ_96000, 96000}};

static const rate_bit aptx_rates[] = {
    {OA2DP_APTX_FREQ_16000, 16000}, {OA2DP_APTX_FREQ_32000, 32000},
    {OA2DP_APTX_FREQ_44100, 44100}, {OA2DP_APTX_FREQ_48000, 48000}};

static const rate_bit fs_rates[] = {
    {OA2DP_FS_SINK_FREQ_44100, 44100}, {OA2DP_FS_SINK_FREQ_48000, 48000}};

static const rate_bit ldac_rates[] = {
    {OA2DP_LDAC_FREQ_44100, 44100},   {OA2DP_LDAC_FREQ_48000, 48000},
    {OA2DP_LDAC_FREQ_88200, 88200},   {OA2DP_LDAC_FREQ_96000, 96000},
    {OA2DP_LDAC_FREQ_176400, 176400}, {OA2DP_LDAC_FREQ_192000, 192000}};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t bit_for_rate(const rate_bit *t, size_t n, uint32_t mask,
                             uint32_t rate)
{
    for (size_t i = 0; i < n; i++)
        if (t[i].rate == rate && (mask & t[i].bit))
            return t[i].bit;
    return 0;
}

static uint32_t rate_for_bit(const rate_bit *t, size_t n, uint32_t bit)
{
    for (size_t i = 0; i < n; i++)
        if (t[i].bit == bit)
            return t[i].rate;
    return 0;
}

/* Choose one rate bit out of `mask`: the user's rate if possible, otherwise
 * 48 kHz, then 44.1 kHz, then the highest rate available. */
static uint32_t pick_rate(const rate_bit *t, size_t n, uint32_t mask,
                          uint32_t wanted)
{
    uint32_t b;
    if (wanted && (b = bit_for_rate(t, n, mask, wanted)))
        return b;
    if ((b = bit_for_rate(t, n, mask, 48000)))
        return b;
    if ((b = bit_for_rate(t, n, mask, 44100)))
        return b;
    uint32_t best = 0, best_rate = 0;
    for (size_t i = 0; i < n; i++)
        if ((mask & t[i].bit) && t[i].rate > best_rate) {
            best = t[i].bit;
            best_rate = t[i].rate;
        }
    return best;
}

/* First bit of `order` that is present in `mask`. */
static uint8_t pick_first(uint8_t mask, const uint8_t *order, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (mask & order[i])
            return order[i];
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Names                                                                  */
/* ---------------------------------------------------------------------- */

const char *oa2dp_codec_name(oa2dp_codec_id id)
{
    switch (id) {
    case OA2DP_CODEC_SBC: return "SBC";
    case OA2DP_CODEC_AAC: return "AAC";
    case OA2DP_CODEC_APTX: return "aptX";
    case OA2DP_CODEC_APTX_HD: return "aptX HD";
    case OA2DP_CODEC_APTX_LL: return "aptX Low Latency";
    case OA2DP_CODEC_FASTSTREAM: return "FastStream";
    case OA2DP_CODEC_LDAC: return "LDAC";
    default: return "unknown";
    }
}

/* ---------------------------------------------------------------------- */
/* Parse / build                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t vendor;
    uint16_t codec_id;
    oa2dp_codec_id id;
    size_t min_info; /* bytes after the 6-byte vendor header */
} vendor_codec;

static const vendor_codec vendor_codecs[] = {
    {OA2DP_VENDOR_APT, OA2DP_APTX_CODEC_ID, OA2DP_CODEC_APTX, 1},
    {OA2DP_VENDOR_QTIL, OA2DP_APTX_HD_CODEC_ID, OA2DP_CODEC_APTX_HD, 5},
    {OA2DP_VENDOR_CSR, OA2DP_APTX_LL_CODEC_ID, OA2DP_CODEC_APTX_LL, 2},
    {OA2DP_VENDOR_CSR, OA2DP_FASTSTREAM_CODEC_ID, OA2DP_CODEC_FASTSTREAM, 2},
    {OA2DP_VENDOR_SONY, OA2DP_LDAC_CODEC_ID, OA2DP_CODEC_LDAC, 2},
};

int oa2dp_codec_parse(const uint8_t *ie, size_t len, oa2dp_codec_caps *out)
{
    if (!ie || !out || len < 2)
        return OA2DP_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->codec = OA2DP_CODEC_UNKNOWN;

    if ((ie[0] >> 4) != OA2DP_MEDIA_TYPE_AUDIO)
        return OA2DP_ERR_UNSUPPORTED;

    const uint8_t *p = ie + 2;
    size_t n = len - 2;

    switch (ie[1]) {
    case OA2DP_CODEC_TYPE_SBC:
        if (n < 4)
            return OA2DP_ERR_INVAL;
        out->codec = OA2DP_CODEC_SBC;
        out->u.sbc.freq = p[0] & 0xF0;
        out->u.sbc.chmode = p[0] & 0x0F;
        out->u.sbc.blocks = p[1] & 0xF0;
        out->u.sbc.subbands = p[1] & 0x0C;
        out->u.sbc.alloc = p[1] & 0x03;
        out->u.sbc.min_bitpool = p[2];
        out->u.sbc.max_bitpool = p[3];
        return OA2DP_OK;

    case OA2DP_CODEC_TYPE_AAC:
        if (n < 6)
            return OA2DP_ERR_INVAL;
        out->codec = OA2DP_CODEC_AAC;
        out->u.aac.object_type = p[0];
        out->u.aac.freq = (uint16_t)((p[1] << 4) | (p[2] >> 4));
        out->u.aac.channels = p[2] & 0x0C;
        out->u.aac.vbr = (p[3] & 0x80) != 0;
        out->u.aac.bitrate =
            ((uint32_t)(p[3] & 0x7F) << 16) | ((uint32_t)p[4] << 8) | p[5];
        return OA2DP_OK;

    case OA2DP_CODEC_TYPE_VENDOR: {
        if (n < 6)
            return OA2DP_ERR_INVAL;
        uint32_t vendor = rd_le32(p);
        uint16_t codec_id = rd_le16(p + 4);
        const uint8_t *v = p + 6;
        size_t vn = n - 6;
        for (size_t i = 0; i < COUNT(vendor_codecs); i++) {
            const vendor_codec *vc = &vendor_codecs[i];
            if (vc->vendor != vendor || vc->codec_id != codec_id)
                continue;
            if (vn < vc->min_info)
                return OA2DP_ERR_INVAL;
            out->codec = vc->id;
            switch (vc->id) {
            case OA2DP_CODEC_APTX:
            case OA2DP_CODEC_APTX_HD:
                out->u.aptx.freq = v[0] & 0xF0;
                out->u.aptx.chmode = v[0] & 0x0F;
                break;
            case OA2DP_CODEC_APTX_LL:
                out->u.aptx_ll.freq = v[0] & 0xF0;
                out->u.aptx_ll.chmode = v[0] & 0x0F;
                out->u.aptx_ll.bidirect = (v[1] & 0x01) != 0;
                out->u.aptx_ll.has_new_caps = (v[1] & 0x02) != 0;
                if (out->u.aptx_ll.has_new_caps) {
                    if (vn < 11)
                        return OA2DP_ERR_INVAL;
                    /* v[2] reserved */
                    out->u.aptx_ll.target_level = rd_le16(v + 3);
                    out->u.aptx_ll.initial_level = rd_le16(v + 5);
                    out->u.aptx_ll.sra_max_rate = v[7];
                    out->u.aptx_ll.sra_avg_time = v[8];
                    out->u.aptx_ll.good_working_level = rd_le16(v + 9);
                }
                break;
            case OA2DP_CODEC_FASTSTREAM:
                out->u.fs.direction = v[0];
                out->u.fs.sink_freq = v[1] & 0x0F;
                out->u.fs.source_freq = v[1] >> 4;
                break;
            case OA2DP_CODEC_LDAC:
                out->u.ldac.freq = v[0] & 0x3F;
                out->u.ldac.chmode = v[1] & 0x07;
                break;
            default:
                break;
            }
            return OA2DP_OK;
        }
        return OA2DP_ERR_UNSUPPORTED;
    }

    default:
        return OA2DP_ERR_UNSUPPORTED;
    }
}

static int vendor_header(uint8_t *buf, size_t cap, size_t total,
                         uint32_t vendor, uint16_t codec_id)
{
    if (cap < total)
        return OA2DP_ERR_NOSPC;
    memset(buf, 0, total);
    buf[0] = OA2DP_MEDIA_TYPE_AUDIO << 4;
    buf[1] = OA2DP_CODEC_TYPE_VENDOR;
    wr_le32(buf + 2, vendor);
    wr_le16(buf + 6, codec_id);
    return OA2DP_OK;
}

int oa2dp_codec_build(const oa2dp_codec_caps *c, uint8_t *buf, size_t cap)
{
    if (!c || !buf)
        return OA2DP_ERR_INVAL;

    switch (c->codec) {
    case OA2DP_CODEC_SBC:
        if (cap < 6)
            return OA2DP_ERR_NOSPC;
        buf[0] = OA2DP_MEDIA_TYPE_AUDIO << 4;
        buf[1] = OA2DP_CODEC_TYPE_SBC;
        buf[2] = (uint8_t)((c->u.sbc.freq & 0xF0) | (c->u.sbc.chmode & 0x0F));
        buf[3] = (uint8_t)((c->u.sbc.blocks & 0xF0) | (c->u.sbc.subbands & 0x0C) |
                           (c->u.sbc.alloc & 0x03));
        buf[4] = c->u.sbc.min_bitpool;
        buf[5] = c->u.sbc.max_bitpool;
        return 6;

    case OA2DP_CODEC_AAC: {
        if (cap < 8)
            return OA2DP_ERR_NOSPC;
        uint32_t br = c->u.aac.bitrate & 0x7FFFFF;
        buf[0] = OA2DP_MEDIA_TYPE_AUDIO << 4;
        buf[1] = OA2DP_CODEC_TYPE_AAC;
        buf[2] = c->u.aac.object_type;
        buf[3] = (uint8_t)(c->u.aac.freq >> 4);
        buf[4] = (uint8_t)(((c->u.aac.freq & 0x0F) << 4) | (c->u.aac.channels & 0x0C));
        buf[5] = (uint8_t)((c->u.aac.vbr ? 0x80 : 0) | (br >> 16));
        buf[6] = (uint8_t)(br >> 8);
        buf[7] = (uint8_t)br;
        return 8;
    }

    case OA2DP_CODEC_APTX:
        if (vendor_header(buf, cap, 9, OA2DP_VENDOR_APT, OA2DP_APTX_CODEC_ID))
            return OA2DP_ERR_NOSPC;
        buf[8] = (uint8_t)((c->u.aptx.freq & 0xF0) | (c->u.aptx.chmode & 0x0F));
        return 9;

    case OA2DP_CODEC_APTX_HD:
        if (vendor_header(buf, cap, 13, OA2DP_VENDOR_QTIL, OA2DP_APTX_HD_CODEC_ID))
            return OA2DP_ERR_NOSPC;
        buf[8] = (uint8_t)((c->u.aptx.freq & 0xF0) | (c->u.aptx.chmode & 0x0F));
        return 13; /* 4 reserved bytes stay zero */

    case OA2DP_CODEC_APTX_LL: {
        const oa2dp_aptx_ll_caps *ll = &c->u.aptx_ll;
        size_t total = ll->has_new_caps ? 19 : 10;
        if (vendor_header(buf, cap, total, OA2DP_VENDOR_CSR, OA2DP_APTX_LL_CODEC_ID))
            return OA2DP_ERR_NOSPC;
        buf[8] = (uint8_t)((ll->freq & 0xF0) | (ll->chmode & 0x0F));
        buf[9] = (uint8_t)((ll->bidirect ? 0x01 : 0) | (ll->has_new_caps ? 0x02 : 0));
        if (ll->has_new_caps) {
            wr_le16(buf + 11, ll->target_level);
            wr_le16(buf + 13, ll->initial_level);
            buf[15] = ll->sra_max_rate;
            buf[16] = ll->sra_avg_time;
            wr_le16(buf + 17, ll->good_working_level);
        }
        return (int)total;
    }

    case OA2DP_CODEC_FASTSTREAM:
        if (vendor_header(buf, cap, 10, OA2DP_VENDOR_CSR, OA2DP_FASTSTREAM_CODEC_ID))
            return OA2DP_ERR_NOSPC;
        buf[8] = c->u.fs.direction;
        buf[9] = (uint8_t)((c->u.fs.sink_freq & 0x0F) | (c->u.fs.source_freq << 4));
        return 10;

    case OA2DP_CODEC_LDAC:
        if (vendor_header(buf, cap, 10, OA2DP_VENDOR_SONY, OA2DP_LDAC_CODEC_ID))
            return OA2DP_ERR_NOSPC;
        buf[8] = c->u.ldac.freq & 0x3F;
        buf[9] = c->u.ldac.chmode & 0x07;
        return 10;

    default:
        return OA2DP_ERR_UNSUPPORTED;
    }
}

/* ---------------------------------------------------------------------- */
/* Local capabilities (what the bundled encoder adapters accept)          */
/* ---------------------------------------------------------------------- */

void oa2dp_codec_local_caps(oa2dp_codec_id id, oa2dp_codec_caps *out)
{
    memset(out, 0, sizeof(*out));
    out->codec = id;
    switch (id) {
    case OA2DP_CODEC_SBC:
        out->u.sbc.freq = OA2DP_SBC_FREQ_16000 | OA2DP_SBC_FREQ_32000 |
                          OA2DP_SBC_FREQ_44100 | OA2DP_SBC_FREQ_48000;
        out->u.sbc.chmode = OA2DP_SBC_CH_MONO | OA2DP_SBC_CH_DUAL |
                            OA2DP_SBC_CH_STEREO | OA2DP_SBC_CH_JOINT;
        out->u.sbc.blocks = OA2DP_SBC_BLOCKS_4 | OA2DP_SBC_BLOCKS_8 |
                            OA2DP_SBC_BLOCKS_12 | OA2DP_SBC_BLOCKS_16;
        out->u.sbc.subbands = OA2DP_SBC_SUBBANDS_4 | OA2DP_SBC_SUBBANDS_8;
        out->u.sbc.alloc = OA2DP_SBC_ALLOC_SNR_BIT | OA2DP_SBC_ALLOC_LOUDNESS_BIT;
        out->u.sbc.min_bitpool = OA2DP_SBC_MIN_BITPOOL;
        out->u.sbc.max_bitpool = OA2DP_SBC_MAX_BITPOOL;
        break;
    case OA2DP_CODEC_AAC:
        /* Media Foundation AAC encoder: AAC-LC, 44.1/48 kHz, CBR up to
         * 192 kbit/s. Replace when another AAC encoder is plugged in. */
        out->u.aac.object_type = OA2DP_AAC_OBJ_MPEG2_LC | OA2DP_AAC_OBJ_MPEG4_LC;
        out->u.aac.freq = OA2DP_AAC_FREQ_44100 | OA2DP_AAC_FREQ_48000;
        out->u.aac.channels = OA2DP_AAC_CH_1 | OA2DP_AAC_CH_2;
        out->u.aac.vbr = false;
        out->u.aac.bitrate = 192000;
        break;
    case OA2DP_CODEC_APTX:
        out->u.aptx.freq = OA2DP_APTX_FREQ_16000 | OA2DP_APTX_FREQ_32000 |
                           OA2DP_APTX_FREQ_44100 | OA2DP_APTX_FREQ_48000;
        out->u.aptx.chmode = OA2DP_APTX_CH_STEREO;
        break;
    case OA2DP_CODEC_APTX_HD:
        out->u.aptx.freq = OA2DP_APTX_FREQ_44100 | OA2DP_APTX_FREQ_48000;
        out->u.aptx.chmode = OA2DP_APTX_CH_STEREO;
        break;
    case OA2DP_CODEC_APTX_LL:
        out->u.aptx_ll.freq = OA2DP_APTX_FREQ_44100 | OA2DP_APTX_FREQ_48000;
        out->u.aptx_ll.chmode = OA2DP_APTX_CH_STEREO;
        break;
    case OA2DP_CODEC_FASTSTREAM:
        out->u.fs.direction = OA2DP_FS_DIR_SINK;
        out->u.fs.sink_freq = OA2DP_FS_SINK_FREQ_44100 | OA2DP_FS_SINK_FREQ_48000;
        out->u.fs.source_freq = 0;
        break;
    case OA2DP_CODEC_LDAC:
        out->u.ldac.freq = OA2DP_LDAC_FREQ_44100 | OA2DP_LDAC_FREQ_48000 |
                           OA2DP_LDAC_FREQ_88200 | OA2DP_LDAC_FREQ_96000;
        out->u.ldac.chmode = OA2DP_LDAC_CH_MONO | OA2DP_LDAC_CH_DUAL |
                             OA2DP_LDAC_CH_STEREO;
        break;
    default:
        out->codec = OA2DP_CODEC_UNKNOWN;
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Selection                                                              */
/* ---------------------------------------------------------------------- */

uint8_t oa2dp_sbc_max_bitpool(uint8_t chmode, uint8_t subbands)
{
    unsigned sb = subbands == OA2DP_SBC_SUBBANDS_4 ? 4 : 8;
    unsigned max = (chmode == OA2DP_SBC_CH_MONO || chmode == OA2DP_SBC_CH_DUAL)
                       ? 16 * sb
                       : 32 * sb;
    return (uint8_t)(max > OA2DP_SBC_MAX_BITPOOL ? OA2DP_SBC_MAX_BITPOOL : max);
}

/* A2DP "high quality" recommendations; dual channel gets a per-channel
 * bitpool that yields roughly 450 kbit/s ("SBC XQ" style). */
uint8_t oa2dp_sbc_auto_bitpool(uint8_t chmode, uint8_t freq)
{
    bool f48 = freq == OA2DP_SBC_FREQ_48000;
    switch (chmode) {
    case OA2DP_SBC_CH_MONO: return f48 ? 29 : 31;
    case OA2DP_SBC_CH_DUAL: return f48 ? 35 : 38;
    default: return f48 ? 51 : 53;
    }
}

static int select_sbc(const oa2dp_sbc_caps *l, const oa2dp_sbc_caps *r,
                      const oa2dp_settings *s, oa2dp_sbc_caps *o)
{
    uint8_t freq = l->freq & r->freq, ch = l->chmode & r->chmode;
    uint8_t blk = l->blocks & r->blocks, sb = l->subbands & r->subbands;
    uint8_t al = l->alloc & r->alloc;
    if (!freq || !ch || !blk || !sb || !al)
        return OA2DP_ERR_UNSUPPORTED;

    o->freq = (uint8_t)pick_rate(sbc_rates, COUNT(sbc_rates), freq, s->sample_rate);

    static const uint8_t auto_ch[] = {OA2DP_SBC_CH_JOINT, OA2DP_SBC_CH_STEREO,
                                      OA2DP_SBC_CH_DUAL, OA2DP_SBC_CH_MONO};
    uint8_t want = 0;
    switch (s->chmode) {
    case OA2DP_CHMODE_MONO: want = OA2DP_SBC_CH_MONO; break;
    case OA2DP_CHMODE_DUAL: want = OA2DP_SBC_CH_DUAL; break;
    case OA2DP_CHMODE_STEREO: want = OA2DP_SBC_CH_STEREO; break;
    case OA2DP_CHMODE_JOINT: want = OA2DP_SBC_CH_JOINT; break;
    default: break;
    }
    o->chmode = (want & ch) ? want : pick_first(ch, auto_ch, COUNT(auto_ch));

    static const uint8_t auto_blk[] = {OA2DP_SBC_BLOCKS_16, OA2DP_SBC_BLOCKS_12,
                                       OA2DP_SBC_BLOCKS_8, OA2DP_SBC_BLOCKS_4};
    uint8_t want_blk = s->sbc_blocks == 4    ? OA2DP_SBC_BLOCKS_4
                       : s->sbc_blocks == 8  ? OA2DP_SBC_BLOCKS_8
                       : s->sbc_blocks == 12 ? OA2DP_SBC_BLOCKS_12
                       : s->sbc_blocks == 16 ? OA2DP_SBC_BLOCKS_16
                                             : 0;
    o->blocks = (want_blk & blk) ? want_blk : pick_first(blk, auto_blk, COUNT(auto_blk));

    static const uint8_t auto_sb[] = {OA2DP_SBC_SUBBANDS_8, OA2DP_SBC_SUBBANDS_4};
    uint8_t want_sb = s->sbc_subbands == 4   ? OA2DP_SBC_SUBBANDS_4
                      : s->sbc_subbands == 8 ? OA2DP_SBC_SUBBANDS_8
                                             : 0;
    o->subbands = (want_sb & sb) ? want_sb : pick_first(sb, auto_sb, COUNT(auto_sb));

    static const uint8_t auto_al[] = {OA2DP_SBC_ALLOC_LOUDNESS_BIT, OA2DP_SBC_ALLOC_SNR_BIT};
    uint8_t want_al = s->sbc_alloc == OA2DP_SBC_ALLOC_LOUDNESS ? OA2DP_SBC_ALLOC_LOUDNESS_BIT
                      : s->sbc_alloc == OA2DP_SBC_ALLOC_SNR    ? OA2DP_SBC_ALLOC_SNR_BIT
                                                               : 0;
    o->alloc = (want_al & al) ? want_al : pick_first(al, auto_al, COUNT(auto_al));

    uint8_t lo = l->min_bitpool > r->min_bitpool ? l->min_bitpool : r->min_bitpool;
    uint8_t hi = l->max_bitpool < r->max_bitpool ? l->max_bitpool : r->max_bitpool;
    uint8_t mode_max = oa2dp_sbc_max_bitpool(o->chmode, o->subbands);
    if (lo < OA2DP_SBC_MIN_BITPOOL)
        lo = OA2DP_SBC_MIN_BITPOOL;
    if (hi > mode_max)
        hi = mode_max;
    if (lo > hi)
        return OA2DP_ERR_UNSUPPORTED;

    uint8_t bp = s->sbc_bitpool ? s->sbc_bitpool : oa2dp_sbc_auto_bitpool(o->chmode, o->freq);
    if (bp > hi)
        bp = hi;
    if (bp < lo)
        bp = lo;
    o->min_bitpool = lo;
    o->max_bitpool = bp;
    return OA2DP_OK;
}

static int select_aac(const oa2dp_aac_caps *l, const oa2dp_aac_caps *r,
                      const oa2dp_settings *s, oa2dp_aac_caps *o)
{
    uint8_t obj = l->object_type & r->object_type;
    uint16_t freq = l->freq & r->freq;
    uint8_t ch = l->channels & r->channels;
    if (!obj || !freq || !ch)
        return OA2DP_ERR_UNSUPPORTED;

    static const uint8_t obj_order[] = {OA2DP_AAC_OBJ_MPEG2_LC, OA2DP_AAC_OBJ_MPEG4_LC,
                                        OA2DP_AAC_OBJ_MPEG4_LTP, OA2DP_AAC_OBJ_MPEG4_SCA};
    o->object_type = pick_first(obj, obj_order, COUNT(obj_order));
    if (!o->object_type)
        return OA2DP_ERR_UNSUPPORTED;
    o->freq = (uint16_t)pick_rate(aac_rates, COUNT(aac_rates), freq, s->sample_rate);

    if (s->chmode == OA2DP_CHMODE_MONO && (ch & OA2DP_AAC_CH_1))
        o->channels = OA2DP_AAC_CH_1;
    else
        o->channels = (ch & OA2DP_AAC_CH_2) ? OA2DP_AAC_CH_2 : OA2DP_AAC_CH_1;

    o->vbr = s->aac_vbr && l->vbr && r->vbr;

    uint32_t br = l->bitrate;
    if (r->bitrate && r->bitrate < br)
        br = r->bitrate;
    if (s->aac_bitrate && s->aac_bitrate < br)
        br = s->aac_bitrate;
    o->bitrate = br;
    return OA2DP_OK;
}

static uint8_t pick_aptx_ch(uint8_t ch, const oa2dp_settings *s)
{
    if (s->chmode == OA2DP_CHMODE_MONO && (ch & OA2DP_APTX_CH_MONO))
        return OA2DP_APTX_CH_MONO;
    return (ch & OA2DP_APTX_CH_STEREO) ? OA2DP_APTX_CH_STEREO
                                       : (uint8_t)(ch & OA2DP_APTX_CH_MONO);
}

int oa2dp_codec_select(const oa2dp_codec_caps *local,
                       const oa2dp_codec_caps *remote,
                       const oa2dp_settings *s, oa2dp_codec_caps *config)
{
    oa2dp_codec_caps own;
    if (!remote || !s || !config)
        return OA2DP_ERR_INVAL;
    if (!local) {
        oa2dp_codec_local_caps(remote->codec, &own);
        local = &own;
    }
    if (local->codec != remote->codec || remote->codec == OA2DP_CODEC_UNKNOWN)
        return OA2DP_ERR_UNSUPPORTED;

    memset(config, 0, sizeof(*config));
    config->codec = remote->codec;

    switch (remote->codec) {
    case OA2DP_CODEC_SBC:
        return select_sbc(&local->u.sbc, &remote->u.sbc, s, &config->u.sbc);
    case OA2DP_CODEC_AAC:
        return select_aac(&local->u.aac, &remote->u.aac, s, &config->u.aac);
    case OA2DP_CODEC_APTX:
    case OA2DP_CODEC_APTX_HD: {
        uint8_t f = local->u.aptx.freq & remote->u.aptx.freq;
        uint8_t ch = local->u.aptx.chmode & remote->u.aptx.chmode;
        if (!f || !ch)
            return OA2DP_ERR_UNSUPPORTED;
        config->u.aptx.freq = (uint8_t)pick_rate(aptx_rates, COUNT(aptx_rates), f, s->sample_rate);
        config->u.aptx.chmode = pick_aptx_ch(ch, s);
        return OA2DP_OK;
    }
    case OA2DP_CODEC_APTX_LL: {
        uint8_t f = local->u.aptx_ll.freq & remote->u.aptx_ll.freq;
        uint8_t ch = local->u.aptx_ll.chmode & remote->u.aptx_ll.chmode;
        if (!f || !ch)
            return OA2DP_ERR_UNSUPPORTED;
        config->u.aptx_ll.freq = (uint8_t)pick_rate(aptx_rates, COUNT(aptx_rates), f, s->sample_rate);
        config->u.aptx_ll.chmode = pick_aptx_ch(ch, s);
        config->u.aptx_ll.bidirect = local->u.aptx_ll.bidirect && remote->u.aptx_ll.bidirect;
        if (remote->u.aptx_ll.has_new_caps) {
            /* Echo the sink's preferred buffer levels back unchanged. */
            config->u.aptx_ll.has_new_caps = true;
            config->u.aptx_ll.target_level = remote->u.aptx_ll.target_level;
            config->u.aptx_ll.initial_level = remote->u.aptx_ll.initial_level;
            config->u.aptx_ll.sra_max_rate = remote->u.aptx_ll.sra_max_rate;
            config->u.aptx_ll.sra_avg_time = remote->u.aptx_ll.sra_avg_time;
            config->u.aptx_ll.good_working_level = remote->u.aptx_ll.good_working_level;
        }
        return OA2DP_OK;
    }
    case OA2DP_CODEC_FASTSTREAM: {
        if (!(remote->u.fs.direction & OA2DP_FS_DIR_SINK))
            return OA2DP_ERR_UNSUPPORTED;
        uint8_t f = local->u.fs.sink_freq & remote->u.fs.sink_freq;
        if (!f)
            return OA2DP_ERR_UNSUPPORTED;
        config->u.fs.direction = OA2DP_FS_DIR_SINK;
        config->u.fs.sink_freq = (uint8_t)pick_rate(fs_rates, COUNT(fs_rates), f, s->sample_rate);
        return OA2DP_OK;
    }
    case OA2DP_CODEC_LDAC: {
        uint8_t f = local->u.ldac.freq & remote->u.ldac.freq;
        uint8_t ch = local->u.ldac.chmode & remote->u.ldac.chmode;
        if (!f || !ch)
            return OA2DP_ERR_UNSUPPORTED;
        config->u.ldac.freq = (uint8_t)pick_rate(ldac_rates, COUNT(ldac_rates), f, s->sample_rate);
        static const uint8_t order[] = {OA2DP_LDAC_CH_STEREO, OA2DP_LDAC_CH_DUAL,
                                        OA2DP_LDAC_CH_MONO};
        uint8_t want = s->chmode == OA2DP_CHMODE_MONO   ? OA2DP_LDAC_CH_MONO
                       : s->chmode == OA2DP_CHMODE_DUAL ? OA2DP_LDAC_CH_DUAL
                                                        : 0;
        config->u.ldac.chmode = (want & ch) ? want : pick_first(ch, order, COUNT(order));
        return OA2DP_OK;
    }
    default:
        return OA2DP_ERR_UNSUPPORTED;
    }
}

int oa2dp_codec_choose_best(const oa2dp_codec_caps *remote, size_t count,
                            const oa2dp_settings *s, oa2dp_codec_caps *config)
{
    if (!remote || !s || !config)
        return OA2DP_ERR_INVAL;
    for (uint8_t p = 0; p < s->priority_count; p++)
        for (size_t i = 0; i < count; i++)
            if (remote[i].codec == s->priority[p] &&
                oa2dp_codec_select(NULL, &remote[i], s, config) == OA2DP_OK)
                return (int)i;
    return OA2DP_ERR_NOTFOUND;
}

/* ---------------------------------------------------------------------- */
/* Validation of a peer-chosen configuration                              */
/* ---------------------------------------------------------------------- */

static uint8_t check_field(uint32_t cfg, uint32_t local, uint8_t invalid,
                           uint8_t unsupported)
{
    if (!single_bit(cfg))
        return invalid;
    if (!(cfg & local))
        return unsupported;
    return 0;
}

uint8_t oa2dp_codec_check_config(const oa2dp_codec_caps *local,
                                 const oa2dp_codec_caps *c)
{
    uint8_t e;
    if (!local || !c)
        return OA2DP_A2DP_INVALID_CODEC_TYPE;
    if (c->codec != local->codec || c->codec == OA2DP_CODEC_UNKNOWN)
        return OA2DP_A2DP_NOT_SUPPORTED_CODEC_TYPE;

    switch (c->codec) {
    case OA2DP_CODEC_SBC: {
        const oa2dp_sbc_caps *l = &local->u.sbc, *x = &c->u.sbc;
        if ((e = check_field(x->freq, l->freq, OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY,
                             OA2DP_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY)))
            return e;
        if ((e = check_field(x->chmode, l->chmode, OA2DP_A2DP_INVALID_CHANNEL_MODE,
                             OA2DP_A2DP_NOT_SUPPORTED_CHANNEL_MODE)))
            return e;
        if ((e = check_field(x->blocks, l->blocks, OA2DP_A2DP_INVALID_BLOCK_LENGTH,
                             OA2DP_A2DP_INVALID_BLOCK_LENGTH)))
            return e;
        if ((e = check_field(x->subbands, l->subbands, OA2DP_A2DP_INVALID_SUBBANDS,
                             OA2DP_A2DP_NOT_SUPPORTED_SUBBANDS)))
            return e;
        if ((e = check_field(x->alloc, l->alloc, OA2DP_A2DP_INVALID_ALLOCATION_METHOD,
                             OA2DP_A2DP_NOT_SUPPORTED_ALLOCATION_METHOD)))
            return e;
        if (x->min_bitpool < OA2DP_SBC_MIN_BITPOOL || x->min_bitpool > OA2DP_SBC_MAX_BITPOOL)
            return OA2DP_A2DP_INVALID_MINIMUM_BITPOOL_VALUE;
        if (x->max_bitpool < x->min_bitpool || x->max_bitpool > OA2DP_SBC_MAX_BITPOOL)
            return OA2DP_A2DP_INVALID_MAXIMUM_BITPOOL_VALUE;
        if (x->min_bitpool < l->min_bitpool)
            return OA2DP_A2DP_NOT_SUPPORTED_MINIMUM_BITPOOL_VALUE;
        if (x->max_bitpool > l->max_bitpool ||
            x->max_bitpool > oa2dp_sbc_max_bitpool(x->chmode, x->subbands))
            return OA2DP_A2DP_NOT_SUPPORTED_MAXIMUM_BITPOOL_VALUE;
        return 0;
    }
    case OA2DP_CODEC_AAC: {
        const oa2dp_aac_caps *l = &local->u.aac, *x = &c->u.aac;
        if ((e = check_field(x->object_type, l->object_type, OA2DP_A2DP_INVALID_OBJECT_TYPE,
                             OA2DP_A2DP_NOT_SUPPORTED_OBJECT_TYPE)))
            return e;
        if ((e = check_field(x->freq, l->freq, OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY,
                             OA2DP_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY)))
            return e;
        if ((e = check_field(x->channels, l->channels, OA2DP_A2DP_INVALID_CHANNELS,
                             OA2DP_A2DP_NOT_SUPPORTED_CHANNELS)))
            return e;
        if (x->vbr && !l->vbr)
            return OA2DP_A2DP_NOT_SUPPORTED_VBR;
        return 0;
    }
    case OA2DP_CODEC_APTX:
    case OA2DP_CODEC_APTX_HD:
        if ((e = check_field(c->u.aptx.freq, local->u.aptx.freq,
                             OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY,
                             OA2DP_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY)))
            return e;
        return check_field(c->u.aptx.chmode, local->u.aptx.chmode,
                           OA2DP_A2DP_INVALID_CHANNEL_MODE,
                           OA2DP_A2DP_NOT_SUPPORTED_CHANNEL_MODE);
    case OA2DP_CODEC_APTX_LL:
        if ((e = check_field(c->u.aptx_ll.freq, local->u.aptx_ll.freq,
                             OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY,
                             OA2DP_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY)))
            return e;
        if ((e = check_field(c->u.aptx_ll.chmode, local->u.aptx_ll.chmode,
                             OA2DP_A2DP_INVALID_CHANNEL_MODE,
                             OA2DP_A2DP_NOT_SUPPORTED_CHANNEL_MODE)))
            return e;
        if (c->u.aptx_ll.bidirect && !local->u.aptx_ll.bidirect)
            return OA2DP_A2DP_NOT_SUPPORTED_CODEC_PARAMETER;
        return 0;
    case OA2DP_CODEC_FASTSTREAM:
        if (c->u.fs.direction & ~local->u.fs.direction)
            return OA2DP_A2DP_NOT_SUPPORTED_CODEC_PARAMETER;
        return check_field(c->u.fs.sink_freq, local->u.fs.sink_freq,
                           OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY,
                           OA2DP_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY);
    case OA2DP_CODEC_LDAC:
        if ((e = check_field(c->u.ldac.freq, local->u.ldac.freq,
                             OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY,
                             OA2DP_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY)))
            return e;
        return check_field(c->u.ldac.chmode, local->u.ldac.chmode,
                           OA2DP_A2DP_INVALID_CHANNEL_MODE,
                           OA2DP_A2DP_NOT_SUPPORTED_CHANNEL_MODE);
    default:
        return OA2DP_A2DP_NOT_SUPPORTED_CODEC_TYPE;
    }
}

/* ---------------------------------------------------------------------- */
/* Properties                                                             */
/* ---------------------------------------------------------------------- */

uint32_t oa2dp_codec_sample_rate(const oa2dp_codec_caps *c)
{
    switch (c->codec) {
    case OA2DP_CODEC_SBC: return rate_for_bit(sbc_rates, COUNT(sbc_rates), c->u.sbc.freq);
    case OA2DP_CODEC_AAC: return rate_for_bit(aac_rates, COUNT(aac_rates), c->u.aac.freq);
    case OA2DP_CODEC_APTX:
    case OA2DP_CODEC_APTX_HD: return rate_for_bit(aptx_rates, COUNT(aptx_rates), c->u.aptx.freq);
    case OA2DP_CODEC_APTX_LL: return rate_for_bit(aptx_rates, COUNT(aptx_rates), c->u.aptx_ll.freq);
    case OA2DP_CODEC_FASTSTREAM: return rate_for_bit(fs_rates, COUNT(fs_rates), c->u.fs.sink_freq);
    case OA2DP_CODEC_LDAC: return rate_for_bit(ldac_rates, COUNT(ldac_rates), c->u.ldac.freq);
    default: return 0;
    }
}

uint8_t oa2dp_codec_channels(const oa2dp_codec_caps *c)
{
    switch (c->codec) {
    case OA2DP_CODEC_SBC: return c->u.sbc.chmode == OA2DP_SBC_CH_MONO ? 1 : 2;
    case OA2DP_CODEC_AAC: return c->u.aac.channels == OA2DP_AAC_CH_1 ? 1 : 2;
    case OA2DP_CODEC_APTX:
    case OA2DP_CODEC_APTX_HD: return c->u.aptx.chmode == OA2DP_APTX_CH_MONO ? 1 : 2;
    case OA2DP_CODEC_APTX_LL: return c->u.aptx_ll.chmode == OA2DP_APTX_CH_MONO ? 1 : 2;
    case OA2DP_CODEC_FASTSTREAM: return 2;
    case OA2DP_CODEC_LDAC: return c->u.ldac.chmode == OA2DP_LDAC_CH_MONO ? 1 : 2;
    default: return 0;
    }
}

uint8_t oa2dp_codec_pcm_bits(const oa2dp_codec_caps *c, const oa2dp_settings *s)
{
    switch (c->codec) {
    case OA2DP_CODEC_APTX_HD: return 24;
    case OA2DP_CODEC_LDAC: return (s && s->bits == 16) ? 16 : 24;
    default: return 16;
    }
}

int oa2dp_codec_set_rate(oa2dp_codec_caps *c, const oa2dp_codec_caps *remote,
                         uint32_t rate)
{
    oa2dp_codec_caps local;
    uint32_t bit;
    if (!c || !remote || c->codec != remote->codec)
        return OA2DP_ERR_INVAL;
    oa2dp_codec_local_caps(c->codec, &local);

    switch (c->codec) {
    case OA2DP_CODEC_SBC:
        bit = bit_for_rate(sbc_rates, COUNT(sbc_rates), local.u.sbc.freq & remote->u.sbc.freq, rate);
        if (bit)
            c->u.sbc.freq = (uint8_t)bit;
        break;
    case OA2DP_CODEC_AAC:
        bit = bit_for_rate(aac_rates, COUNT(aac_rates), local.u.aac.freq & remote->u.aac.freq, rate);
        if (bit)
            c->u.aac.freq = (uint16_t)bit;
        break;
    case OA2DP_CODEC_APTX:
    case OA2DP_CODEC_APTX_HD:
        bit = bit_for_rate(aptx_rates, COUNT(aptx_rates), local.u.aptx.freq & remote->u.aptx.freq, rate);
        if (bit)
            c->u.aptx.freq = (uint8_t)bit;
        break;
    case OA2DP_CODEC_APTX_LL:
        bit = bit_for_rate(aptx_rates, COUNT(aptx_rates), local.u.aptx_ll.freq & remote->u.aptx_ll.freq, rate);
        if (bit)
            c->u.aptx_ll.freq = (uint8_t)bit;
        break;
    case OA2DP_CODEC_FASTSTREAM:
        bit = bit_for_rate(fs_rates, COUNT(fs_rates), local.u.fs.sink_freq & remote->u.fs.sink_freq, rate);
        if (bit)
            c->u.fs.sink_freq = (uint8_t)bit;
        break;
    case OA2DP_CODEC_LDAC:
        bit = bit_for_rate(ldac_rates, COUNT(ldac_rates), local.u.ldac.freq & remote->u.ldac.freq, rate);
        if (bit)
            c->u.ldac.freq = (uint8_t)bit;
        break;
    default:
        return OA2DP_ERR_INVAL;
    }
    return bit ? OA2DP_OK : OA2DP_ERR_UNSUPPORTED;
}
