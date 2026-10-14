/* SPDX-License-Identifier: MIT */
#include "oa2dp/a2dp_codecs.h"
#include "test.h"

static void sbc(void)
{
    oa2dp_codec_caps local, remote, cfg;
    oa2dp_settings s;
    uint8_t buf[OA2DP_CODEC_IE_MAX];

    oa2dp_codec_local_caps(OA2DP_CODEC_SBC, &local);
    int n = oa2dp_codec_build(&local, buf, sizeof(buf));
    const uint8_t want_local[] = {0x00, 0x00, 0xFF, 0xFF, 2, 250};
    CHECK_EQ(n, 6);
    CHECK_MEM(buf, want_local, 6);

    /* Typical headset: everything, bitpool 2..53. */
    const uint8_t hs[] = {0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35};
    CHECK_EQ(oa2dp_codec_parse(hs, sizeof(hs), &remote), OA2DP_OK);
    CHECK_EQ(remote.codec, OA2DP_CODEC_SBC);
    CHECK_EQ(remote.u.sbc.max_bitpool, 53);

    oa2dp_settings_default(&s);
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.sbc.freq, OA2DP_SBC_FREQ_48000);
    CHECK_EQ(cfg.u.sbc.chmode, OA2DP_SBC_CH_JOINT);
    CHECK_EQ(cfg.u.sbc.blocks, OA2DP_SBC_BLOCKS_16);
    CHECK_EQ(cfg.u.sbc.subbands, OA2DP_SBC_SUBBANDS_8);
    CHECK_EQ(cfg.u.sbc.alloc, OA2DP_SBC_ALLOC_LOUDNESS_BIT);
    CHECK_EQ(cfg.u.sbc.min_bitpool, 2);
    CHECK_EQ(cfg.u.sbc.max_bitpool, 51);
    CHECK_EQ(oa2dp_codec_sample_rate(&cfg), 48000);
    CHECK_EQ(oa2dp_codec_channels(&cfg), 2);
    CHECK_EQ(oa2dp_codec_check_config(&local, &cfg), 0);

    s.sample_rate = 44100;
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.sbc.max_bitpool, 53);

    /* Dual channel "XQ" with explicit bitpool clamped by headset max. */
    s.chmode = OA2DP_CHMODE_DUAL;
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.sbc.chmode, OA2DP_SBC_CH_DUAL);
    CHECK_EQ(cfg.u.sbc.max_bitpool, 38);
    s.sbc_bitpool = 60;
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.sbc.max_bitpool, 53);

    /* Explicit block / subband / allocation choices. */
    s.sbc_blocks = 8;
    s.sbc_subbands = 4;
    s.sbc_alloc = OA2DP_SBC_ALLOC_SNR;
    s.chmode = OA2DP_CHMODE_MONO;
    s.sbc_bitpool = 0;
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.sbc.blocks, OA2DP_SBC_BLOCKS_8);
    CHECK_EQ(cfg.u.sbc.subbands, OA2DP_SBC_SUBBANDS_4);
    CHECK_EQ(cfg.u.sbc.alloc, OA2DP_SBC_ALLOC_SNR_BIT);
    CHECK_EQ(cfg.u.sbc.chmode, OA2DP_SBC_CH_MONO);
    CHECK_EQ(cfg.u.sbc.max_bitpool, 31);
    CHECK_EQ(oa2dp_codec_channels(&cfg), 1);

    /* Headset that only allows mono: stereo request falls back. */
    remote.u.sbc.chmode = OA2DP_SBC_CH_MONO;
    s.chmode = OA2DP_CHMODE_JOINT;
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.sbc.chmode, OA2DP_SBC_CH_MONO);

    /* Validation of peer configurations. */
    oa2dp_settings_default(&s);
    oa2dp_codec_parse(hs, sizeof(hs), &remote);
    oa2dp_codec_select(NULL, &remote, &s, &cfg);
    oa2dp_codec_caps bad = cfg;
    bad.u.sbc.freq = OA2DP_SBC_FREQ_44100 | OA2DP_SBC_FREQ_48000;
    CHECK_EQ(oa2dp_codec_check_config(&local, &bad), OA2DP_A2DP_INVALID_SAMPLING_FREQUENCY);
    bad = cfg;
    bad.u.sbc.max_bitpool = 251;
    CHECK_EQ(oa2dp_codec_check_config(&local, &bad), OA2DP_A2DP_INVALID_MAXIMUM_BITPOOL_VALUE);
    bad = cfg;
    bad.u.sbc.min_bitpool = 1;
    CHECK_EQ(oa2dp_codec_check_config(&local, &bad), OA2DP_A2DP_INVALID_MINIMUM_BITPOOL_VALUE);
    bad = cfg;
    bad.u.sbc.chmode = 0;
    CHECK_EQ(oa2dp_codec_check_config(&local, &bad), OA2DP_A2DP_INVALID_CHANNEL_MODE);
    bad = cfg;
    bad.codec = OA2DP_CODEC_AAC;
    CHECK_EQ(oa2dp_codec_check_config(&local, &bad), OA2DP_A2DP_NOT_SUPPORTED_CODEC_TYPE);

    /* Truncated element. */
    CHECK_EQ(oa2dp_codec_parse(hs, 4, &remote), OA2DP_ERR_INVAL);
}

