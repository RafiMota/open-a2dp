/* SPDX-License-Identifier: MIT */
/* SBC and FastStream through libsbc (BlueZ project, LGPL-2.1-or-later).
 * libsbc is linked dynamically (sbc.dll) to honour the LGPL. */
#ifdef OA2DP_WITH_LIBSBC

#include <stdlib.h>
#include <string.h>

#include <sbc/sbc.h>

#include "oa2dp/encoder.h"
#include "oa2dp/media.h"

#define SBC_ABR_LEVELS 5
#define FASTSTREAM_BITPOOL 29

typedef struct {
    oa2dp_encoder base;
    sbc_t sbc;
    uint8_t min_bitpool, max_bitpool;
    size_t codesize; /* PCM bytes per SBC frame */
    int16_t *pcm16;
} sbc_enc;

static int sbc_enc_encode(oa2dp_encoder *e, const int32_t *pcm, uint8_t *out, size_t cap,
                          size_t *out_len, uint8_t *frames)
{
    sbc_enc *x = (sbc_enc *)e;
    ssize_t written = 0;
    oa2dp_pcm_to_s16(pcm, x->pcm16, e->info.samples_per_call * e->info.channels);
    ssize_t used = sbc_encode(&x->sbc, x->pcm16, x->codesize, out, cap, &written);
    if (used < 0 || (size_t)used != x->codesize || written <= 0)
        return OA2DP_ERR_IO;
    *out_len = (size_t)written;
    *frames = 1;
    return OA2DP_OK;
}

static int sbc_enc_set_level(oa2dp_encoder *e, uint8_t level)
{
    sbc_enc *x = (sbc_enc *)e;
    if (level >= e->info.levels)
        return OA2DP_ERR_INVAL;
    /* libsbc picks up a changed bitpool on the next frame; the decoder reads
     * it from every frame header, so no renegotiation is needed. */
    x->sbc.bitpool = oa2dp_sbc_abr_bitpool(x->min_bitpool, x->max_bitpool, level, e->info.levels);
    return OA2DP_OK;
}

static void sbc_enc_destroy(oa2dp_encoder *e)
{
    sbc_enc *x = (sbc_enc *)e;
    sbc_finish(&x->sbc);
    free(x->pcm16);
    free(x);
}

static const oa2dp_encoder_ops sbc_ops = {sbc_enc_encode, sbc_enc_set_level, NULL, sbc_enc_destroy};

static int finish_create(sbc_enc *x, oa2dp_codec_id id, uint32_t rate, uint8_t channels,
                         bool adaptive, oa2dp_encoder **out)
{
    x->sbc.endian = SBC_LE;
    x->codesize = sbc_get_codesize(&x->sbc);
    x->base.ops = &sbc_ops;
    x->base.info.codec = id;
    x->base.info.sample_rate = rate;
    x->base.info.channels = channels;
    x->base.info.samples_per_call = (uint32_t)(x->codesize / (2u * channels));
    x->base.info.max_output = sbc_get_frame_length(&x->sbc);
    x->base.info.levels = adaptive ? SBC_ABR_LEVELS : 1;
    x->base.info.delay_us = 0;
    x->pcm16 = malloc(x->codesize);
    if (!x->pcm16) {
        sbc_finish(&x->sbc);
        free(x);
        return OA2DP_ERR_NOSPC;
    }
    *out = &x->base;
    return OA2DP_OK;
}

int oa2dp_sbc_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s,
                             oa2dp_encoder **out)
{
    uint8_t ie[OA2DP_CODEC_IE_MAX];
    if (c->codec != OA2DP_CODEC_SBC || oa2dp_codec_build(c, ie, sizeof(ie)) != 6)
        return OA2DP_ERR_INVAL;
    sbc_enc *x = calloc(1, sizeof(*x));
    if (!x)
        return OA2DP_ERR_NOSPC;
    /* sbc_init_a2dp takes the 4-byte SBC Codec Specific Information Element. */
    if (sbc_init_a2dp(&x->sbc, 0, ie + 2, 4) < 0) {
        free(x);
        return OA2DP_ERR_INVAL;
    }
    x->min_bitpool = c->u.sbc.min_bitpool;
    x->max_bitpool = c->u.sbc.max_bitpool;
    x->sbc.bitpool = x->max_bitpool;
    return finish_create(x, OA2DP_CODEC_SBC, oa2dp_codec_sample_rate(c),
                         oa2dp_codec_channels(c), s->adaptive_bitrate, out);
}

int oa2dp_faststream_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s,
                                    oa2dp_encoder **out)
{
    (void)s;
    if (c->codec != OA2DP_CODEC_FASTSTREAM)
        return OA2DP_ERR_INVAL;
    sbc_enc *x = calloc(1, sizeof(*x));
    if (!x)
        return OA2DP_ERR_NOSPC;
    if (sbc_init(&x->sbc, 0) < 0) {
        free(x);
        return OA2DP_ERR_INVAL;
    }
    /* FastStream: fixed joint-stereo SBC, 8 subbands, 16 blocks, bitpool 29. */
    uint32_t rate = oa2dp_codec_sample_rate(c);
    x->sbc.frequency = rate == 48000 ? SBC_FREQ_48000 : SBC_FREQ_44100;
    x->sbc.mode = SBC_MODE_JOINT_STEREO;
    x->sbc.subbands = SBC_SB_8;
    x->sbc.blocks = SBC_BLK_16;
    x->sbc.allocation = SBC_AM_LOUDNESS;
    x->sbc.bitpool = FASTSTREAM_BITPOOL;
    x->min_bitpool = x->max_bitpool = FASTSTREAM_BITPOOL;
    return finish_create(x, OA2DP_CODEC_FASTSTREAM, rate, 2, false, out);
}

#endif /* OA2DP_WITH_LIBSBC */
