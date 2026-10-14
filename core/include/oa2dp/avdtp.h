/* SPDX-License-Identifier: MIT */
/*
 * AVDTP 1.3 signalling (Bluetooth SIG "Audio/Video Distribution Transport
 * Protocol"). Transport-agnostic: the caller owns the L2CAP channels, feeds
 * every received signalling SDU to oa2dp_avdtp_input() and provides a send
 * callback. No dynamic allocation; one oa2dp_avdtp per remote device.
 */
#ifndef OA2DP_AVDTP_H
#define OA2DP_AVDTP_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OA2DP_AVDTP_PSM 0x0019

/* Signal identifiers */
enum {
    OA2DP_AVDTP_DISCOVER = 0x01,
    OA2DP_AVDTP_GET_CAPABILITIES = 0x02,
    OA2DP_AVDTP_SET_CONFIGURATION = 0x03,
    OA2DP_AVDTP_GET_CONFIGURATION = 0x04,
    OA2DP_AVDTP_RECONFIGURE = 0x05,
    OA2DP_AVDTP_OPEN = 0x06,
    OA2DP_AVDTP_START = 0x07,
    OA2DP_AVDTP_CLOSE = 0x08,
    OA2DP_AVDTP_SUSPEND = 0x09,
    OA2DP_AVDTP_ABORT = 0x0A,
    OA2DP_AVDTP_SECURITY_CONTROL = 0x0B,
    OA2DP_AVDTP_GET_ALL_CAPABILITIES = 0x0C,
    OA2DP_AVDTP_DELAYREPORT = 0x0D,
};

/* Message types (low two bits of the first header byte) */
enum {
    OA2DP_AVDTP_MSG_COMMAND = 0,
    OA2DP_AVDTP_MSG_GENERAL_REJECT = 1,
    OA2DP_AVDTP_MSG_ACCEPT = 2,
    OA2DP_AVDTP_MSG_REJECT = 3,
};

/* Service categories */
enum {
    OA2DP_AVDTP_CAT_MEDIA_TRANSPORT = 0x01,
    OA2DP_AVDTP_CAT_REPORTING = 0x02,
    OA2DP_AVDTP_CAT_RECOVERY = 0x03,
    OA2DP_AVDTP_CAT_CONTENT_PROTECTION = 0x04,
    OA2DP_AVDTP_CAT_HEADER_COMPRESSION = 0x05,
    OA2DP_AVDTP_CAT_MULTIPLEXING = 0x06,
    OA2DP_AVDTP_CAT_MEDIA_CODEC = 0x07,
    OA2DP_AVDTP_CAT_DELAY_REPORTING = 0x08,
};

/* AVDTP error codes */
enum {
    OA2DP_AVDTP_BAD_HEADER_FORMAT = 0x01,
    OA2DP_AVDTP_BAD_LENGTH = 0x11,
    OA2DP_AVDTP_BAD_ACP_SEID = 0x12,
    OA2DP_AVDTP_SEP_IN_USE = 0x13,
    OA2DP_AVDTP_SEP_NOT_IN_USE = 0x14,
    OA2DP_AVDTP_BAD_SERV_CATEGORY = 0x17,
    OA2DP_AVDTP_BAD_PAYLOAD_FORMAT = 0x18,
    OA2DP_AVDTP_NOT_SUPPORTED_COMMAND = 0x19,
    OA2DP_AVDTP_INVALID_CAPABILITIES = 0x1A,
    OA2DP_AVDTP_BAD_RECOVERY_TYPE = 0x22,
    OA2DP_AVDTP_BAD_MEDIA_TRANSPORT_FORMAT = 0x23,
    OA2DP_AVDTP_BAD_RECOVERY_FORMAT = 0x25,
    OA2DP_AVDTP_BAD_ROHC_FORMAT = 0x26,
    OA2DP_AVDTP_BAD_CP_FORMAT = 0x27,
    OA2DP_AVDTP_BAD_MULTIPLEXING_FORMAT = 0x28,
    OA2DP_AVDTP_UNSUPPORTED_CONFIGURATION = 0x29,
    OA2DP_AVDTP_BAD_STATE = 0x31,
};

#define OA2DP_AVDTP_TSEP_SRC 0
#define OA2DP_AVDTP_TSEP_SNK 1

