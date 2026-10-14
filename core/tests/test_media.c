/* SPDX-License-Identifier: MIT */
#include "oa2dp/media.h"
#include "test.h"

static void sbc_math(void)
{
    oa2dp_sbc_layout l;
    oa2dp_sbc_caps c = {OA2DP_SBC_FREQ_44100, OA2DP_SBC_CH_JOINT, OA2DP_SBC_BLOCKS_16,
                        OA2DP_SBC_SUBBANDS_8, OA2DP_SBC_ALLOC_LOUDNESS_BIT, 2, 53};
    CHECK_EQ(oa2dp_sbc_layout_from_config(&c, &l), OA2DP_OK);
    CHECK_EQ(oa2dp_sbc_frame_len(&l, 53), 119);
    CHECK_EQ(oa2dp_sbc_bitrate(&l, 53, 44100), 327993);
    CHECK_EQ(oa2dp_sbc_bitpool_for_bitrate(&l, 44100, 328000, 2, 250), 53);

    c.chmode = OA2DP_SBC_CH_DUAL;
    oa2dp_sbc_layout_from_config(&c, &l);
    CHECK_EQ(oa2dp_sbc_frame_len(&l, 47), 200);
    CHECK_EQ(oa2dp_sbc_bitrate(&l, 47, 44100), 551250);

    c.chmode = OA2DP_SBC_CH_STEREO;
    oa2dp_sbc_layout_from_config(&c, &l);
    CHECK_EQ(oa2dp_sbc_frame_len(&l, 53), 118);

    c.chmode = OA2DP_SBC_CH_MONO;
    c.subbands = OA2DP_SBC_SUBBANDS_4;
    c.blocks = OA2DP_SBC_BLOCKS_8;
    oa2dp_sbc_layout_from_config(&c, &l);
    CHECK_EQ(oa2dp_sbc_frame_len(&l, 31), 37);

    c.blocks = 0x30; /* two bits: invalid */
    CHECK_EQ(oa2dp_sbc_layout_from_config(&c, &l), OA2DP_ERR_INVAL);

    CHECK_EQ(oa2dp_sbc_abr_bitpool(2, 53, 0, 5), 53);
    CHECK_EQ(oa2dp_sbc_abr_bitpool(2, 53, 4, 5), 26);
    CHECK_EQ(oa2dp_sbc_abr_bitpool(2, 53, 2, 5), 39);
    CHECK_EQ(oa2dp_sbc_abr_bitpool(40, 53, 4, 5), 40);
}

