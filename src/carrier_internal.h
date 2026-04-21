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

#include <tox/tox.h>
#include <tox/toxav.h>
#include <tox/toxencryptsave.h>

#define CARRIER_MAX_FRIEND_REQUESTS 64
#define CARRIER_MAX_PASSWORD_LEN    64

struct CarrierFriendRequest {
    bool     active;
    uint8_t  public_key[TOX_PUBLIC_KEY_SIZE];
    char     message[TOX_MAX_FRIEND_REQUEST_LENGTH + 1];
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
};

#endif /* CARRIER_INTERNAL_H */
