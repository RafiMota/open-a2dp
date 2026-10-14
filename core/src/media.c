/* SPDX-License-Identifier: MIT */
#include "oa2dp/media.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* SBC                                                                    */
/* ---------------------------------------------------------------------- */

int oa2dp_sbc_layout_from_config(const oa2dp_sbc_caps *c, oa2dp_sbc_layout *l)
{
    switch (c->subbands) {
    case OA2DP_SBC_SUBBANDS_4: l->subbands = 4; break;
    case OA2DP_SBC_SUBBANDS_8: l->subbands = 8; break;
    default: return OA2DP_ERR_INVAL;
    }
    switch (c->blocks) {
    case OA2DP_SBC_BLOCKS_4: l->blocks = 4; break;
    case OA2DP_SBC_BLOCKS_8: l->blocks = 8; break;
    case OA2DP_SBC_BLOCKS_12: l->blocks = 12; break;
    case OA2DP_SBC_BLOCKS_16: l->blocks = 16; break;
    default: return OA2DP_ERR_INVAL;
    }
    switch (c->chmode) {
    case OA2DP_SBC_CH_MONO: l->channels = 1; break;
    case OA2DP_SBC_CH_DUAL:
    case OA2DP_SBC_CH_STEREO:
    case OA2DP_SBC_CH_JOINT: l->channels = 2; break;
    default: return OA2DP_ERR_INVAL;
    }
    l->chmode = c->chmode;
    return OA2DP_OK;
}

unsigned oa2dp_sbc_frame_len(const oa2dp_sbc_layout *l, unsigned bitpool)
{
    unsigned len = 4 + (4 * l->subbands * l->channels) / 8;
    switch (l->chmode) {
    case OA2DP_SBC_CH_MONO:
    case OA2DP_SBC_CH_DUAL:
        len += (l->blocks * l->channels * bitpool + 7) / 8;
        break;
    case OA2DP_SBC_CH_STEREO:
        len += (l->blocks * bitpool + 7) / 8;
        break;
    default: /* joint stereo: one join bit per subband */
        len += (l->subbands + l->blocks * bitpool + 7) / 8;
        break;
    }
    return len;
}

uint32_t oa2dp_sbc_bitrate(const oa2dp_sbc_layout *l, unsigned bitpool, uint32_t rate)
{
    uint64_t bits = 8ull * oa2dp_sbc_frame_len(l, bitpool) * rate;
    return (uint32_t)(bits / (l->subbands * l->blocks));
}

uint8_t oa2dp_sbc_bitpool_for_bitrate(const oa2dp_sbc_layout *l, uint32_t rate,
                                      uint32_t target, uint8_t min, uint8_t max)
{
    uint8_t best = min;
    for (unsigned bp = min; bp <= max; bp++) {
        if (oa2dp_sbc_bitrate(l, bp, rate) > target)
            break;
        best = (uint8_t)bp;
    }
    return best;
}

uint8_t oa2dp_sbc_abr_bitpool(uint8_t min, uint8_t max, uint8_t level, uint8_t levels)
{
    unsigned floor = max / 2;
    if (floor < min)
        floor = min;
    if (levels <= 1 || level == 0 || max <= floor)
        return max;
    if (level >= levels - 1)
        return (uint8_t)floor;
    unsigned span = max - floor;
    return (uint8_t)(max - (span * level + (levels - 1) / 2) / (levels - 1));
}

/* ---------------------------------------------------------------------- */
/* RTP                                                                    */
/* ---------------------------------------------------------------------- */

static bool has_rtp(oa2dp_codec_id c)
{
    return c == OA2DP_CODEC_SBC || c == OA2DP_CODEC_AAC || c == OA2DP_CODEC_APTX_HD ||
           c == OA2DP_CODEC_LDAC;
}

static bool has_frame_count(oa2dp_codec_id c)
{
    return c == OA2DP_CODEC_SBC || c == OA2DP_CODEC_LDAC;
}

size_t oa2dp_rtp_header_len(oa2dp_codec_id c)
{
    return (has_rtp(c) ? OA2DP_RTP_HEADER_LEN : 0) + (has_frame_count(c) ? 1 : 0);
}

