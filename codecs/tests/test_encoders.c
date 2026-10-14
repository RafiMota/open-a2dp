/* SPDX-License-Identifier: MIT */
/* Smoke test for every encoder adapter compiled into this build: feed one
 * second of a 1 kHz tone and check that well-formed frames come out. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "oa2dp/encoder.h"
#include "oa2dp/media.h"

static int failures;

#define EXPECT(c)                                                            \
    do {                                                                     \
        if (!(c)) {                                                          \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                    \
    } while (0)

static void run(oa2dp_codec_id id, const uint8_t *remote_ie, size_t ie_len)
{
    oa2dp_codec_caps remote, cfg;
    oa2dp_settings s;
    oa2dp_encoder *enc = NULL;

    if (!oa2dp_encoder_available(id)) {
        printf("%-18s skipped (not compiled in)\n", oa2dp_codec_name(id));
        return;
    }
    oa2dp_settings_default(&s);
    EXPECT(oa2dp_codec_parse(remote_ie, ie_len, &remote) == OA2DP_OK);
    EXPECT(oa2dp_codec_select(NULL, &remote, &s, &cfg) == OA2DP_OK);
    int r = oa2dp_encoder_create(&cfg, &s, 895, &enc);
    if (r != OA2DP_OK) {
        printf("%-18s create failed: %s\n", oa2dp_codec_name(id), oa2dp_strerror(r));
        failures++;
        return;
    }

    const oa2dp_encoder_info *in = &enc->info;
    size_t n = (size_t)in->samples_per_call * in->channels;
    int32_t *pcm = malloc(n * sizeof(*pcm));
    uint8_t *out = malloc(in->max_output);
    uint32_t calls = in->sample_rate / in->samples_per_call;
    size_t total = 0, frames_total = 0;
    double phase = 0, step = 2 * 3.14159265358979 * 1000 / in->sample_rate;

    for (uint32_t c = 0; c < calls; c++) {
        for (uint32_t i = 0; i < in->samples_per_call; i++) {
            int32_t v = (int32_t)(sin(phase) * 4000000.0);
            phase += step;
            for (uint8_t ch = 0; ch < in->channels; ch++)
                pcm[i * in->channels + ch] = v;
        }
        size_t len = 0;
        uint8_t frames = 0;
        r = oa2dp_encoder_encode(enc, pcm, out, in->max_output, &len, &frames);
        EXPECT(r == OA2DP_OK);
        if (r != OA2DP_OK)
            break;
        EXPECT(len <= in->max_output);
        if (id == OA2DP_CODEC_SBC && len)
            EXPECT(out[0] == 0x9C); /* SBC syncword */
        if (id == OA2DP_CODEC_AAC && len)
            EXPECT((out[0] & 0x80) == 0); /* LATM useSameStreamMux = 0 */
        total += len;
        frames_total += frames;
    }
    double kbps = total * 8.0 / 1000.0;
    printf("%-18s %u Hz %u ch: %zu frames, %.0f kbit/s\n", oa2dp_codec_name(id),
           (unsigned)in->sample_rate, in->channels, frames_total, kbps);
    EXPECT(frames_total > 0);
    EXPECT(total > 0);
    free(pcm);
    free(out);
    oa2dp_encoder_destroy(enc);
}

int main(void)
{
    static const uint8_t sbc[] = {0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35};
    static const uint8_t aac[] = {0x00, 0x02, 0x80, 0x01, 0x8C, 0x84, 0xE2, 0x00};
    static const uint8_t aptx[] = {0x00, 0xFF, 0x4F, 0, 0, 0, 0x01, 0x00, 0x32};
    static const uint8_t aptx_hd[] = {0x00, 0xFF, 0xD7, 0, 0, 0, 0x24, 0x00, 0x32, 0, 0, 0, 0};
    static const uint8_t ldac[] = {0x00, 0xFF, 0x2D, 0x01, 0, 0, 0xAA, 0x00, 0x3C, 0x07};
    static const uint8_t fs[] = {0x00, 0xFF, 0x0A, 0, 0, 0, 0x01, 0x00, 0x03, 0x23};

    run(OA2DP_CODEC_SBC, sbc, sizeof(sbc));
    run(OA2DP_CODEC_AAC, aac, sizeof(aac));
    run(OA2DP_CODEC_APTX, aptx, sizeof(aptx));
    run(OA2DP_CODEC_APTX_HD, aptx_hd, sizeof(aptx_hd));
    run(OA2DP_CODEC_LDAC, ldac, sizeof(ldac));
    run(OA2DP_CODEC_FASTSTREAM, fs, sizeof(fs));
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
