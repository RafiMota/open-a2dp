/* SPDX-License-Identifier: MIT */
/*
 * Common interface over the codec libraries. Every adapter consumes the
 * same PCM format: interleaved int32_t samples carrying 24-bit values
 * (range -8388608 .. 8388607). Adapters convert to what their library needs.
 */
#ifndef OA2DP_ENCODER_H
#define OA2DP_ENCODER_H

#include "oa2dp/a2dp_codecs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oa2dp_encoder oa2dp_encoder;

typedef struct {
    oa2dp_codec_id codec;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t samples_per_call; /* per channel; encode() always consumes this many */
    size_t max_output;         /* worst-case bytes produced by one encode() */
    uint8_t levels;            /* quality levels for oa2dp_abr (1 = fixed) */
    bool native_abr;           /* encoder adapts itself from queue_feedback() */
    uint32_t delay_us;         /* algorithmic encoder delay */
} oa2dp_encoder_info;

typedef struct {
    /* Encode exactly info.samples_per_call samples per channel. May produce
     * no output (encoder priming). *frames = codec frames in the output. */
    int (*encode)(oa2dp_encoder *e, const int32_t *pcm, uint8_t *out, size_t cap,
                  size_t *out_len, uint8_t *frames);
    /* 0 = best quality. Optional. */
    int (*set_level)(oa2dp_encoder *e, uint8_t level);
    /* Transmit queue depth in packets, for encoders with native ABR. Optional. */
    void (*queue_feedback)(oa2dp_encoder *e, uint32_t queued_packets);
    void (*destroy)(oa2dp_encoder *e);
} oa2dp_encoder_ops;

struct oa2dp_encoder {
    const oa2dp_encoder_ops *ops;
    oa2dp_encoder_info info;
};

/* True when the adapter for `id` was compiled in. */
bool oa2dp_encoder_available(oa2dp_codec_id id);

/* Create an encoder for a negotiated configuration. `mtu` is the media
 * channel MTU (LDAC sizes its frames from it). */
int oa2dp_encoder_create(const oa2dp_codec_caps *config, const oa2dp_settings *s,
                         size_t mtu, oa2dp_encoder **out);

static inline int oa2dp_encoder_encode(oa2dp_encoder *e, const int32_t *pcm, uint8_t *out,
                                       size_t cap, size_t *out_len, uint8_t *frames)
{
    return e->ops->encode(e, pcm, out, cap, out_len, frames);
}

static inline int oa2dp_encoder_set_level(oa2dp_encoder *e, uint8_t level)
{
    return e->ops->set_level ? e->ops->set_level(e, level) : OA2DP_ERR_UNSUPPORTED;
}

static inline void oa2dp_encoder_destroy(oa2dp_encoder *e)
{
    if (e)
        e->ops->destroy(e);
}

/* Sample conversion helpers shared by adapters. */
void oa2dp_pcm_to_s16(const int32_t *in, int16_t *out, size_t n);
void oa2dp_pcm_to_s24_packed(const int32_t *in, uint8_t *out, size_t n);
void oa2dp_pcm_to_s32(const int32_t *in, int32_t *out, size_t n);

/* Adapter constructors (each defined only when its library is enabled). */
int oa2dp_sbc_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s, oa2dp_encoder **out);
int oa2dp_faststream_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s, oa2dp_encoder **out);
int oa2dp_aptx_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s, oa2dp_encoder **out);
int oa2dp_ldac_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s, size_t mtu, oa2dp_encoder **out);
int oa2dp_aac_mf_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s, oa2dp_encoder **out);

#ifdef __cplusplus
}
#endif

#endif