int oa2dp_rtp_init(oa2dp_rtp *r, oa2dp_codec_id codec, uint8_t *buf, size_t mtu,
                   uint32_t ssrc)
{
    size_t h = oa2dp_rtp_header_len(codec);
    if (!r || !buf || mtu <= h + 1)
        return OA2DP_ERR_INVAL;
    memset(r, 0, sizeof(*r));
    r->codec = codec;
    r->buf = buf;
    r->mtu = mtu;
    r->header_len = h;
    r->len = h;
    r->ssrc = ssrc;
    r->seq = (uint16_t)(ssrc ^ (ssrc >> 16)); /* arbitrary start, RFC 3550 */
    return OA2DP_OK;
}

static unsigned max_frames(oa2dp_codec_id c)
{
    return has_frame_count(c) ? 15 : 255;
}

int oa2dp_rtp_add_frames(oa2dp_rtp *r, const uint8_t *data, size_t len,
                         uint8_t count, uint32_t samples)
{
    if (count == 0 || count > max_frames(r->codec) || len > r->mtu - r->header_len)
        return OA2DP_ERR_NOSPC;
    if (r->len + len > r->mtu || r->frames + count > max_frames(r->codec))
        return 0;
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    r->frames = (uint8_t)(r->frames + count);
    r->samples += samples;
    return 1;
}

int oa2dp_rtp_add_frame(oa2dp_rtp *r, const uint8_t *frame, size_t len, uint32_t samples)
{
    return oa2dp_rtp_add_frames(r, frame, len, 1, samples);
}

static void write_rtp(oa2dp_rtp *r, bool marker)
{
    uint8_t *b = r->buf;
    b[0] = 0x80; /* version 2 */
    b[1] = (uint8_t)((marker ? 0x80 : 0) | OA2DP_RTP_PAYLOAD_TYPE);
    b[2] = (uint8_t)(r->seq >> 8);
    b[3] = (uint8_t)r->seq;
    b[4] = (uint8_t)(r->timestamp >> 24);
    b[5] = (uint8_t)(r->timestamp >> 16);
    b[6] = (uint8_t)(r->timestamp >> 8);
    b[7] = (uint8_t)r->timestamp;
    b[8] = (uint8_t)(r->ssrc >> 24);
    b[9] = (uint8_t)(r->ssrc >> 16);
    b[10] = (uint8_t)(r->ssrc >> 8);
    b[11] = (uint8_t)r->ssrc;
}

size_t oa2dp_rtp_flush(oa2dp_rtp *r)
{
    if (r->frames == 0)
        return 0;
    if (has_rtp(r->codec))
        write_rtp(r, r->codec == OA2DP_CODEC_AAC);
    if (has_frame_count(r->codec))
        r->buf[OA2DP_RTP_HEADER_LEN] = (uint8_t)(r->frames & 0x0F);
    size_t n = r->len;
    r->seq++;
    r->timestamp += r->samples;
    r->len = r->header_len;
    r->frames = 0;
    r->samples = 0;
    return n;
}

int oa2dp_rtp_fragment(oa2dp_rtp *r, const uint8_t *frame, size_t len,
                       uint32_t samples, size_t *offset)
{
    if (r->frames != 0 || !offset || *offset >= len)
        return OA2DP_ERR_STATE;
    if (r->codec == OA2DP_CODEC_LDAC)
        return OA2DP_ERR_UNSUPPORTED; /* LDAC frames always fit an EDR MTU */

    size_t room = r->mtu - r->header_len;
    size_t left = len - *offset;
    size_t n = left < room ? left : room;
    bool first = *offset == 0, last = n == left;

    if (has_rtp(r->codec))
        write_rtp(r, r->codec == OA2DP_CODEC_AAC && last);
    if (r->codec == OA2DP_CODEC_SBC) {
        size_t remaining = (left + room - 1) / room; /* incl. this fragment */
        r->buf[OA2DP_RTP_HEADER_LEN] = (uint8_t)(0x80 | (first ? 0x40 : 0) |
                                                 (last ? 0x20 : 0) | (remaining & 0x0F));
    }
    memcpy(r->buf + r->header_len, frame + *offset, n);
    *offset += n;
    r->seq++;
    if (last)
        r->timestamp += samples;
    return (int)(r->header_len + n);
}

