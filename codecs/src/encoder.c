/* SPDX-License-Identifier: MIT */
#include "oa2dp/encoder.h"

bool oa2dp_encoder_available(oa2dp_codec_id id)
{
    switch (id) {
#ifdef OA2DP_WITH_LIBSBC
    case OA2DP_CODEC_SBC:
    case OA2DP_CODEC_FASTSTREAM:
        return true;
#endif
#ifdef OA2DP_WITH_OPENAPTX
    case OA2DP_CODEC_APTX:
    case OA2DP_CODEC_APTX_HD:
    case OA2DP_CODEC_APTX_LL:
        return true;
#endif
#ifdef OA2DP_WITH_LIBLDAC
    case OA2DP_CODEC_LDAC:
        return true;
#endif
#ifdef OA2DP_WITH_MFAAC
    case OA2DP_CODEC_AAC:
        return true;
#endif
    default:
        return false;
    }
}

int oa2dp_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s,
                         size_t mtu, oa2dp_encoder **out)
{
    if (!c || !s || !out)
        return OA2DP_ERR_INVAL;
    *out = NULL;
    (void)mtu;
    switch (c->codec) {
#ifdef OA2DP_WITH_LIBSBC
    case OA2DP_CODEC_SBC: return oa2dp_sbc_encoder_create(c, s, out);
    case OA2DP_CODEC_FASTSTREAM: return oa2dp_faststream_encoder_create(c, s, out);
#endif
#ifdef OA2DP_WITH_OPENAPTX
    case OA2DP_CODEC_APTX:
    case OA2DP_CODEC_APTX_HD:
    case OA2DP_CODEC_APTX_LL:
        return oa2dp_aptx_encoder_create(c, s, out);
#endif
#ifdef OA2DP_WITH_LIBLDAC
    case OA2DP_CODEC_LDAC: return oa2dp_ldac_encoder_create(c, s, mtu, out);
#endif
#ifdef OA2DP_WITH_MFAAC
    case OA2DP_CODEC_AAC: return oa2dp_aac_mf_encoder_create(c, s, out);
#endif
    default:
        return OA2DP_ERR_UNSUPPORTED;
    }
}

void oa2dp_pcm_to_s16(const int32_t *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int32_t v = (in[i] + 128) >> 8; /* round to nearest */
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        out[i] = (int16_t)v;
    }
}

void oa2dp_pcm_to_s24_packed(const int32_t *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int32_t v = in[i];
        if (v > 8388607)
            v = 8388607;
        if (v < -8388608)
            v = -8388608;
        out[3 * i] = (uint8_t)v;
        out[3 * i + 1] = (uint8_t)(v >> 8);
        out[3 * i + 2] = (uint8_t)(v >> 16);
    }
}

void oa2dp_pcm_to_s32(const int32_t *in, int32_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int32_t v = in[i];
        if (v > 8388607)
            v = 8388607;
        if (v < -8388608)
            v = -8388608;
        out[i] = (int32_t)((uint32_t)v << 8);
    }
}
