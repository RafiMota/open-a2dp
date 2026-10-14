/* SPDX-License-Identifier: MIT */
/*
 * A2DP Codec Information Elements (the payload of the AVDTP "Media Codec"
 * service capability) for every codec OpenA2DP knows about.
 *
 * Sources: A2DP 1.3/1.4 specification (SBC, MPEG-2/4 AAC), Bluetooth SIG
 * Assigned Numbers (company IDs), and the public open-source stacks BlueZ,
 * PipeWire and AOSP for vendor-specific codec layouts.
 *
 * In a capability every field is a bitmask of supported values; in a
 * configuration every bitmask field has exactly one bit set.
 */
#ifndef OA2DP_A2DP_CODECS_H
#define OA2DP_A2DP_CODECS_H

#include "common.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OA2DP_MEDIA_TYPE_AUDIO 0x00

#define OA2DP_CODEC_TYPE_SBC 0x00
#define OA2DP_CODEC_TYPE_MPEG12 0x01
#define OA2DP_CODEC_TYPE_AAC 0x02
#define OA2DP_CODEC_TYPE_USAC 0x03
#define OA2DP_CODEC_TYPE_ATRAC 0x04
#define OA2DP_CODEC_TYPE_VENDOR 0xFF

/* Vendor IDs (Bluetooth SIG company identifiers) and vendor codec IDs. */
#define OA2DP_VENDOR_APT 0x0000004Fu
#define OA2DP_VENDOR_QTIL 0x000000D7u
#define OA2DP_VENDOR_CSR 0x0000000Au
#define OA2DP_VENDOR_SONY 0x0000012Du
#define OA2DP_APTX_CODEC_ID 0x0001
#define OA2DP_APTX_HD_CODEC_ID 0x0024
#define OA2DP_APTX_LL_CODEC_ID 0x0002
#define OA2DP_FASTSTREAM_CODEC_ID 0x0001
#define OA2DP_LDAC_CODEC_ID 0x00AA

/* --- SBC ------------------------------------------------------------- */
#define OA2DP_SBC_FREQ_16000 0x80
#define OA2DP_SBC_FREQ_32000 0x40
#define OA2DP_SBC_FREQ_44100 0x20
#define OA2DP_SBC_FREQ_48000 0x10
#define OA2DP_SBC_CH_MONO 0x08
#define OA2DP_SBC_CH_DUAL 0x04
#define OA2DP_SBC_CH_STEREO 0x02
#define OA2DP_SBC_CH_JOINT 0x01
#define OA2DP_SBC_BLOCKS_4 0x80
#define OA2DP_SBC_BLOCKS_8 0x40
#define OA2DP_SBC_BLOCKS_12 0x20
#define OA2DP_SBC_BLOCKS_16 0x10
#define OA2DP_SBC_SUBBANDS_4 0x08
#define OA2DP_SBC_SUBBANDS_8 0x04
#define OA2DP_SBC_ALLOC_SNR_BIT 0x02
#define OA2DP_SBC_ALLOC_LOUDNESS_BIT 0x01
#define OA2DP_SBC_MIN_BITPOOL 2
#define OA2DP_SBC_MAX_BITPOOL 250

typedef struct {
    uint8_t freq, chmode, blocks, subbands, alloc;
    uint8_t min_bitpool, max_bitpool;
} oa2dp_sbc_caps;

/* --- MPEG-2/4 AAC ---------------------------------------------------- */
#define OA2DP_AAC_OBJ_MPEG2_LC 0x80
#define OA2DP_AAC_OBJ_MPEG4_LC 0x40
#define OA2DP_AAC_OBJ_MPEG4_LTP 0x20
#define OA2DP_AAC_OBJ_MPEG4_SCA 0x10
/* 12-bit sampling frequency mask, MSB = 8000 Hz ... LSB = 96000 Hz. */
#define OA2DP_AAC_FREQ_8000 0x800
#define OA2DP_AAC_FREQ_11025 0x400
#define OA2DP_AAC_FREQ_12000 0x200
#define OA2DP_AAC_FREQ_16000 0x100
#define OA2DP_AAC_FREQ_22050 0x080
#define OA2DP_AAC_FREQ_24000 0x040
#define OA2DP_AAC_FREQ_32000 0x020
#define OA2DP_AAC_FREQ_44100 0x010
#define OA2DP_AAC_FREQ_48000 0x008
#define OA2DP_AAC_FREQ_64000 0x004
#define OA2DP_AAC_FREQ_88200 0x002
#define OA2DP_AAC_FREQ_96000 0x001
#define OA2DP_AAC_CH_1 0x08
#define OA2DP_AAC_CH_2 0x04

