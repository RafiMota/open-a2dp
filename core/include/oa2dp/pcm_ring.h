/* SPDX-License-Identifier: MIT */
/*
 * Single-producer / single-consumer PCM ring shared between the virtual
 * audio driver (producer, kernel) and the service (consumer, user mode)
 * through one mapped memory section. Plain C, no OS headers, so the same
 * code builds in both worlds and in unit tests.
 *
 * Positions are 64-bit frame counters that only grow; the slot is
 * position % capacity. The producer never overwrites unread audio: when
 * the consumer lags, new audio is dropped and counted in `overruns`.
 */
#ifndef OA2DP_PCM_RING_H
#define OA2DP_PCM_RING_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OA2DP_RING_MAGIC 0x4D435041u /* "APCM" */
#define OA2DP_RING_VERSION 1

#define OA2DP_RING_FMT_FLOAT32 0 /* interleaved IEEE float, [-1, 1] */
#define OA2DP_RING_FMT_S32 1     /* interleaved int32, 24-bit values */

#define OA2DP_RING_FLAG_RUNNING 0x1        /* endpoint is rendering */
#define OA2DP_RING_FLAG_FORMAT_CHANGED 0x2 /* consumer must re-read format */

#if defined(_MSC_VER)
#define OA2DP_ALIGN8 __declspec(align(8))
#else
#define OA2DP_ALIGN8 __attribute__((aligned(8)))
#endif

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;      /* frames, power of two */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t format;        /* OA2DP_RING_FMT_* */
    volatile uint32_t flags;
    OA2DP_ALIGN8 volatile uint64_t write_pos; /* written by producer only */
    OA2DP_ALIGN8 volatile uint64_t read_pos;  /* written by consumer only */
    OA2DP_ALIGN8 volatile uint64_t overruns;  /* frames dropped by producer */
    uint32_t reserved[8];
    /* followed by capacity * channels * 4 bytes of sample data */
} oa2dp_pcm_ring;

/* Total bytes to allocate for a ring (header + data). */
size_t oa2dp_ring_bytes(uint32_t capacity, uint16_t channels);

int oa2dp_ring_init(oa2dp_pcm_ring *r, uint32_t capacity, uint32_t sample_rate,
                    uint16_t channels, uint16_t format);

/* Validate a ring mapped from the other side. `mapped_bytes` is the size of
 * the mapping, so a hostile/buggy header cannot make us read past it. */
int oa2dp_ring_validate(const oa2dp_pcm_ring *r, size_t mapped_bytes);

uint32_t oa2dp_ring_readable(const oa2dp_pcm_ring *r);
uint32_t oa2dp_ring_writable(const oa2dp_pcm_ring *r);

/* Producer: copy up to `frames`; returns frames written. */
uint32_t oa2dp_ring_write(oa2dp_pcm_ring *r, const void *data, uint32_t frames);
/* Consumer: copy up to `frames`; returns frames read. */
uint32_t oa2dp_ring_read(oa2dp_pcm_ring *r, void *data, uint32_t frames);

/* Sample conversion for the consumer. */
void oa2dp_pcm_float_to_s24(const float *in, int32_t *out, size_t n);
/* Interleaved stereo -> mono average, in place allowed (out may == in). */
void oa2dp_pcm_downmix_to_mono(const int32_t *in, int32_t *out, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
