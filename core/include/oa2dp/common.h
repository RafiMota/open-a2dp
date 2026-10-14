/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_COMMON_H
#define OA2DP_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes shared by every core module. Negative values are errors. */
enum {
    OA2DP_OK = 0,
    OA2DP_ERR_INVAL = -1,       /* bad argument or malformed input */
    OA2DP_ERR_NOSPC = -2,       /* output buffer too small / table full */
    OA2DP_ERR_PROTO = -3,       /* peer violated the protocol */
    OA2DP_ERR_UNSUPPORTED = -4, /* valid but not supported by us */
    OA2DP_ERR_STATE = -5,       /* operation not allowed in current state */
    OA2DP_ERR_BUSY = -6,        /* no free transaction label */
    OA2DP_ERR_NOTFOUND = -7,
    OA2DP_ERR_IO = -8,          /* transport send callback failed */
};

const char *oa2dp_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