static void aac(void)
{
    oa2dp_codec_caps remote, cfg, local;
    oa2dp_settings s;
    uint8_t buf[OA2DP_CODEC_IE_MAX];

    /* MPEG-2 LC, 44.1+48 kHz, 2 ch, VBR, 320 kbit/s */
    const uint8_t hs[] = {0x00, 0x02, 0x80, 0x01, 0x8C, 0x84, 0xE2, 0x00};
    CHECK_EQ(oa2dp_codec_parse(hs, sizeof(hs), &remote), OA2DP_OK);
    CHECK_EQ(remote.codec, OA2DP_CODEC_AAC);
    CHECK_EQ(remote.u.aac.object_type, OA2DP_AAC_OBJ_MPEG2_LC);
    CHECK_EQ(remote.u.aac.freq, OA2DP_AAC_FREQ_44100 | OA2DP_AAC_FREQ_48000);
    CHECK_EQ(remote.u.aac.channels, OA2DP_AAC_CH_1 | OA2DP_AAC_CH_2);
    CHECK(remote.u.aac.vbr);
    CHECK_EQ(remote.u.aac.bitrate, 320000);

    int n = oa2dp_codec_build(&remote, buf, sizeof(buf));
    CHECK_EQ(n, 8);
    CHECK_MEM(buf, hs, 8);

    oa2dp_settings_default(&s);
    s.aac_vbr = true; /* local MF encoder is CBR: must stay off */
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.aac.object_type, OA2DP_AAC_OBJ_MPEG2_LC);
    CHECK_EQ(cfg.u.aac.freq, OA2DP_AAC_FREQ_48000);
    CHECK_EQ(cfg.u.aac.channels, OA2DP_AAC_CH_2);
    CHECK(!cfg.u.aac.vbr);
    CHECK_EQ(cfg.u.aac.bitrate, 192000);
    n = oa2dp_codec_build(&cfg, buf, sizeof(buf));
    const uint8_t want[] = {0x00, 0x02, 0x80, 0x00, 0x84, 0x02, 0xEE, 0x00};
    CHECK_EQ(n, 8);
    CHECK_MEM(buf, want, 8);

    s.aac_bitrate = 128000;
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.aac.bitrate, 128000);

    oa2dp_codec_local_caps(OA2DP_CODEC_AAC, &local);
    cfg.u.aac.vbr = true;
    CHECK_EQ(oa2dp_codec_check_config(&local, &cfg), OA2DP_A2DP_NOT_SUPPORTED_VBR);
}