/* ---------------------------------------------------------------------- */
/* LATM                                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap, bit;
    bool overflow;
} bitw;

static void put_bits(bitw *w, uint32_t v, unsigned n)
{
    while (n--) {
        size_t byte = w->bit >> 3;
        if (byte >= w->cap) {
            w->overflow = true;
            return;
        }
        if ((w->bit & 7) == 0)
            w->buf[byte] = 0;
        if ((v >> n) & 1)
            w->buf[byte] |= (uint8_t)(0x80 >> (w->bit & 7));
        w->bit++;
    }
}

static int freq_index(uint32_t rate)
{
    static const uint32_t t[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; i++)
        if (t[i] == rate)
            return i;
    return -1;
}

int oa2dp_latm_write(uint8_t *out, size_t cap, const uint8_t *au, size_t au_len,
                     uint32_t rate, uint8_t channels)
{
    bitw w = {out, cap, 0, false};
    int fi = freq_index(rate);
    if (!out || !au || channels < 1 || channels > 2)
        return OA2DP_ERR_INVAL;

    put_bits(&w, 0, 1);            /* useSameStreamMux = 0 */
    /* StreamMuxConfig() */
    put_bits(&w, 0, 1);            /* audioMuxVersion = 0 */
    put_bits(&w, 1, 1);            /* allStreamsSameTimeFraming */
    put_bits(&w, 0, 6);            /* numSubFrames = 0 (one subframe) */
    put_bits(&w, 0, 4);            /* numProgram = 0 */
    put_bits(&w, 0, 3);            /* numLayer = 0 */
    /* AudioSpecificConfig() */
    put_bits(&w, 2, 5);            /* audioObjectType = AAC LC */
    if (fi >= 0) {
        put_bits(&w, (uint32_t)fi, 4);
    } else {
        put_bits(&w, 0xF, 4);
        put_bits(&w, rate, 24);
    }
    put_bits(&w, channels, 4);     /* channelConfiguration */
    put_bits(&w, 0, 1);            /* GASpecificConfig: frameLengthFlag (1024) */
    put_bits(&w, 0, 1);            /* dependsOnCoreCoder */
    put_bits(&w, 0, 1);            /* extensionFlag */
    put_bits(&w, 0, 3);            /* frameLengthType = 0 (variable) */
    put_bits(&w, 0xFF, 8);         /* latmBufferFullness: variable rate */
    put_bits(&w, 0, 1);            /* otherDataPresent */
    put_bits(&w, 0, 1);            /* crcCheckPresent */
    /* PayloadLengthInfo() */
    size_t left = au_len;
    while (left >= 255) {
        put_bits(&w, 255, 8);
        left -= 255;
    }
    put_bits(&w, (uint32_t)left, 8);
    /* PayloadMux() */
    for (size_t i = 0; i < au_len; i++)
        put_bits(&w, au[i], 8);
    /* byte alignment */
    while (w.bit & 7)
        put_bits(&w, 0, 1);
    return w.overflow ? OA2DP_ERR_NOSPC : (int)(w.bit >> 3);
}

/* ---------------------------------------------------------------------- */
/* Adaptive bitrate                                                       */
/* ---------------------------------------------------------------------- */

void oa2dp_abr_init(oa2dp_abr *a, uint8_t levels, uint32_t high_water,
                    uint32_t low_water, uint32_t down_hold_ms, uint32_t up_hold_ms)
{
    memset(a, 0, sizeof(*a));
    a->levels = levels ? levels : 1;
    a->high_water = high_water;
    a->low_water = low_water;
    a->down_hold_ms = down_hold_ms;
    a->up_hold_ms = up_hold_ms;
}

int oa2dp_abr_update(oa2dp_abr *a, uint32_t queued, uint32_t now)
{
    int8_t trend = queued >= a->high_water ? 1 : queued <= a->low_water ? -1 : 0;
    if (trend != a->trend) {
        a->trend = trend;
        a->since = now;
    }

    if (trend > 0 && a->level + 1 < a->levels &&
        (queued >= 2 * a->high_water || now - a->since >= a->down_hold_ms)) {
        a->level++;
        a->since = now;
        return a->level;
    }
    if (trend < 0 && a->level > 0 && now - a->since >= a->up_hold_ms) {
        a->level--;
        a->since = now;
        return a->level;
    }
    return -1;
}
