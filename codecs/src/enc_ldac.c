/* SPDX-License-Identifier: MIT */
/* LDAC through libldac and libldacBT_abr from AOSP (Apache-2.0). */
#ifdef OA2DP_WITH_LIBLDAC

#include <stdlib.h>

#include <ldacBT.h>
#ifdef OA2DP_WITH_LDAC_ABR
#include <ldacBT_abr.h>
#endif

#include "oa2dp/encoder.h"

/* LDAC consumes 128 samples per channel per ldacBT_encode() call. */
#define LDAC_SAMPLES_PER_CALL 128
#define LDAC_MAX_OUTPUT 1024

/* ABR tuning used by the open-source stacks that ship libldacBT_abr
 * (interval in ms, then queue-depth thresholds in packets). Tune with
 * real-world testing. */
#define LDAC_ABR_INTERVAL_MS 5
#define LDAC_ABR_TH_CRITICAL 6
#define LDAC_ABR_TH_DANGEROUS_TREND 4
#define LDAC_ABR_TH_SAFETY_HQSQ 2

typedef struct {
    oa2dp_encoder base;
    HANDLE_LDAC_BT h;
#ifdef OA2DP_WITH_LDAC_ABR
    HANDLE_LDAC_ABR abr;
#endif
    int32_t *pcm32;
} ldac_enc;

static int eqmid_for(oa2dp_ldac_quality q)
{
    switch (q) {
    case OA2DP_LDAC_HQ: return LDACBT_EQMID_HQ;
    case OA2DP_LDAC_MQ: return LDACBT_EQMID_MQ;
    default: return LDACBT_EQMID_SQ; /* SQ, and the starting point for ABR */
    }
}

static int ldac_enc_encode(oa2dp_encoder *e, const int32_t *pcm, uint8_t *out, size_t cap,
                           size_t *out_len, uint8_t *frames)
{
    ldac_enc *x = (ldac_enc *)e;
    int used = 0, size = 0, nframes = 0;
    if (cap < LDAC_MAX_OUTPUT)
        return OA2DP_ERR_NOSPC;
    oa2dp_pcm_to_s32(pcm, x->pcm32, (size_t)e->info.samples_per_call * e->info.channels);
    if (ldacBT_encode(x->h, x->pcm32, &used, out, &size, &nframes) < 0)
        return OA2DP_ERR_IO;
    *out_len = (size_t)size;
    *frames = (uint8_t)nframes;
    return OA2DP_OK;
}

static int ldac_enc_set_level(oa2dp_encoder *e, uint8_t level)
{
    static const int eq[] = {LDACBT_EQMID_HQ, LDACBT_EQMID_SQ, LDACBT_EQMID_MQ};
    ldac_enc *x = (ldac_enc *)e;
    if (level >= 3)
        return OA2DP_ERR_INVAL;
    return ldacBT_set_eqmid(x->h, eq[level]) < 0 ? OA2DP_ERR_IO : OA2DP_OK;
}

#ifdef OA2DP_WITH_LDAC_ABR
static void ldac_enc_feedback(oa2dp_encoder *e, uint32_t queued)
{
    ldac_enc *x = (ldac_enc *)e;
    if (x->abr)
        ldac_ABR_Proc(x->h, x->abr, queued, 1);
}
#endif

static void ldac_enc_destroy(oa2dp_encoder *e)
{
    ldac_enc *x = (ldac_enc *)e;
#ifdef OA2DP_WITH_LDAC_ABR
    if (x->abr)
        ldac_ABR_free_handle(x->abr);
#endif
    if (x->h) {
        ldacBT_close_handle(x->h);
        ldacBT_free_handle(x->h);
    }
    free(x->pcm32);
    free(x);
}

static const oa2dp_encoder_ops ldac_ops = {
    ldac_enc_encode, ldac_enc_set_level,
#ifdef OA2DP_WITH_LDAC_ABR
    ldac_enc_feedback,
#else
    NULL,
#endif
    ldac_enc_destroy};

int oa2dp_ldac_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s,
                              size_t mtu, oa2dp_encoder **out)
{
    if (c->codec != OA2DP_CODEC_LDAC)
        return OA2DP_ERR_INVAL;
    ldac_enc *x = calloc(1, sizeof(*x));
    if (!x)
        return OA2DP_ERR_NOSPC;
    uint8_t ch = oa2dp_codec_channels(c);
    x->pcm32 = malloc(sizeof(int32_t) * LDAC_SAMPLES_PER_CALL * ch);
    x->h = ldacBT_get_handle();
    if (!x->pcm32 || !x->h) {
        ldac_enc_destroy(&x->base);
        return OA2DP_ERR_NOSPC;
    }
    /* The channel-mode bits of the A2DP element equal LDACBT_CHANNEL_MODE_*. */
    if (ldacBT_init_handle_encode(x->h, (int)mtu, eqmid_for(s->ldac_quality),
                                  c->u.ldac.chmode, LDACBT_SMPL_FMT_S32,
                                  (int)oa2dp_codec_sample_rate(c)) < 0) {
        ldac_enc_destroy(&x->base);
        return OA2DP_ERR_INVAL;
    }

    bool abr = s->ldac_quality == OA2DP_LDAC_ABR;
#ifdef OA2DP_WITH_LDAC_ABR
    if (abr && s->adaptive_bitrate) {
        x->abr = ldac_ABR_get_handle();
        if (x->abr) {
            ldac_ABR_Init(x->abr, LDAC_ABR_INTERVAL_MS);
            ldac_ABR_set_thresholds(x->abr, LDAC_ABR_TH_CRITICAL, LDAC_ABR_TH_DANGEROUS_TREND,
                                    LDAC_ABR_TH_SAFETY_HQSQ);
        }
    }
    x->base.info.native_abr = x->abr != NULL;
#endif

    x->base.ops = &ldac_ops;
    x->base.info.codec = OA2DP_CODEC_LDAC;
    x->base.info.sample_rate = oa2dp_codec_sample_rate(c);
    x->base.info.channels = ch;
    x->base.info.samples_per_call = LDAC_SAMPLES_PER_CALL;
    x->base.info.max_output = LDAC_MAX_OUTPUT;
    /* Without the ABR library, fall back to the generic controller over
     * HQ/SQ/MQ when the user picked "adaptive". */
    x->base.info.levels = (abr && s->adaptive_bitrate && !x->base.info.native_abr) ? 3 : 1;
    *out = &x->base;
    return OA2DP_OK;
}

#endif /* OA2DP_WITH_LIBLDAC */
