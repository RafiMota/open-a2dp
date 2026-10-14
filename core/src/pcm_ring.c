/* SPDX-License-Identifier: MIT */
#include "oa2dp/pcm_ring.h"

#include <string.h>

#if defined(_MSC_VER)
#if defined(_KERNEL_MODE)
#include <wdm.h>
#define OA2DP_FENCE() KeMemoryBarrier()
#elif defined(_M_ARM64)
#include <intrin.h>
#define OA2DP_FENCE() __dmb(_ARM64_BARRIER_ISH)
#else
#include <intrin.h>
#define OA2DP_FENCE() __faststorefence()
#endif
#else
#define OA2DP_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

static uint8_t *ring_data(const oa2dp_pcm_ring *r)
{
    return (uint8_t *)(r + 1);
}

static size_t frame_bytes(const oa2dp_pcm_ring *r)
{
    return (size_t)r->channels * 4;
}

size_t oa2dp_ring_bytes(uint32_t capacity, uint16_t channels)
{
    return sizeof(oa2dp_pcm_ring) + (size_t)capacity * channels * 4;
}

int oa2dp_ring_init(oa2dp_pcm_ring *r, uint32_t capacity, uint32_t sample_rate,
                    uint16_t channels, uint16_t format)
{
    if (!r || capacity == 0 || (capacity & (capacity - 1)) || channels == 0 ||
        channels > 8 || format > OA2DP_RING_FMT_S32)
        return OA2DP_ERR_INVAL;
    memset(r, 0, sizeof(*r));
    r->magic = OA2DP_RING_MAGIC;
    r->version = OA2DP_RING_VERSION;
    r->capacity = capacity;
    r->sample_rate = sample_rate;
    r->channels = channels;
    r->format = format;
    return OA2DP_OK;
}

int oa2dp_ring_validate(const oa2dp_pcm_ring *r, size_t mapped)
{
    if (!r || mapped < sizeof(*r))
        return OA2DP_ERR_INVAL;
    if (r->magic != OA2DP_RING_MAGIC || r->version != OA2DP_RING_VERSION)
        return OA2DP_ERR_PROTO;
    if (r->capacity == 0 || (r->capacity & (r->capacity - 1)) || r->channels == 0 ||
        r->channels > 8 || r->format > OA2DP_RING_FMT_S32)
        return OA2DP_ERR_PROTO;
    if (oa2dp_ring_bytes(r->capacity, r->channels) > mapped)
        return OA2DP_ERR_PROTO;
    return OA2DP_OK;
}

uint32_t oa2dp_ring_readable(const oa2dp_pcm_ring *r)
{
    uint64_t w = r->write_pos;
    OA2DP_FENCE();
    uint64_t d = w - r->read_pos;
    return d > r->capacity ? r->capacity : (uint32_t)d;
}

uint32_t oa2dp_ring_writable(const oa2dp_pcm_ring *r)
{
    uint64_t rd = r->read_pos;
    OA2DP_FENCE();
    uint64_t d = r->write_pos - rd;
    return d >= r->capacity ? 0 : (uint32_t)(r->capacity - d);
}

static void copy_in(oa2dp_pcm_ring *r, uint64_t pos, const uint8_t *src, uint32_t frames)
{
    size_t fb = frame_bytes(r);
    uint32_t slot = (uint32_t)(pos & (r->capacity - 1));
    uint32_t first = r->capacity - slot;
    if (first > frames)
        first = frames;
    memcpy(ring_data(r) + slot * fb, src, first * fb);
    if (frames > first)
        memcpy(ring_data(r), src + first * fb, (frames - first) * fb);
}

static void copy_out(const oa2dp_pcm_ring *r, uint64_t pos, uint8_t *dst, uint32_t frames)
{
    size_t fb = frame_bytes(r);
    uint32_t slot = (uint32_t)(pos & (r->capacity - 1));
    uint32_t first = r->capacity - slot;
    if (first > frames)
        first = frames;
    memcpy(dst, ring_data(r) + slot * fb, first * fb);
    if (frames > first)
        memcpy(dst + first * fb, ring_data(r), (frames - first) * fb);
}

uint32_t oa2dp_ring_write(oa2dp_pcm_ring *r, const void *data, uint32_t frames)
{
    uint32_t room = oa2dp_ring_writable(r);
    uint32_t n = frames < room ? frames : room;
    if (n < frames)
        r->overruns += frames - n;
    if (n) {
        copy_in(r, r->write_pos, (const uint8_t *)data, n);
        OA2DP_FENCE(); /* data visible before the new position */
        r->write_pos += n;
    }
    return n;
}

uint32_t oa2dp_ring_read(oa2dp_pcm_ring *r, void *data, uint32_t frames)
{
    uint32_t avail = oa2dp_ring_readable(r);
    uint32_t n = frames < avail ? frames : avail;
    if (n) {
        copy_out(r, r->read_pos, (uint8_t *)data, n);
        OA2DP_FENCE(); /* finish reading before releasing the slots */
        r->read_pos += n;
    }
    return n;
}

/* Consumer-only; kept out of oa2dpaudio.sys, which never touches floats. */
#if !defined(_KERNEL_MODE)
void oa2dp_pcm_float_to_s24(const float *in, int32_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        if (!(v == v))
            v = 0.0f; /* NaN */
        if (v > 1.0f)
            v = 1.0f;
        if (v < -1.0f)
            v = -1.0f;
        float s = v * 8388608.0f;
        int32_t x = (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
        if (x > 8388607)
            x = 8388607;
        out[i] = x;
    }
}
#endif

void oa2dp_pcm_downmix_to_mono(const int32_t *in, int32_t *out, size_t frames)
{
    for (size_t i = 0; i < frames; i++)
        out[i] = (int32_t)(((int64_t)in[2 * i] + in[2 * i + 1]) / 2);
}