static void vendor(void)
{
    oa2dp_codec_caps remote, cfg, back;
    oa2dp_settings s;
    uint8_t buf[OA2DP_CODEC_IE_MAX];
    oa2dp_settings_default(&s);

    const uint8_t aptx[] = {0x00, 0xFF, 0x4F, 0, 0, 0, 0x01, 0x00, 0x32};
    CHECK_EQ(oa2dp_codec_parse(aptx, sizeof(aptx), &remote), OA2DP_OK);
    CHECK_EQ(remote.codec, OA2DP_CODEC_APTX);
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.aptx.freq, OA2DP_APTX_FREQ_48000);
    CHECK_EQ(cfg.u.aptx.chmode, OA2DP_APTX_CH_STEREO);
    CHECK_EQ(oa2dp_codec_build(&cfg, buf, sizeof(buf)), 9);
    CHECK_EQ(buf[8], 0x12);

    const uint8_t hd[] = {0x00, 0xFF, 0xD7, 0, 0, 0, 0x24, 0x00, 0x32, 0, 0, 0, 0};
    CHECK_EQ(oa2dp_codec_parse(hd, sizeof(hd), &remote), OA2DP_OK);
    CHECK_EQ(remote.codec, OA2DP_CODEC_APTX_HD);
    CHECK_EQ(oa2dp_codec_build(&remote, buf, sizeof(buf)), 13);
    CHECK_MEM(buf, hd, 13);
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(oa2dp_codec_pcm_bits(&cfg, &s), 24);
    /* aptX HD element shorter than 13 bytes is malformed */
    CHECK_EQ(oa2dp_codec_parse(hd, 9, &remote), OA2DP_ERR_INVAL);

    const uint8_t ldac[] = {0x00, 0xFF, 0x2D, 0x01, 0, 0, 0xAA, 0x00, 0x3C, 0x07};
    CHECK_EQ(oa2dp_codec_parse(ldac, sizeof(ldac), &remote), OA2DP_OK);
    CHECK_EQ(remote.codec, OA2DP_CODEC_LDAC);
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.ldac.freq, OA2DP_LDAC_FREQ_48000);
    CHECK_EQ(cfg.u.ldac.chmode, OA2DP_LDAC_CH_STEREO);
    s.sample_rate = 96000;
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(oa2dp_codec_sample_rate(&cfg), 96000);
    s.sample_rate = 0;
    CHECK_EQ(oa2dp_codec_set_rate(&cfg, &remote, 44100), OA2DP_OK);
    CHECK_EQ(oa2dp_codec_sample_rate(&cfg), 44100);
    CHECK_EQ(oa2dp_codec_set_rate(&cfg, &remote, 192000), OA2DP_ERR_UNSUPPORTED);
    CHECK_EQ(oa2dp_codec_sample_rate(&cfg), 44100);

    /* aptX LL with "new caps" block round-trips. */
    oa2dp_codec_caps ll = {0};
    ll.codec = OA2DP_CODEC_APTX_LL;
    ll.u.aptx_ll.freq = OA2DP_APTX_FREQ_48000;
    ll.u.aptx_ll.chmode = OA2DP_APTX_CH_STEREO;
    ll.u.aptx_ll.has_new_caps = true;
    ll.u.aptx_ll.target_level = 180;
    ll.u.aptx_ll.initial_level = 240;
    ll.u.aptx_ll.sra_max_rate = 50;
    ll.u.aptx_ll.sra_avg_time = 1;
    ll.u.aptx_ll.good_working_level = 180;
    int n = oa2dp_codec_build(&ll, buf, sizeof(buf));
    CHECK_EQ(n, 19);
    CHECK_EQ(oa2dp_codec_parse(buf, (size_t)n, &back), OA2DP_OK);
    CHECK_EQ(back.codec, OA2DP_CODEC_APTX_LL);
    CHECK_EQ(back.u.aptx_ll.initial_level, 240);
    CHECK_EQ(back.u.aptx_ll.good_working_level, 180);
    CHECK_EQ(oa2dp_codec_select(NULL, &back, &s, &cfg), OA2DP_OK);
    CHECK(cfg.u.aptx_ll.has_new_caps);
    CHECK_EQ(cfg.u.aptx_ll.target_level, 180);

    const uint8_t fs[] = {0x00, 0xFF, 0x0A, 0, 0, 0, 0x01, 0x00, 0x03, 0x23};
    CHECK_EQ(oa2dp_codec_parse(fs, sizeof(fs), &remote), OA2DP_OK);
    CHECK_EQ(remote.codec, OA2DP_CODEC_FASTSTREAM);
    CHECK_EQ(remote.u.fs.source_freq, OA2DP_FS_SOURCE_FREQ_16000);
    CHECK_EQ(oa2dp_codec_select(NULL, &remote, &s, &cfg), OA2DP_OK);
    CHECK_EQ(cfg.u.fs.direction, OA2DP_FS_DIR_SINK);
    CHECK_EQ(cfg.u.fs.sink_freq, OA2DP_FS_SINK_FREQ_48000);

    const uint8_t unknown[] = {0x00, 0xFF, 0x34, 0x12, 0, 0, 0x01, 0x00, 0x00};
    CHECK_EQ(oa2dp_codec_parse(unknown, sizeof(unknown), &remote), OA2DP_ERR_UNSUPPORTED);
}

static void best(void)
{
    oa2dp_codec_caps list[4], cfg;
    oa2dp_settings s;
    const uint8_t sbc[] = {0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35};
    const uint8_t aac[] = {0x00, 0x02, 0x80, 0x01, 0x8C, 0x84, 0xE2, 0x00};
    const uint8_t aptx[] = {0x00, 0xFF, 0x4F, 0, 0, 0, 0x01, 0x00, 0x32};
    const uint8_t ldac[] = {0x00, 0xFF, 0x2D, 0x01, 0, 0, 0xAA, 0x00, 0x3C, 0x07};
    oa2dp_codec_parse(sbc, sizeof(sbc), &list[0]);
    oa2dp_codec_parse(aac, sizeof(aac), &list[1]);
    oa2dp_codec_parse(aptx, sizeof(aptx), &list[2]);
    oa2dp_codec_parse(ldac, sizeof(ldac), &list[3]);

    oa2dp_settings_default(&s);
    CHECK_EQ(oa2dp_codec_choose_best(list, 4, &s, &cfg), 3);
    CHECK_EQ(cfg.codec, OA2DP_CODEC_LDAC);

    s.priority[0] = OA2DP_CODEC_AAC;
    s.priority[1] = OA2DP_CODEC_SBC;
    s.priority_count = 2;
    CHECK_EQ(oa2dp_codec_choose_best(list, 4, &s, &cfg), 1);
    CHECK_EQ(oa2dp_codec_choose_best(list, 1, &s, &cfg), 0);

    s.priority[0] = OA2DP_CODEC_APTX_HD;
    s.priority_count = 1;
    CHECK_EQ(oa2dp_codec_choose_best(list, 4, &s, &cfg), OA2DP_ERR_NOTFOUND);
}

void test_codecs(void)
{
    sbc();
    aac();
    vendor();
    best();
}