static void rtp(void)
{
    uint8_t buf[2048], frame[300];
    oa2dp_rtp r;
    memset(frame, 0x5A, sizeof(frame));

    CHECK_EQ(oa2dp_rtp_header_len(OA2DP_CODEC_SBC), 13);
    CHECK_EQ(oa2dp_rtp_header_len(OA2DP_CODEC_LDAC), 13);
    CHECK_EQ(oa2dp_rtp_header_len(OA2DP_CODEC_AAC), 12);
    CHECK_EQ(oa2dp_rtp_header_len(OA2DP_CODEC_APTX_HD), 12);
    CHECK_EQ(oa2dp_rtp_header_len(OA2DP_CODEC_APTX), 0);
    CHECK_EQ(oa2dp_rtp_header_len(OA2DP_CODEC_APTX_LL), 0);
    CHECK_EQ(oa2dp_rtp_header_len(OA2DP_CODEC_FASTSTREAM), 0);

    /* SBC: three 119-byte frames fill a 370-byte MTU exactly. */
    CHECK_EQ(oa2dp_rtp_init(&r, OA2DP_CODEC_SBC, buf, 13 + 3 * 119, 0x11223344), OA2DP_OK);
    uint16_t seq0 = r.seq;
    for (int i = 0; i < 3; i++)
        CHECK_EQ(oa2dp_rtp_add_frame(&r, frame, 119, 128), 1);
    CHECK_EQ(oa2dp_rtp_add_frame(&r, frame, 119, 128), 0);
    CHECK_EQ(oa2dp_rtp_flush(&r), 370);
    CHECK_EQ(buf[0], 0x80);
    CHECK_EQ(buf[1], OA2DP_RTP_PAYLOAD_TYPE);
    CHECK_EQ((buf[2] << 8) | buf[3], seq0);
    CHECK_EQ(buf[4] | buf[5] | buf[6] | buf[7], 0); /* timestamp 0 */
    CHECK_EQ(buf[8], 0x11);
    CHECK_EQ(buf[11], 0x44);
    CHECK_EQ(buf[12], 3);
    CHECK_EQ(r.timestamp, 384);
    CHECK_EQ(oa2dp_rtp_flush(&r), 0);

    oa2dp_rtp_add_frame(&r, frame, 119, 128);
    CHECK_EQ(oa2dp_rtp_flush(&r), 13 + 119);
    CHECK_EQ((buf[2] << 8) | buf[3], (uint16_t)(seq0 + 1));
    CHECK_EQ((buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7], 384);

    /* The 4-bit frame counter limits a packet to 15 frames. */
    oa2dp_rtp_init(&r, OA2DP_CODEC_SBC, buf, sizeof(buf), 1);
    for (int i = 0; i < 15; i++)
        CHECK_EQ(oa2dp_rtp_add_frame(&r, frame, 10, 128), 1);
    CHECK_EQ(oa2dp_rtp_add_frame(&r, frame, 10, 128), 0);
    oa2dp_rtp_flush(&r);
    CHECK_EQ(buf[12], 15);

    /* Fragmentation of an oversized SBC frame. */
    oa2dp_rtp_init(&r, OA2DP_CODEC_SBC, buf, 13 + 100, 1);
    CHECK_EQ(oa2dp_rtp_add_frame(&r, frame, 250, 128), OA2DP_ERR_NOSPC);
    size_t off = 0;
    uint32_t ts = r.timestamp;
    CHECK_EQ(oa2dp_rtp_fragment(&r, frame, 250, 128, &off), 113);
    CHECK_EQ(buf[12], 0xC3);
    CHECK_EQ(oa2dp_rtp_fragment(&r, frame, 250, 128, &off), 113);
    CHECK_EQ(buf[12], 0x82);
    CHECK_EQ(r.timestamp, ts);
    CHECK_EQ(oa2dp_rtp_fragment(&r, frame, 250, 128, &off), 13 + 50);
    CHECK_EQ(buf[12], 0xA1);
    CHECK_EQ(off, 250);
    CHECK_EQ(r.timestamp, ts + 128);
    CHECK_EQ(oa2dp_rtp_fragment(&r, frame, 250, 128, &off), OA2DP_ERR_STATE);

    /* aptX: raw stream, no header. */
    oa2dp_rtp_init(&r, OA2DP_CODEC_APTX, buf, 100, 1);
    CHECK_EQ(oa2dp_rtp_add_frame(&r, frame, 64, 64), 1);
    CHECK_EQ(oa2dp_rtp_flush(&r), 64);
    CHECK_EQ(buf[0], 0x5A);

    /* AAC: marker bit on complete AudioMuxElements. */
    oa2dp_rtp_init(&r, OA2DP_CODEC_AAC, buf, 800, 1);
    oa2dp_rtp_add_frame(&r, frame, 200, 1024);
    CHECK_EQ(oa2dp_rtp_flush(&r), 212);
    CHECK_EQ(buf[1], 0x80 | OA2DP_RTP_PAYLOAD_TYPE);
    oa2dp_rtp_init(&r, OA2DP_CODEC_AAC, buf, 112, 1);
    off = 0;
    oa2dp_rtp_fragment(&r, frame, 150, 1024, &off);
    CHECK_EQ(buf[1], OA2DP_RTP_PAYLOAD_TYPE);
    oa2dp_rtp_fragment(&r, frame, 150, 1024, &off);
    CHECK_EQ(buf[1], 0x80 | OA2DP_RTP_PAYLOAD_TYPE);

    /* LDAC: frame count header. */
    oa2dp_rtp_init(&r, OA2DP_CODEC_LDAC, buf, 700, 1);
    oa2dp_rtp_add_frame(&r, frame, 165, 128);
    oa2dp_rtp_add_frame(&r, frame, 165, 128);
    CHECK_EQ(oa2dp_rtp_flush(&r), 13 + 330);
    CHECK_EQ(buf[12], 2);
}

