/* SPDX-License-Identifier: MIT */
/*
 * Media-path helpers: SBC frame arithmetic, RTP packetisation per codec,
 * MPEG-4 LATM framing for AAC and the adaptive-bitrate controller.
 */
#ifndef OA2DP_MEDIA_H
#define OA2DP_MEDIA_H

#include "a2dp_codecs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- SBC arithmetic (A2DP spec, "SBC frame length and bit rate") ------- */

typedef struct {
    unsigned subbands;  /* 4 or 8 */
    unsigned blocks;    /* 4, 8, 12, 16 */
    unsigned channels;  /* 1 or 2 */
    uint8_t chmode;     /* OA2DP_SBC_CH_* (single bit) */
} oa2dp_sbc_layout;

int oa2dp_sbc_layout_from_config(const oa2dp_sbc_caps *cfg, oa2dp_sbc_layout *out);
unsigned oa2dp_sbc_frame_len(const oa2dp_sbc_layout *l, unsigned bitpool);
/* bit/s */
uint32_t oa2dp_sbc_bitrate(const oa2dp_sbc_layout *l, unsigned bitpool, uint32_t rate);
/* Largest bitpool in [min,max] whose bitrate does not exceed `target`
 * (returns `min` if even that is too high). */
uint8_t oa2dp_sbc_bitpool_for_bitrate(const oa2dp_sbc_layout *l, uint32_t rate,
                                      uint32_t target, uint8_t min, uint8_t max);

/* --- RTP / media packets ---------------------------------------------- */

#define OA2DP_RTP_HEADER_LEN 12
#define OA2DP_RTP_PAYLOAD_TYPE 96

typedef struct {
    oa2dp_codec_id codec;
    uint8_t *buf;        /* caller-owned, at least `mtu` bytes */
    size_t mtu;
    size_t header_len;   /* RTP + codec media payload header */
    size_t len;          /* bytes used, including header space */
    uint8_t frames;
    uint32_t samples;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} oa2dp_rtp;

/* Bytes in front of the codec data: 13 for SBC/LDAC, 12 for AAC and
 * aptX HD, 0 for aptX, aptX LL and FastStream (raw streams). */
size_t oa2dp_rtp_header_len(oa2dp_codec_id codec);

int oa2dp_rtp_init(oa2dp_rtp *r, oa2dp_codec_id codec, uint8_t *buf, size_t mtu,
                   uint32_t ssrc);

/* Append one encoded frame. Returns 1 when appended, 0 when it does not fit
 * (flush first), OA2DP_ERR_NOSPC when the frame cannot fit even an empty
 * packet (use oa2dp_rtp_fragment). */
int oa2dp_rtp_add_frame(oa2dp_rtp *r, const uint8_t *frame, size_t len,
                        uint32_t samples);

/* Same, for a chunk that already holds `count` codec frames (LDAC encoder
 * output). */
int oa2dp_rtp_add_frames(oa2dp_rtp *r, const uint8_t *data, size_t len,
                         uint8_t count, uint32_t samples);

/* Finish the current packet: writes headers, advances sequence number and
 * timestamp. Returns the packet length (0 if nothing was added). */
size_t oa2dp_rtp_flush(oa2dp_rtp *r);

/* Emit the next fragment of an oversized frame into r->buf. `*offset` is
 * advanced; the frame is complete when *offset == len. Only valid with an
 * empty packet. Returns the packet length or < 0. */
int oa2dp_rtp_fragment(oa2dp_rtp *r, const uint8_t *frame, size_t len,
                       uint32_t samples, size_t *offset);

/* --- MPEG-4 LATM (AAC) ------------------------------------------------ */

/* Wrap one raw AAC-LC access unit in an AudioMuxElement with
 * muxConfigPresent = 1 (StreamMuxConfig in-band), as used over A2DP.
 * Returns the byte length or < 0. */
int oa2dp_latm_write(uint8_t *out, size_t cap, const uint8_t *au, size_t au_len,
                     uint32_t sample_rate, uint8_t channels);

/* --- Adaptive bitrate ------------------------------------------------- */

typedef struct {
    uint8_t level;          /* 0 = best quality */
    uint8_t levels;
    uint32_t high_water;    /* queued bytes that count as congestion */
    uint32_t low_water;     /* queued bytes that count as clear */
    uint32_t down_hold_ms;  /* congestion must last this long to step down */
    uint32_t up_hold_ms;    /* clear link must last this long to step up */
    uint32_t since;
    int8_t trend;           /* -1 clear, 0 neutral, +1 congested */
} oa2dp_abr;

void oa2dp_abr_init(oa2dp_abr *a, uint8_t levels, uint32_t high_water,
                    uint32_t low_water, uint32_t down_hold_ms, uint32_t up_hold_ms);
/* Feed the current transmit-queue depth. Returns the new level when it
 * changed, otherwise -1. Queue depth >= 2 * high_water steps down at once. */
int oa2dp_abr_update(oa2dp_abr *a, uint32_t queued, uint32_t now_ms);

/* SBC bitpool for an ABR level: evenly spaced from `max` (level 0) down to
 * max(min, max/2) (last level). */
uint8_t oa2dp_sbc_abr_bitpool(uint8_t min, uint8_t max, uint8_t level, uint8_t levels);

#ifdef __cplusplus
}
#endif

#endif