#define OA2DP_AVDTP_MAX_SEPS 16          /* remote endpoints remembered per discover */
#define OA2DP_AVDTP_MAX_LOCAL_SEPS 8
#define OA2DP_AVDTP_CODEC_MAX 32
#define OA2DP_AVDTP_CP_MAX 16
#define OA2DP_AVDTP_MSG_MAX 1024         /* reassembled message limit */
#define OA2DP_AVDTP_TIMEOUT_MS 3000      /* signalling response timeout */

typedef enum {
    OA2DP_SEP_IDLE = 0,
    OA2DP_SEP_CONFIGURED,
    OA2DP_SEP_OPEN,
    OA2DP_SEP_STREAMING,
    OA2DP_SEP_CLOSING,
    OA2DP_SEP_ABORTING,
} oa2dp_sep_state;

/* Parsed set of service capabilities. */
typedef struct {
    bool media_transport;
    bool reporting;
    bool recovery;
    uint8_t recovery_type, recovery_mrws, recovery_mnmp;
    bool content_protection;
    uint16_t cp_type;
    uint8_t cp_value[OA2DP_AVDTP_CP_MAX];
    uint8_t cp_value_len;
    bool header_compression;
    uint8_t header_compression_flags;
    bool multiplexing;
    bool delay_reporting;
    bool media_codec;
    uint8_t codec[OA2DP_AVDTP_CODEC_MAX]; /* media type byte, codec type, info */
    uint8_t codec_len;
} oa2dp_avdtp_caps;

typedef struct {
    uint8_t seid;
    bool in_use;
    uint8_t media_type;
    uint8_t tsep;
} oa2dp_avdtp_sep_info;

/* Delivered for every response to one of our commands (msg_type ACCEPT,
 * REJECT, GENERAL_REJECT or timed_out) and for every remote command we have
 * accepted (msg_type COMMAND). */
typedef struct {
    uint8_t signal_id;
    uint8_t msg_type;
    bool timed_out;
    uint8_t label;
    uint8_t local_seid;   /* our SEP concerned (0 for DISCOVER) */
    uint8_t remote_seid;  /* peer SEP concerned, when known */
    uint8_t error_code;   /* for REJECT */
    uint8_t error_category;
    oa2dp_avdtp_sep_info seps[OA2DP_AVDTP_MAX_SEPS]; /* DISCOVER accept */
    uint8_t sep_count;
    oa2dp_avdtp_caps caps; /* GET_*CAPABILITIES / GET_CONFIGURATION accept,
                              SET_CONFIGURATION / RECONFIGURE command */
    uint16_t delay;        /* DELAYREPORT, 1/10 ms */
} oa2dp_avdtp_event;

typedef struct {
    /* Send one signalling SDU. Return 0 on success. Required. */
    int (*send)(void *user, const uint8_t *pdu, size_t len);
    /* Monotonic milliseconds, used for timeouts. Optional. */
    uint32_t (*now_ms)(void *user);
    /* Optional notification sink. */
    void (*event)(void *user, const oa2dp_avdtp_event *ev);
    /* Accept (return 0) or reject (return error code, set *category) a
     * configuration the peer proposes with SET_CONFIGURATION/RECONFIGURE.
     * Optional: if NULL every configuration is accepted. */
    uint8_t (*check_config)(void *user, uint8_t local_seid,
                            const oa2dp_avdtp_caps *cfg, uint8_t *category);
    /* Approve a peer OPEN/START/SUSPEND/CLOSE. Return 0 to accept or an
     * AVDTP error code. Optional. */
    uint8_t (*approve)(void *user, uint8_t local_seid, uint8_t signal_id);
} oa2dp_avdtp_callbacks;

typedef struct {
    uint8_t seid;
    uint8_t tsep;
    uint8_t media_type;
    oa2dp_sep_state state;
    oa2dp_avdtp_caps caps;    /* advertised capabilities */
    oa2dp_avdtp_caps config;  /* current configuration (CONFIGURED and later) */
    uint8_t remote_seid;      /* peer SEP bound by SET_CONFIGURATION */
    bool initiator;           /* we sent SET_CONFIGURATION */
} oa2dp_avdtp_sep;

typedef struct {
    bool used;
    uint8_t signal_id;
    uint8_t local_seid;
    uint8_t remote_seid;
    uint32_t sent_ms;
    oa2dp_avdtp_caps pending_config; /* SET_CONFIGURATION / RECONFIGURE */
} oa2dp_avdtp_pending;