typedef struct {
    const uint8_t *p;
    size_t bit;
} bitr;

static uint32_t get_bits(bitr *r, unsigned n)
{
    uint32_t v = 0;
    while (n--) {
        v = (v << 1) | ((r->p[r->bit >> 3] >> (7 - (r->bit & 7))) & 1);
        r->bit++;
    }
    return v;
}

static void latm(void)
{
    uint8_t au[300], out[400];
    for (int i = 0; i < 300; i++)
        au[i] = (uint8_t)i;
    int n = oa2dp_latm_write(out, sizeof(out), au, sizeof(au), 44100, 2);
    CHECK(n > 0);

    bitr r = {out, 0};
    CHECK_EQ(get_bits(&r, 1), 0);  /* useSameStreamMux */
    CHECK_EQ(get_bits(&r, 1), 0);  /* audioMuxVersion */
    CHECK_EQ(get_bits(&r, 1), 1);  /* allStreamsSameTimeFraming */
    CHECK_EQ(get_bits(&r, 6), 0);
    CHECK_EQ(get_bits(&r, 4), 0);
    CHECK_EQ(get_bits(&r, 3), 0);
    CHECK_EQ(get_bits(&r, 5), 2);  /* AAC LC */
    CHECK_EQ(get_bits(&r, 4), 4);  /* 44100 */
    CHECK_EQ(get_bits(&r, 4), 2);  /* stereo */
    CHECK_EQ(get_bits(&r, 3), 0);
    CHECK_EQ(get_bits(&r, 3), 0);  /* frameLengthType */
    CHECK_EQ(get_bits(&r, 8), 0xFF);
    CHECK_EQ(get_bits(&r, 1), 0);
    CHECK_EQ(get_bits(&r, 1), 0);
    CHECK_EQ(get_bits(&r, 8), 255);
    CHECK_EQ(get_bits(&r, 8), 45);
    bool same = true;
    for (int i = 0; i < 300; i++)
        if (get_bits(&r, 8) != au[i])
            same = false;
    CHECK(same);
    CHECK_EQ(n, (int)((r.bit + 7) / 8));

    CHECK_EQ(oa2dp_latm_write(out, 100, au, sizeof(au), 44100, 2), OA2DP_ERR_NOSPC);
    /* Non-standard rate uses the escape code. */
    n = oa2dp_latm_write(out, sizeof(out), au, 10, 50000, 1);
    r.p = out;
    r.bit = 16 + 5;
    CHECK_EQ(get_bits(&r, 4), 0xF);
    CHECK_EQ(get_bits(&r, 24), 50000);
}

static void abr(void)
{
    oa2dp_abr a;
    oa2dp_abr_init(&a, 4, 1000, 200, 500, 3000);
    CHECK_EQ(oa2dp_abr_update(&a, 1500, 0), -1);
    CHECK_EQ(oa2dp_abr_update(&a, 1500, 499), -1);
    CHECK_EQ(oa2dp_abr_update(&a, 1500, 500), 1);
    CHECK_EQ(oa2dp_abr_update(&a, 3000, 510), 2);  /* severe: immediate */
    CHECK_EQ(oa2dp_abr_update(&a, 500, 520), -1);  /* neutral */
    CHECK_EQ(oa2dp_abr_update(&a, 100, 600), -1);
    CHECK_EQ(oa2dp_abr_update(&a, 100, 3599), -1);
    CHECK_EQ(oa2dp_abr_update(&a, 100, 3600), 1);
    CHECK_EQ(oa2dp_abr_update(&a, 100, 6600), 0);
    CHECK_EQ(oa2dp_abr_update(&a, 100, 9600), -1); /* already best */
    for (uint32_t t = 10000; t < 20000; t += 600)
        oa2dp_abr_update(&a, 5000, t);
    CHECK_EQ(a.level, 3); /* never past the last level */
}

void test_media(void)
{
    sbc_math();
    rtp();
    latm();
    abr();
}
