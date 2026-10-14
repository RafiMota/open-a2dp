/* SPDX-License-Identifier: MIT */
#include "oa2dp/settings.h"

#include <stdio.h>
#include <string.h>

const char *oa2dp_strerror(int err)
{
    switch (err) {
    case OA2DP_OK: return "ok";
    case OA2DP_ERR_INVAL: return "invalid argument";
    case OA2DP_ERR_NOSPC: return "no space";
    case OA2DP_ERR_PROTO: return "protocol error";
    case OA2DP_ERR_UNSUPPORTED: return "not supported";
    case OA2DP_ERR_STATE: return "bad state";
    case OA2DP_ERR_BUSY: return "busy";
    case OA2DP_ERR_NOTFOUND: return "not found";
    case OA2DP_ERR_IO: return "I/O error";
    default: return "unknown error";
    }
}

static const char *const codec_keys[OA2DP_CODEC_COUNT] = {
    "sbc", "aac", "aptx", "aptx_hd", "aptx_ll", "faststream", "ldac"};

const char *oa2dp_codec_key(oa2dp_codec_id id)
{
    return (id >= 0 && id < OA2DP_CODEC_COUNT) ? codec_keys[id] : "unknown";
}

oa2dp_codec_id oa2dp_codec_from_key(const char *key, size_t len)
{
    for (int i = 0; i < OA2DP_CODEC_COUNT; i++)
        if (strlen(codec_keys[i]) == len && memcmp(codec_keys[i], key, len) == 0)
            return (oa2dp_codec_id)i;
    return OA2DP_CODEC_UNKNOWN;
}

void oa2dp_settings_default(oa2dp_settings *s)
{
    static const oa2dp_codec_id order[] = {
        OA2DP_CODEC_LDAC, OA2DP_CODEC_APTX_HD, OA2DP_CODEC_APTX,
        OA2DP_CODEC_AAC,  OA2DP_CODEC_SBC};
    memset(s, 0, sizeof(*s));
    memcpy(s->priority, order, sizeof(order));
    s->priority_count = (uint8_t)(sizeof(order) / sizeof(order[0]));
    s->adaptive_bitrate = true;
    s->follow_source_rate = true;
    s->ldac_quality = OA2DP_LDAC_ABR;
}

/* --- parsing ----------------------------------------------------------- */

typedef struct {
    const char *name;
    int value;
} enum_name;

static const enum_name chmode_names[] = {
    {"auto", OA2DP_CHMODE_AUTO},     {"mono", OA2DP_CHMODE_MONO},
    {"dual", OA2DP_CHMODE_DUAL},     {"stereo", OA2DP_CHMODE_STEREO},
    {"joint", OA2DP_CHMODE_JOINT},   {NULL, 0}};
static const enum_name alloc_names[] = {
    {"auto", OA2DP_SBC_ALLOC_AUTO}, {"loudness", OA2DP_SBC_ALLOC_LOUDNESS},
    {"snr", OA2DP_SBC_ALLOC_SNR},   {NULL, 0}};
static const enum_name ldac_names[] = {
    {"abr", OA2DP_LDAC_ABR}, {"hq", OA2DP_LDAC_HQ},
    {"sq", OA2DP_LDAC_SQ},   {"mq", OA2DP_LDAC_MQ}, {NULL, 0}};

static bool str_eq(const char *a, size_t alen, const char *b)
{
    return strlen(b) == alen && memcmp(a, b, alen) == 0;
}

static bool parse_enum(const enum_name *t, const char *v, size_t n, int *out)
{
    for (; t->name; t++)
        if (str_eq(v, n, t->name)) {
            *out = t->value;
            return true;
        }
    return false;
}

static const char *enum_str(const enum_name *t, int v)
{
    for (; t->name; t++)
        if (t->value == v)
            return t->name;
    return "auto";
}

static bool parse_uint(const char *v, size_t n, uint32_t max, uint32_t *out)
{
    uint32_t x = 0;
    if (n == 0 || n > 10)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return false;
        uint64_t next = (uint64_t)x * 10 + (uint32_t)(v[i] - '0');
        if (next > max)
            return false;
        x = (uint32_t)next;
    }
    *out = x;
    return true;
}

static bool parse_bool(const char *v, size_t n, bool *out)
{
    if (str_eq(v, n, "1") || str_eq(v, n, "true") || str_eq(v, n, "on")) {
        *out = true;
        return true;
    }
    if (str_eq(v, n, "0") || str_eq(v, n, "false") || str_eq(v, n, "off")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_priority(oa2dp_settings *s, const char *v, size_t n)
{
    oa2dp_codec_id list[OA2DP_CODEC_COUNT];
    uint8_t count = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && v[j] != ',')
            j++;
        size_t a = i, b = j;
        while (a < b && v[a] == ' ')
            a++;
        while (b > a && v[b - 1] == ' ')
            b--;
        if (b > a) {
            oa2dp_codec_id id = oa2dp_codec_from_key(v + a, b - a);
            if (id == OA2DP_CODEC_UNKNOWN)
                return false;
            for (uint8_t k = 0; k < count; k++)
                if (list[k] == id)
                    return false; /* duplicate */
            list[count++] = id;
        }
        i = j + 1;
    }
    if (count == 0)
        return false; /* at least one codec must stay enabled */
    memcpy(s->priority, list, count * sizeof(list[0]));
    s->priority_count = count;
    return true;
}

