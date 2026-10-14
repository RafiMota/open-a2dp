/* SPDX-License-Identifier: MIT */
/* Per-device user settings (SPEC F3). */
#ifndef OA2DP_SETTINGS_H
#define OA2DP_SETTINGS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OA2DP_CODEC_SBC = 0,
    OA2DP_CODEC_AAC,
    OA2DP_CODEC_APTX,
    OA2DP_CODEC_APTX_HD,
    OA2DP_CODEC_APTX_LL,
    OA2DP_CODEC_FASTSTREAM,
    OA2DP_CODEC_LDAC,
    OA2DP_CODEC_COUNT,
    OA2DP_CODEC_UNKNOWN = -1
} oa2dp_codec_id;

/* 0 always means "automatic". */
typedef enum {
    OA2DP_CHMODE_AUTO = 0,
    OA2DP_CHMODE_MONO,
    OA2DP_CHMODE_DUAL,
    OA2DP_CHMODE_STEREO,
    OA2DP_CHMODE_JOINT
} oa2dp_chmode;

typedef enum {
    OA2DP_SBC_ALLOC_AUTO = 0,
    OA2DP_SBC_ALLOC_LOUDNESS,
    OA2DP_SBC_ALLOC_SNR
} oa2dp_sbc_alloc;

typedef enum {
    OA2DP_LDAC_ABR = 0, /* adaptive, default */
    OA2DP_LDAC_HQ,      /* 990 / 909 kbps */
    OA2DP_LDAC_SQ,      /* 660 / 606 kbps */
    OA2DP_LDAC_MQ       /* 330 / 303 kbps */
} oa2dp_ldac_quality;

typedef struct {
    /* Codecs to try, best first. Codecs not listed are disabled. */
    oa2dp_codec_id priority[OA2DP_CODEC_COUNT];
    uint8_t priority_count;

    uint32_t sample_rate;  /* Hz, 0 = auto (48000, then 44100, then best) */
    uint8_t bits;          /* 16 or 24, 0 = auto (24 when codec allows) */
    oa2dp_chmode chmode;   /* applies to every codec; SBC also has DUAL/JOINT */

    /* SBC */
    uint8_t sbc_bitpool;   /* 0 = auto */
    uint8_t sbc_blocks;    /* 4/8/12/16, 0 = auto (16) */
    uint8_t sbc_subbands;  /* 4/8, 0 = auto (8) */
    oa2dp_sbc_alloc sbc_alloc;

    /* AAC */
    uint32_t aac_bitrate;  /* bit/s, 0 = highest the headset allows */
    bool aac_vbr;

    /* LDAC */
    oa2dp_ldac_quality ldac_quality;

    /* Lower quality automatically on a congested link (F3.9). */
    bool adaptive_bitrate;

    /* Follow the Windows endpoint format with AVDTP RECONFIGURE (F4.3). */
    bool follow_source_rate;
} oa2dp_settings;

void oa2dp_settings_default(oa2dp_settings *s);

/* Text form: one "key=value" per line. Unknown keys are ignored so that newer
 * settings files load in older versions. Returns OA2DP_OK or OA2DP_ERR_INVAL
 * (with *bad_line set to the 1-based line number) for invalid values. */
int oa2dp_settings_parse(oa2dp_settings *s, const char *text, int *bad_line);

/* Returns the number of characters written (excluding NUL), or
 * OA2DP_ERR_NOSPC. */
int oa2dp_settings_format(const oa2dp_settings *s, char *buf, size_t cap);

const char *oa2dp_codec_key(oa2dp_codec_id id);    /* "sbc", "aptx_hd", ... */
oa2dp_codec_id oa2dp_codec_from_key(const char *key, size_t len);

#ifdef __cplusplus
}
#endif

#endif
