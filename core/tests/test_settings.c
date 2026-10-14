/* SPDX-License-Identifier: MIT */
#include "oa2dp/settings.h"
#include "test.h"

void test_settings(void)
{
    oa2dp_settings s, t;
    char buf[1024];
    int bad = 0;

    oa2dp_settings_default(&s);
    CHECK_EQ(s.priority_count, 5);
    CHECK_EQ(s.priority[0], OA2DP_CODEC_LDAC);
    CHECK_EQ(s.priority[4], OA2DP_CODEC_SBC);
    CHECK(s.adaptive_bitrate);

    int n = oa2dp_settings_format(&s, buf, sizeof(buf));
    CHECK(n > 0);
    oa2dp_settings_default(&t);
    t.sbc_bitpool = 99;
    CHECK_EQ(oa2dp_settings_parse(&t, buf, &bad), OA2DP_OK);
    CHECK(memcmp(&s, &t, sizeof(s)) == 0);

    const char *text = "# user file\r\n"
                       "codecs = aptx_ll, sbc\r\n"
                       "sample_rate=44100\n"
                       "channel_mode=dual\n"
                       "sbc_bitpool=47\n"
                       "sbc_blocks=16\n"
                       "sbc_subbands=8\n"
                       "sbc_allocation=snr\n"
                       "ldac_quality=hq\n"
                       "aac_bitrate=256000\n"
                       "aac_vbr=on\n"
                       "adaptive_bitrate=false\n"
                       "future_option=42\n";
    oa2dp_settings_default(&s);
    CHECK_EQ(oa2dp_settings_parse(&s, text, &bad), OA2DP_OK);
    CHECK_EQ(s.priority_count, 2);
    CHECK_EQ(s.priority[0], OA2DP_CODEC_APTX_LL);
    CHECK_EQ(s.sample_rate, 44100);
    CHECK_EQ(s.chmode, OA2DP_CHMODE_DUAL);
    CHECK_EQ(s.sbc_bitpool, 47);
    CHECK_EQ(s.sbc_alloc, OA2DP_SBC_ALLOC_SNR);
    CHECK_EQ(s.ldac_quality, OA2DP_LDAC_HQ);
    CHECK_EQ(s.aac_bitrate, 256000);
    CHECK(s.aac_vbr);
    CHECK(!s.adaptive_bitrate);

    CHECK_EQ(oa2dp_settings_parse(&s, "bits=24\nsample_rate=12345\n", &bad), OA2DP_ERR_INVAL);
    CHECK_EQ(bad, 2);
    CHECK_EQ(oa2dp_settings_parse(&s, "codecs=sbc,sbc\n", &bad), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_settings_parse(&s, "codecs=\n", &bad), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_settings_parse(&s, "codecs=opus\n", &bad), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_settings_parse(&s, "sbc_blocks=10\n", &bad), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_settings_parse(&s, "no equals sign\n", &bad), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_settings_parse(&s, "sbc_bitpool=251\n", &bad), OA2DP_ERR_INVAL);

    CHECK_EQ(oa2dp_settings_format(&s, buf, 20), OA2DP_ERR_NOSPC);
    CHECK_EQ(oa2dp_codec_from_key("aptx_hd", 7), OA2DP_CODEC_APTX_HD);
    CHECK_EQ(oa2dp_codec_from_key("aptx", 4), OA2DP_CODEC_APTX);
}
