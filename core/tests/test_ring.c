/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "oa2dp/pcm_ring.h"
#include "test.h"

void test_ring(void)
{
    size_t bytes = oa2dp_ring_bytes(8, 2);
    oa2dp_pcm_ring *r = calloc(1, bytes);
    int32_t in[2 * 16], out[2 * 16];
    for (int i = 0; i < 32; i++)
        in[i] = i + 1;

    CHECK_EQ(oa2dp_ring_init(r, 6, 48000, 2, OA2DP_RING_FMT_S32), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_ring_init(r, 8, 48000, 2, OA2DP_RING_FMT_S32), OA2DP_OK);
    CHECK_EQ(oa2dp_ring_validate(r, bytes), OA2DP_OK);
    CHECK_EQ(oa2dp_ring_validate(r, bytes - 1), OA2DP_ERR_PROTO);

    CHECK_EQ(oa2dp_ring_writable(r), 8);
    CHECK_EQ(oa2dp_ring_write(r, in, 5), 5);
    CHECK_EQ(oa2dp_ring_readable(r), 5);
    CHECK_EQ(oa2dp_ring_read(r, out, 3), 3);
    CHECK_MEM(out, in, 3 * 2 * sizeof(int32_t));

    /* Wrap around the end of the buffer. */
    CHECK_EQ(oa2dp_ring_write(r, in + 10, 6), 6);
    CHECK_EQ(oa2dp_ring_readable(r), 8);
    CHECK_EQ(oa2dp_ring_write(r, in, 1), 0); /* full: dropped */
    CHECK_EQ(r->overruns, 1);
    CHECK_EQ(oa2dp_ring_read(r, out, 16), 8);
    CHECK_MEM(out, in + 6, 2 * 2 * sizeof(int32_t));     /* frames 3,4 of first write */
    CHECK_MEM(out + 4, in + 10, 6 * 2 * sizeof(int32_t)); /* the wrapped write */
    CHECK_EQ(oa2dp_ring_readable(r), 0);
    CHECK_EQ(oa2dp_ring_read(r, out, 1), 0);

    /* Corrupted header from the other side is rejected. */
    r->capacity = 1000;
    CHECK_EQ(oa2dp_ring_validate(r, bytes), OA2DP_ERR_PROTO);
    r->capacity = 1u << 20;
    CHECK_EQ(oa2dp_ring_validate(r, bytes), OA2DP_ERR_PROTO);
    free(r);

    float f[5] = {0.0f, 1.0f, -1.0f, 2.0f, 0.5f};
    int32_t s[5];
    oa2dp_pcm_float_to_s24(f, s, 5);
    CHECK_EQ(s[0], 0);
    CHECK_EQ(s[1], 8388607);
    CHECK_EQ(s[2], -8388608);
    CHECK_EQ(s[3], 8388607);
    CHECK_EQ(s[4], 4194304);

    int32_t st[4] = {100, 300, -10, 10}, mono[2];
    oa2dp_pcm_downmix_to_mono(st, mono, 2);
    CHECK_EQ(mono[0], 200);
    CHECK_EQ(mono[1], 0);
}