typedef struct {
    uint8_t object_type;
    uint16_t freq;     /* 12-bit mask */
    uint8_t channels;
    bool vbr;
    uint32_t bitrate;  /* 23 bits, bit/s; 0 = unknown */
} oa2dp_aac_caps;

/* --- aptX / aptX HD / aptX LL ----------------------------------------- */
#define OA2DP_APTX_FREQ_16000 0x80
#define OA2DP_APTX_FREQ_32000 0x40
#define OA2DP_APTX_FREQ_44100 0x20
#define OA2DP_APTX_FREQ_48000 0x10
#define OA2DP_APTX_CH_MONO 0x01
#define OA2DP_APTX_CH_STEREO 0x02

typedef struct {
    uint8_t freq, chmode;
} oa2dp_aptx_caps;

typedef struct {
    uint8_t freq, chmode;
    bool bidirect;
    bool has_new_caps;
    uint16_t target_level;
    uint16_t initial_level;
    uint8_t sra_max_rate;
    uint8_t sra_avg_time;
    uint16_t good_working_level;
} oa2dp_aptx_ll_caps;

/* --- FastStream -------------------------------------------------------- */
#define OA2DP_FS_DIR_SINK 0x01
#define OA2DP_FS_DIR_SOURCE 0x02
#define OA2DP_FS_SINK_FREQ_48000 0x01
#define OA2DP_FS_SINK_FREQ_44100 0x02
#define OA2DP_FS_SOURCE_FREQ_16000 0x02

typedef struct {
    uint8_t direction, sink_freq, source_freq;
} oa2dp_faststream_caps;

/* --- LDAC ------------------------------------------------------------- */
#define OA2DP_LDAC_FREQ_44100 0x20
#define OA2DP_LDAC_FREQ_48000 0x10
#define OA2DP_LDAC_FREQ_88200 0x08
#define OA2DP_LDAC_FREQ_96000 0x04
#define OA2DP_LDAC_FREQ_176400 0x02
#define OA2DP_LDAC_FREQ_192000 0x01
#define OA2DP_LDAC_CH_MONO 0x04
#define OA2DP_LDAC_CH_DUAL 0x02
#define OA2DP_LDAC_CH_STEREO 0x01

typedef struct {
    uint8_t freq, chmode;
} oa2dp_ldac_caps;

/* --- Generic ---------------------------------------------------------- */
typedef struct {
    oa2dp_codec_id codec;
    union {
        oa2dp_sbc_caps sbc;
        oa2dp_aac_caps aac;
        oa2dp_aptx_caps aptx;      /* APTX and APTX_HD */
        oa2dp_aptx_ll_caps aptx_ll;
        oa2dp_faststream_caps fs;
        oa2dp_ldac_caps ldac;
    } u;
} oa2dp_codec_caps;

/* Longest Codec Information Element we build (aptX LL with new caps). */
#define OA2DP_CODEC_IE_MAX 32