typedef struct {
    oa2dp_avdtp_callbacks cb;
    void *user;
    uint16_t out_mtu;

    oa2dp_avdtp_sep seps[OA2DP_AVDTP_MAX_LOCAL_SEPS];
    uint8_t sep_count;

    oa2dp_avdtp_pending pending[16]; /* indexed by transaction label */
    uint8_t next_label;

    /* Reassembly of fragmented incoming messages */
    uint8_t rx_buf[OA2DP_AVDTP_MSG_MAX];
    size_t rx_len;
    uint8_t rx_label, rx_msg_type, rx_signal, rx_remaining;
    bool rx_active;
} oa2dp_avdtp;

void oa2dp_avdtp_init(oa2dp_avdtp *av, const oa2dp_avdtp_callbacks *cb,
                      void *user, uint16_t out_mtu);

/* Register a local stream endpoint. Returns the SEID (1..62) or < 0. */
int oa2dp_avdtp_add_sep(oa2dp_avdtp *av, uint8_t tsep, uint8_t media_type,
                        const oa2dp_avdtp_caps *caps);
oa2dp_avdtp_sep *oa2dp_avdtp_sep_get(oa2dp_avdtp *av, uint8_t seid);

/* Feed one received signalling SDU. Returns OA2DP_OK or an error (the
 * message is dropped or rejected; the channel stays usable). */
int oa2dp_avdtp_input(oa2dp_avdtp *av, const uint8_t *pdu, size_t len);

/* Initiator procedures. Each returns the transaction label (0..15) or < 0. */
int oa2dp_avdtp_discover(oa2dp_avdtp *av);
int oa2dp_avdtp_get_capabilities(oa2dp_avdtp *av, uint8_t remote_seid, bool all);
int oa2dp_avdtp_set_configuration(oa2dp_avdtp *av, uint8_t local_seid,
                                  uint8_t remote_seid,
                                  const oa2dp_avdtp_caps *config);
int oa2dp_avdtp_get_configuration(oa2dp_avdtp *av, uint8_t remote_seid);
int oa2dp_avdtp_reconfigure(oa2dp_avdtp *av, uint8_t local_seid,
                            const oa2dp_avdtp_caps *config);
int oa2dp_avdtp_open(oa2dp_avdtp *av, uint8_t local_seid);
int oa2dp_avdtp_start(oa2dp_avdtp *av, uint8_t local_seid);
int oa2dp_avdtp_suspend(oa2dp_avdtp *av, uint8_t local_seid);
int oa2dp_avdtp_close(oa2dp_avdtp *av, uint8_t local_seid);
int oa2dp_avdtp_abort(oa2dp_avdtp *av, uint8_t local_seid);
int oa2dp_avdtp_delay_report(oa2dp_avdtp *av, uint8_t local_seid, uint16_t delay);

/* The media transport channel of `local_seid` was closed: finishes CLOSE /
 * ABORT and returns the endpoint to IDLE. */
void oa2dp_avdtp_media_closed(oa2dp_avdtp *av, uint8_t local_seid);

/* The signalling channel went away: every endpoint returns to IDLE and
 * every outstanding command completes with timed_out. */
void oa2dp_avdtp_reset(oa2dp_avdtp *av);

/* Expire commands older than OA2DP_AVDTP_TIMEOUT_MS. */
void oa2dp_avdtp_check_timeouts(oa2dp_avdtp *av);

/* Capability (de)serialisation, exposed for tests and tools.
 * parse: on failure returns OA2DP_ERR_PROTO and sets *err_code and
 * *err_category to the AVDTP error to send back. With `strict` false,
 * unknown categories are skipped (used for capabilities from newer peers);
 * with `strict` true they are an error (used for configurations). */
int oa2dp_avdtp_caps_parse(const uint8_t *buf, size_t len, bool strict,
                           oa2dp_avdtp_caps *caps, uint8_t *err_code,
                           uint8_t *err_category);
/* `basic_only` omits categories newer than AVDTP 1.0 (delay reporting), as
 * required for GET_CAPABILITIES responses. Returns bytes written or < 0. */
int oa2dp_avdtp_caps_build(const oa2dp_avdtp_caps *caps, bool basic_only,
                           uint8_t *buf, size_t cap);

const char *oa2dp_avdtp_signal_name(uint8_t signal_id);
const char *oa2dp_sep_state_name(oa2dp_sep_state st);

#ifdef __cplusplus
}
#endif

#endif
