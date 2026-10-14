/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_TEST_H
#define OA2DP_TEST_H

#include <stdio.h>
#include <string.h>

extern int g_failures;
extern int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        long long va_ = (long long)(a), vb_ = (long long)(b);                  \
        g_checks++;                                                            \
        if (va_ != vb_) {                                                      \
            g_failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld != %lld)\n", \
                    __FILE__, __LINE__, #a, #b, va_, vb_);                     \
        }                                                                      \
    } while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

void test_codecs(void);
void test_avdtp(void);
void test_media(void);
void test_settings(void);
void test_ring(void);

#endif
