/* SPDX-License-Identifier: MIT */
/* aptX, aptX HD and aptX Low Latency through libopenaptx (Pali Rohár).
 * Check the libopenaptx licence of the version you ship (see
 * THIRD_PARTY.md); it is linked dynamically. aptX LL uses the classic aptX
 * bitstream; only buffering differs, which the service handles. */
#ifdef OA2DP_WITH_OPENAPTX

#include <stdlib.h>

#include <openaptx.h>

#include "oa2dp/encoder.h"

/* 4 PCM samples per channel make one aptX codeword per channel. */
#define APTX_SAMPLES_PER_CALL 128

typedef struct {
    oa2dp_encoder base;
    struct aptx_context *ctx;
    uint8_t *pcm24;
} aptx_enc;

static int aptx_enc_encode(oa2dp_encoder *e, const int32_t *pcm, uint8_t *out, size_t cap,
                           size_t *out_len, uint8_t *frames)
{
    aptx_enc *x = (aptx_enc *)e;
    size_t n = (size_t)e->info.samples_per_call * 2;
    size_t written = 0;
    oa2dp_pcm_to_s24_packed(pcm, x->pcm24, n);
    size_t used = aptx_encode(x->ctx, x->pcm24, n * 3, out, cap, &written);
    if (used != n * 3)
        return OA2DP_ERR_IO;
    *out_len = written;
    *frames = 1;
    return OA2DP_OK;
}

static void aptx_enc_destroy(oa2dp_encoder *e)
{
    aptx_enc *x = (aptx_enc *)e;
    aptx_finish(x->ctx);
    free(x->pcm24);
    free(x);
}

static const oa2dp_encoder_ops aptx_ops = {aptx_enc_encode, NULL, NULL, aptx_enc_destroy};

int oa2dp_aptx_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s,
                              oa2dp_encoder **out)
{
    (void)s;
    bool hd = c->codec == OA2DP_CODEC_APTX_HD;
    if (c->codec != OA2DP_CODEC_APTX && !hd && c->codec != OA2DP_CODEC_APTX_LL)
        return OA2DP_ERR_INVAL;
    if (oa2dp_codec_channels(c) != 2)
        return OA2DP_ERR_UNSUPPORTED; /* libopenaptx encodes stereo only */

    aptx_enc *x = calloc(1, sizeof(*x));
    if (!x)
        return OA2DP_ERR_NOSPC;
    x->ctx = aptx_init(hd ? 1 : 0);
    x->pcm24 = malloc(APTX_SAMPLES_PER_CALL * 2 * 3);
    if (!x->ctx || !x->pcm24) {
        if (x->ctx)
            aptx_finish(x->ctx);
        free(x->pcm24);
        free(x);
        return OA2DP_ERR_NOSPC;
    }
    x->base.ops = &aptx_ops;
    x->base.info.codec = c->codec;
    x->base.info.sample_rate = oa2dp_codec_sample_rate(c);
    x->base.info.channels = 2;
    x->base.info.samples_per_call = APTX_SAMPLES_PER_CALL;
    /* 4 samples -> 2 bytes (aptX) or 3 bytes (HD) per channel */
    x->base.info.max_output = APTX_SAMPLES_PER_CALL / 4 * 2 * (hd ? 3 : 2);
    x->base.info.levels = 1;
    x->base.info.delay_us = 0; /* TODO: measure the QMF delay of libopenaptx */
    *out = &x->base;
    return OA2DP_OK;
}

#endif /* OA2DP_WITH_OPENAPTX */
