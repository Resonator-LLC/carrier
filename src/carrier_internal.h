/*  carrier_internal.h
 *
 *  Internal state for Carrier. Not part of the public API.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef CARRIER_INTERNAL_H
#define CARRIER_INTERNAL_H

#include "carrier.h"

#include <stdio.h>

#include <tox/tox.h>
#include <tox/toxav.h>
#include <tox/toxencryptsave.h>

#define CARRIER_MAX_FRIEND_REQUESTS 64
#define CARRIER_MAX_PASSWORD_LEN    64
#define CARRIER_MAX_TRANSFERS       32
#define CARRIER_MAX_PATH_LEN        512

struct CarrierFriendRequest {
    bool     active;
    uint8_t  public_key[TOX_PUBLIC_KEY_SIZE];
    char     message[TOX_MAX_FRIEND_REQUEST_LENGTH + 1];
};

typedef enum {
    CARRIER_TRANSFER_NONE = 0,
    CARRIER_TRANSFER_OUTBOUND_PENDING,   /* offer sent, awaiting RESUME */
    CARRIER_TRANSFER_OUTBOUND_ACTIVE,    /* sending chunks */
    CARRIER_TRANSFER_INBOUND_PENDING,    /* offer received, awaiting accept */
    CARRIER_TRANSFER_INBOUND_ACTIVE,     /* receiving chunks */
} CarrierTransferState;

struct CarrierTransfer {
    bool                  active;
    CarrierTransferState  state;
    uint32_t              friend_id;
    uint32_t              file_id;
    uint64_t              file_size;
    uint64_t              position;        /* bytes transferred so far */
    FILE                 *fp;              /* open handle for the duration of transfer */
    char                  path[CARRIER_MAX_PATH_LEN];
    char                  filename[CARRIER_MAX_NAME_LENGTH];
    uint64_t              last_progress_bytes; /* throttle progress events */
    int64_t               last_progress_ms;
};

struct Carrier {
    Tox   *tox;
    ToxAV *av;

    /* Profile */
    char  *profile_path;
    bool   is_encrypted;
    char   password[CARRIER_MAX_PASSWORD_LEN + 1];
    int    password_len;

    /* DHT bootstrap */
    char  *nodes_path;

    /* Friend requests */
    struct CarrierFriendRequest friend_requests[CARRIER_MAX_FRIEND_REQUESTS];
    int    num_friend_requests;

    /* File transfers */
    struct CarrierTransfer transfers[CARRIER_MAX_TRANSFERS];

    /* Event callback */
    carrier_event_cb event_cb;
    void            *event_userdata;

    /* Log callback */
    carrier_log_cb   log_cb;
    void            *log_userdata;
    CarrierLogLevel  log_level;

    /* Connection tracking */
    int    self_connection_status;  /* Tox_Connection */

    /* AV state */
    bool   av_initialized;

    /* AV frame counters (rate-limited DEBUG logging: emit every 100th
     * frame + the first frame of each direction, so [AV] shows call
     * start and ongoing flow without per-frame spam). */
    uint64_t av_audio_recv_frames;
    uint64_t av_video_recv_frames;
    uint64_t av_audio_send_frames;
    uint64_t av_video_send_frames;
};

#endif /* CARRIER_INTERNAL_H */