static bool apply(oa2dp_settings *s, const char *k, size_t kn, const char *v, size_t vn)
{
    uint32_t u;
    int e;
    if (str_eq(k, kn, "codecs"))
        return parse_priority(s, v, vn);
    if (str_eq(k, kn, "sample_rate")) {
        static const uint32_t ok[] = {0, 16000, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
        if (!parse_uint(v, vn, 192000, &u))
            return false;
        for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
            if (ok[i] == u) {
                s->sample_rate = u;
                return true;
            }
        return false;
    }
    if (str_eq(k, kn, "bits")) {
        if (!parse_uint(v, vn, 32, &u) || (u != 0 && u != 16 && u != 24))
            return false;
        s->bits = (uint8_t)u;
        return true;
    }
    if (str_eq(k, kn, "channel_mode")) {
        if (!parse_enum(chmode_names, v, vn, &e))
            return false;
        s->chmode = (oa2dp_chmode)e;
        return true;
    }
    if (str_eq(k, kn, "sbc_bitpool")) {
        if (!parse_uint(v, vn, 250, &u) || u == 1)
            return false;
        s->sbc_bitpool = (uint8_t)u;
        return true;
    }
    if (str_eq(k, kn, "sbc_blocks")) {
        if (!parse_uint(v, vn, 16, &u) || (u % 4 != 0))
            return false;
        s->sbc_blocks = (uint8_t)u;
        return true;
    }
    if (str_eq(k, kn, "sbc_subbands")) {
        if (!parse_uint(v, vn, 8, &u) || (u != 0 && u != 4 && u != 8))
            return false;
        s->sbc_subbands = (uint8_t)u;
        return true;
    }
    if (str_eq(k, kn, "sbc_allocation")) {
        if (!parse_enum(alloc_names, v, vn, &e))
            return false;
        s->sbc_alloc = (oa2dp_sbc_alloc)e;
        return true;
    }
    if (str_eq(k, kn, "aac_bitrate")) {
        if (!parse_uint(v, vn, 0x7FFFFF, &u))
            return false;
        s->aac_bitrate = u;
        return true;
    }
    if (str_eq(k, kn, "aac_vbr"))
        return parse_bool(v, vn, &s->aac_vbr);
    if (str_eq(k, kn, "ldac_quality")) {
        if (!parse_enum(ldac_names, v, vn, &e))
            return false;
        s->ldac_quality = (oa2dp_ldac_quality)e;
        return true;
    }
    if (str_eq(k, kn, "adaptive_bitrate"))
        return parse_bool(v, vn, &s->adaptive_bitrate);
    if (str_eq(k, kn, "follow_source_rate"))
        return parse_bool(v, vn, &s->follow_source_rate);
    return true; /* unknown key: ignore for forward compatibility */
}

int oa2dp_settings_parse(oa2dp_settings *s, const char *text, int *bad_line)
{
    int line = 0;
    if (!s || !text)
        return OA2DP_ERR_INVAL;
    const char *p = text;
    while (*p) {
        line++;
        const char *end = p;
        while (*end && *end != '\n')
            end++;
        const char *a = p, *b = end;
        while (a < b && (*a == ' ' || *a == '\t'))
            a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r'))
            b--;
        if (a < b && *a != '#' && *a != ';') {
            const char *eq = memchr(a, '=', (size_t)(b - a));
            if (!eq)
                goto bad;
            const char *ke = eq, *vs = eq + 1;
            while (ke > a && (ke[-1] == ' ' || ke[-1] == '\t'))
                ke--;
            while (vs < b && (*vs == ' ' || *vs == '\t'))
                vs++;
            if (!apply(s, a, (size_t)(ke - a), vs, (size_t)(b - vs)))
                goto bad;
        }
        p = *end ? end + 1 : end;
    }
    return OA2DP_OK;
bad:
    if (bad_line)
        *bad_line = line;
    return OA2DP_ERR_INVAL;
}

int oa2dp_settings_format(const oa2dp_settings *s, char *buf, size_t cap)
{
    char codecs[96] = "";
    size_t off = 0;
    for (uint8_t i = 0; i < s->priority_count; i++) {
        int n = snprintf(codecs + off, sizeof(codecs) - off, "%s%s", i ? "," : "",
                         oa2dp_codec_key(s->priority[i]));
        if (n < 0 || (size_t)n >= sizeof(codecs) - off)
            return OA2DP_ERR_NOSPC;
        off += (size_t)n;
    }
    int n = snprintf(buf, cap,
                     "codecs=%s\n"
                     "sample_rate=%u\n"
                     "bits=%u\n"
                     "channel_mode=%s\n"
                     "sbc_bitpool=%u\n"
                     "sbc_blocks=%u\n"
                     "sbc_subbands=%u\n"
                     "sbc_allocation=%s\n"
                     "aac_bitrate=%u\n"
                     "aac_vbr=%d\n"
                     "ldac_quality=%s\n"
                     "adaptive_bitrate=%d\n"
                     "follow_source_rate=%d\n",
                     codecs, (unsigned)s->sample_rate, (unsigned)s->bits,
                     enum_str(chmode_names, s->chmode), (unsigned)s->sbc_bitpool,
                     (unsigned)s->sbc_blocks, (unsigned)s->sbc_subbands,
                     enum_str(alloc_names, s->sbc_alloc), (unsigned)s->aac_bitrate,
                     s->aac_vbr ? 1 : 0, enum_str(ldac_names, s->ldac_quality),
                     s->adaptive_bitrate ? 1 : 0, s->follow_source_rate ? 1 : 0);
    if (n < 0 || (size_t)n >= cap)
        return OA2DP_ERR_NOSPC;
    return n;
}