/* A2DP-specific AVDTP error codes (A2DP spec, "Error codes"). */
enum {
    OA2DP_A2DP_INVALID_CODEC_TYPE = 0xC1,
    OA2DP_A2DP_NOT_SUPPORTED_CODEC_TYPE = 0xC2,
    OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY = 0xC3,
    OA2DP_A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY = 0xC4,
    OA2DP_A2DP_INVALID_CHANNEL_MODE = 0xC5,
    OA2DP_A2DP_NOT_SUPPORTED_CHANNEL_MODE = 0xC6,
    OA2DP_A2DP_INVALID_SUBBANDS = 0xC7,
    OA2DP_A2DP_NOT_SUPPORTED_SUBBANDS = 0xC8,
    OA2DP_A2DP_INVALID_ALLOCATION_METHOD = 0xC9,
    OA2DP_A2DP_NOT_SUPPORTED_ALLOCATION_METHOD = 0xCA,
    OA2DP_A2DP_INVALID_MINIMUM_BITPOOL_VALUE = 0xCB,
    OA2DP_A2DP_NOT_SUPPORTED_MINIMUM_BITPOOL_VALUE = 0xCC,
    OA2DP_A2DP_INVALID_MAXIMUM_BITPOOL_VALUE = 0xCD,
    OA2DP_A2DP_NOT_SUPPORTED_MAXIMUM_BITPOOL_VALUE = 0xCE,
    OA2DP_A2DP_NOT_SUPPORTED_VBR = 0xD3,
    OA2DP_A2DP_NOT_SUPPORTED_BIT_RATE = 0xD5,
    OA2DP_A2DP_INVALID_OBJECT_TYPE = 0xD6,
    OA2DP_A2DP_NOT_SUPPORTED_OBJECT_TYPE = 0xD7,
    OA2DP_A2DP_INVALID_CHANNELS = 0xD8,
    OA2DP_A2DP_NOT_SUPPORTED_CHANNELS = 0xD9,
    OA2DP_A2DP_INVALID_BLOCK_LENGTH = 0xDD,
    OA2DP_A2DP_INVALID_CODEC_PARAMETER = 0xE0,
    OA2DP_A2DP_NOT_SUPPORTED_CODEC_PARAMETER = 0xE1,
};

const char *oa2dp_codec_name(oa2dp_codec_id id); /* "aptX HD", ... */

/* Parse a Media Codec capability payload (starts with the media-type byte).
 * Returns OA2DP_OK, OA2DP_ERR_INVAL (malformed) or OA2DP_ERR_UNSUPPORTED
 * (well-formed but unknown codec). */
int oa2dp_codec_parse(const uint8_t *ie, size_t len, oa2dp_codec_caps *out);

/* Serialise; returns bytes written or a negative error. */
int oa2dp_codec_build(const oa2dp_codec_caps *c, uint8_t *buf, size_t cap);

/* What OpenA2DP can encode for a codec (used for our SEP capabilities). */
void oa2dp_codec_local_caps(oa2dp_codec_id id, oa2dp_codec_caps *out);

/* Choose a configuration for one codec from the remote capabilities and the
 * user settings. Returns OA2DP_OK or OA2DP_ERR_UNSUPPORTED when no common
 * configuration exists. `local` may be NULL to use oa2dp_codec_local_caps. */
int oa2dp_codec_select(const oa2dp_codec_caps *local,
                       const oa2dp_codec_caps *remote,
                       const oa2dp_settings *s, oa2dp_codec_caps *config);

/* Check a configuration proposed by the peer against our capabilities.
 * Returns 0 if acceptable, otherwise an A2DP/AVDTP error code (> 0). */
uint8_t oa2dp_codec_check_config(const oa2dp_codec_caps *local,
                                 const oa2dp_codec_caps *config);

/* Among `count` remote endpoints, choose the first codec in the user's
 * priority list that both sides support. Returns the index into `remote`
 * or OA2DP_ERR_NOTFOUND. */
int oa2dp_codec_choose_best(const oa2dp_codec_caps *remote, size_t count,
                            const oa2dp_settings *s, oa2dp_codec_caps *config);

/* SBC bitpool limits for one channel mode / subband bit (OA2DP_SBC_CH_*,
 * OA2DP_SBC_SUBBANDS_*): the most the frame format allows, and the bitpool
 * used when the user leaves it on automatic (A2DP high-quality presets). */
uint8_t oa2dp_sbc_max_bitpool(uint8_t chmode, uint8_t subbands);
uint8_t oa2dp_sbc_auto_bitpool(uint8_t chmode, uint8_t freq);

/* Decoded properties of a single configuration. */
uint32_t oa2dp_codec_sample_rate(const oa2dp_codec_caps *config);
uint8_t oa2dp_codec_channels(const oa2dp_codec_caps *config);
/* PCM bits per sample the encoder consumes for this configuration. */
uint8_t oa2dp_codec_pcm_bits(const oa2dp_codec_caps *config,
                             const oa2dp_settings *s);
/* Change only the sampling rate of a configuration (for RECONFIGURE).
 * Returns OA2DP_ERR_UNSUPPORTED if `remote` cannot do `rate`. */
int oa2dp_codec_set_rate(oa2dp_codec_caps *config,
                         const oa2dp_codec_caps *remote, uint32_t rate);

#ifdef __cplusplus
}
#endif

#endif
