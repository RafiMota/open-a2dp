/* SPDX-License-Identifier: MIT */
#include "test.h"

int g_failures;
int g_checks;

int main(void)
{
    test_codecs();
    test_avdtp();
    test_media();
    test_settings();
    test_ring();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
