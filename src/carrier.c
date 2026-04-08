/*  carrier.c
 *
 *  Core implementation of the Carrier library.
 *  Wraps toxcore to provide a clean C API with event callbacks.
 *
 *  Copyright (c) 2025-2026 Resonator LLC
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier.h"
#include "carrier_internal.h"
#include "carrier_events.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <tox/tox.h>
#include <tox/toxav.h>
#include <tox/toxencryptsave.h>

/* ---------------------------------------------------------------------------
 * Tox callbacks → CarrierEvent emission
 * ---------------------------------------------------------------------------*/

static void cb_self_connection_status(Tox *tox, Tox_Connection status, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    if (status != TOX_CONNECTION_NONE && c->self_connection_status == TOX_CONNECTION_NONE) {
        carrier_emit_connected(c, (int)status);
    } else if (status == TOX_CONNECTION_NONE && c->self_connection_status != TOX_CONNECTION_NONE) {
        carrier_emit_disconnected(c);
    }

    c->self_connection_status = (int)status;
}

static void cb_friend_request(Tox *tox, const uint8_t *public_key,
                              const uint8_t *data, size_t length, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    /* Store the request */
    int slot = -1;

    for (int i = 0; i < CARRIER_MAX_FRIEND_REQUESTS; i++) {
        if (!c->friend_requests[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        carrier_emit_error(c, "FriendRequest", "Friend request queue full");
        return;
    }

    c->friend_requests[slot].active = true;
    memcpy(c->friend_requests[slot].public_key, public_key, TOX_PUBLIC_KEY_SIZE);

    size_t msg_len = length < sizeof(c->friend_requests[slot].message) - 1
                     ? length : sizeof(c->friend_requests[slot].message) - 1;
    memcpy(c->friend_requests[slot].message, data, msg_len);
    c->friend_requests[slot].message[msg_len] = '\0';
    c->num_friend_requests++;

    /* Emit event */
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_FRIEND_REQUEST;
    ev.friend_request.request_id = (uint32_t)slot;

    /* Hex-encode the public key */
    for (int i = 0; i < TOX_PUBLIC_KEY_SIZE && i * 2 < CARRIER_MAX_KEY_LENGTH - 1; i++) {
        sprintf(ev.friend_request.key + i * 2, "%02X", public_key[i]);
    }

    snprintf(ev.friend_request.message, sizeof(ev.friend_request.message),
             "%s", c->friend_requests[slot].message);

    carrier_emit(c, &ev);
}

static void cb_friend_message(Tox *tox, uint32_t friend_number,
                              Tox_Message_Type type, const uint8_t *message,
                              size_t length, void *userdata)
{
    (void)tox;
    (void)type;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_TEXT_MESSAGE;
    ev.text_message.friend_id = friend_number;

    /* Get friend name */
    size_t name_size = tox_friend_get_name_size(c->tox, friend_number, NULL);

    if (name_size > 0 && name_size < CARRIER_MAX_NAME_LENGTH) {
        tox_friend_get_name(c->tox, friend_number, (uint8_t *)ev.text_message.name, NULL);
        ev.text_message.name[name_size] = '\0';
    }

    size_t text_len = length < sizeof(ev.text_message.text) - 1
                      ? length : sizeof(ev.text_message.text) - 1;
    memcpy(ev.text_message.text, message, text_len);
    ev.text_message.text[text_len] = '\0';

    carrier_emit(c, &ev);
}

static void cb_friend_connection_status(Tox *tox, uint32_t friend_number,
                                        Tox_Connection status, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));

    if (status != TOX_CONNECTION_NONE) {
        ev.type = CARRIER_EVENT_FRIEND_ONLINE;
        ev.friend_online.friend_id = friend_number;

        size_t name_size = tox_friend_get_name_size(c->tox, friend_number, NULL);

        if (name_size > 0 && name_size < CARRIER_MAX_NAME_LENGTH) {
            tox_friend_get_name(c->tox, friend_number,
                                (uint8_t *)ev.friend_online.name, NULL);
            ev.friend_online.name[name_size] = '\0';
        }
    } else {
        ev.type = CARRIER_EVENT_FRIEND_OFFLINE;
        ev.friend_offline.friend_id = friend_number;
    }

    carrier_emit(c, &ev);
}

static void cb_friend_name(Tox *tox, uint32_t friend_number,
                           const uint8_t *name, size_t length, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_NICK;
    ev.nick.friend_id = friend_number;

    size_t n = length < CARRIER_MAX_NAME_LENGTH - 1 ? length : CARRIER_MAX_NAME_LENGTH - 1;
    memcpy(ev.nick.name, name, n);
    ev.nick.name[n] = '\0';

    carrier_emit(c, &ev);
}

static void cb_friend_status(Tox *tox, uint32_t friend_number,
                             Tox_User_Status status, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_STATUS;
    ev.status.friend_id = friend_number;
    ev.status.status = (int)status;

    carrier_emit(c, &ev);
}

static void cb_friend_status_message(Tox *tox, uint32_t friend_number,
                                     const uint8_t *message, size_t length,
                                     void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_STATUS_MESSAGE;
    ev.status_message.friend_id = friend_number;

    size_t n = length < CARRIER_MAX_MESSAGE_LENGTH - 1 ? length : CARRIER_MAX_MESSAGE_LENGTH - 1;
    memcpy(ev.status_message.text, message, n);
    ev.status_message.text[n] = '\0';

    carrier_emit(c, &ev);
}

static void cb_friend_read_receipt(Tox *tox, uint32_t friend_number,
                                   uint32_t message_id, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_MESSAGE_SENT;
    ev.message_sent.friend_id = friend_number;
    ev.message_sent.receipt = message_id;

    carrier_emit(c, &ev);
}

static void cb_file_recv(Tox *tox, uint32_t friend_number, uint32_t file_number,
                         uint32_t kind, uint64_t file_size,
                         const uint8_t *filename, size_t filename_length,
                         void *userdata)
{
    (void)tox;
    (void)kind;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_FILE_TRANSFER;
    ev.file_transfer.friend_id = friend_number;
    ev.file_transfer.file_id = file_number;
    ev.file_transfer.file_size = file_size;

    size_t n = filename_length < CARRIER_MAX_NAME_LENGTH - 1
               ? filename_length : CARRIER_MAX_NAME_LENGTH - 1;
    memcpy(ev.file_transfer.filename, filename, n);
    ev.file_transfer.filename[n] = '\0';

    carrier_emit(c, &ev);
}

/* Group callbacks */
static void cb_group_message(Tox *tox, uint32_t group_number, uint32_t peer_id,
                             Tox_Message_Type type, const uint8_t *message,
                             size_t length, Tox_Group_Message_Id msg_id,
                             void *userdata)
{
    (void)tox;
    (void)type;
    (void)msg_id;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_GROUP_MESSAGE;
    ev.group_message.group_id = group_number;
    ev.group_message.peer_id = peer_id;

    /* Get peer name */
    Tox_Err_Group_Peer_Query err;
    size_t name_size = tox_group_peer_get_name_size(c->tox, group_number, peer_id, &err);

    if (err == TOX_ERR_GROUP_PEER_QUERY_OK && name_size < CARRIER_MAX_NAME_LENGTH) {
        tox_group_peer_get_name(c->tox, group_number, peer_id,
                                (uint8_t *)ev.group_message.name, &err);
        ev.group_message.name[name_size] = '\0';
    }

    size_t n = length < CARRIER_MAX_MESSAGE_LENGTH - 1 ? length : CARRIER_MAX_MESSAGE_LENGTH - 1;
    memcpy(ev.group_message.text, message, n);
    ev.group_message.text[n] = '\0';

    carrier_emit(c, &ev);
}

static void cb_group_invite(Tox *tox, uint32_t friend_number,
                            const uint8_t *invite_data, size_t length,
                            const uint8_t *group_name, size_t group_name_length,
                            void *userdata)
{
    (void)tox;
    (void)invite_data;
    (void)length;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_GROUP_INVITE;
    ev.group_invite.friend_id = friend_number;

    size_t n = group_name_length < CARRIER_MAX_NAME_LENGTH - 1
               ? group_name_length : CARRIER_MAX_NAME_LENGTH - 1;
    memcpy(ev.group_invite.name, group_name, n);
    ev.group_invite.name[n] = '\0';

    carrier_emit(c, &ev);
}

static void cb_group_peer_join(Tox *tox, uint32_t group_number,
                               uint32_t peer_id, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_GROUP_PEER_JOIN;
    ev.group_peer_join.group_id = group_number;
    ev.group_peer_join.peer_id = peer_id;

    Tox_Err_Group_Peer_Query err;
    size_t name_size = tox_group_peer_get_name_size(c->tox, group_number, peer_id, &err);

    if (err == TOX_ERR_GROUP_PEER_QUERY_OK && name_size < CARRIER_MAX_NAME_LENGTH) {
        tox_group_peer_get_name(c->tox, group_number, peer_id,
                                (uint8_t *)ev.group_peer_join.name, &err);
        ev.group_peer_join.name[name_size] = '\0';
    }

    carrier_emit(c, &ev);
}

static void cb_group_self_join(Tox *tox, uint32_t group_number, void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_GROUP_SELF_JOIN;
    ev.group_self_join.group_id = group_number;

    carrier_emit(c, &ev);
}

/* ToxAV callbacks */
static void cb_av_call(ToxAV *av, uint32_t friend_number,
                       bool audio_enabled, bool video_enabled, void *userdata)
{
    (void)av;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_CALL;
    ev.call.friend_id = friend_number;
    ev.call.audio = audio_enabled;
    ev.call.video = video_enabled;

    carrier_emit(c, &ev);
}

static void cb_av_call_state(ToxAV *av, uint32_t friend_number,
                             uint32_t state, void *userdata)
{
    (void)av;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_CALL_STATE;
    ev.call_state.friend_id = friend_number;
    ev.call_state.state = state;

    carrier_emit(c, &ev);
}

static void cb_av_audio_receive_frame(ToxAV *av, uint32_t friend_number,
                                      const int16_t *pcm, size_t sample_count,
                                      uint8_t channels, uint32_t sampling_rate,
                                      void *userdata)
{
    (void)av;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_AUDIO_FRAME;
    ev.audio_frame.friend_id = friend_number;
    ev.audio_frame.pcm = pcm;
    ev.audio_frame.samples = sample_count;
    ev.audio_frame.channels = channels;
    ev.audio_frame.sample_rate = sampling_rate;

    carrier_emit(c, &ev);
}

static void cb_av_video_receive_frame(ToxAV *av, uint32_t friend_number,
                                      uint16_t width, uint16_t height,
                                      const uint8_t *y, const uint8_t *u,
                                      const uint8_t *v,
                                      int32_t ystride, int32_t ustride,
                                      int32_t vstride,
                                      void *userdata)
{
    (void)av;
    (void)ystride;
    (void)ustride;
    (void)vstride;
    Carrier *c = (Carrier *)userdata;

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_VIDEO_FRAME;
    ev.video_frame.friend_id = friend_number;
    ev.video_frame.width = width;
    ev.video_frame.height = height;
    ev.video_frame.y = y;
    ev.video_frame.u = u;
    ev.video_frame.v = v;

    carrier_emit(c, &ev);
}

/* ---------------------------------------------------------------------------
 * Pipe mode — lossless custom packet callback
 * ---------------------------------------------------------------------------*/

#define CARRIER_PIPE_DATA_TAG  0xA0
#define CARRIER_PIPE_EOF_TAG   0xA1
#define CARRIER_PIPE_OPEN_TAG  0xA2

static void cb_friend_lossless_packet(Tox *tox, uint32_t friend_number,
                                      const uint8_t *data, size_t length,
                                      void *userdata)
{
    (void)tox;
    Carrier *c = (Carrier *)userdata;

    if (length < 1) {
        return;
    }

    switch (data[0]) {
        case CARRIER_PIPE_OPEN_TAG: {
            CarrierEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = CARRIER_EVENT_PIPE;
            ev.pipe.friend_id = friend_number;
            carrier_emit(c, &ev);
            break;
        }

        case CARRIER_PIPE_DATA_TAG: {
            CarrierEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = CARRIER_EVENT_PIPE_DATA;
            ev.pipe_data.friend_id = friend_number;
            ev.pipe_data.data = data + 1;
            ev.pipe_data.len = length - 1;
            carrier_emit(c, &ev);
            break;
        }

        case CARRIER_PIPE_EOF_TAG: {
            CarrierEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = CARRIER_EVENT_PIPE_EOF;
            ev.pipe_eof.friend_id = friend_number;
            carrier_emit(c, &ev);
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Register all Tox callbacks
 * ---------------------------------------------------------------------------*/

static void register_callbacks(Carrier *c)
{
    tox_callback_self_connection_status(c->tox, cb_self_connection_status);
    tox_callback_friend_request(c->tox, cb_friend_request);
    tox_callback_friend_message(c->tox, cb_friend_message);
    tox_callback_friend_connection_status(c->tox, cb_friend_connection_status);
    tox_callback_friend_name(c->tox, cb_friend_name);
    tox_callback_friend_status(c->tox, cb_friend_status);
    tox_callback_friend_status_message(c->tox, cb_friend_status_message);
    tox_callback_friend_read_receipt(c->tox, cb_friend_read_receipt);
    tox_callback_file_recv(c->tox, cb_file_recv);
    tox_callback_group_message(c->tox, cb_group_message);
    tox_callback_group_invite(c->tox, cb_group_invite);
    tox_callback_group_peer_join(c->tox, cb_group_peer_join);
    tox_callback_group_self_join(c->tox, cb_group_self_join);
    tox_callback_friend_lossless_packet(c->tox, cb_friend_lossless_packet);

    if (c->av != NULL) {
        toxav_callback_call(c->av, cb_av_call, c);
        toxav_callback_call_state(c->av, cb_av_call_state, c);
        toxav_callback_audio_receive_frame(c->av, cb_av_audio_receive_frame, c);
        toxav_callback_video_receive_frame(c->av, cb_av_video_receive_frame, c);
    }
}

/* ---------------------------------------------------------------------------
 * Profile loading / saving
 * ---------------------------------------------------------------------------*/

static long file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }

    return (long)st.st_size;
}

static bool load_profile(Carrier *c, struct Tox_Options *opts)
{
    FILE *fp = fopen(c->profile_path, "rb");

    if (fp == NULL) {
        /* New profile */
        Tox_Err_New err;
        tox_options_set_savedata_type(opts, TOX_SAVEDATA_TYPE_NONE);
        c->tox = tox_new(opts, &err);

        if (c->tox == NULL) {
            fprintf(stderr, "carrier: tox_new() failed: %d\n", err);
            return false;
        }

        tox_self_set_name(c->tox, (const uint8_t *)"Carrier User",
                          strlen("Carrier User"), NULL);
        return true;
    }

    long len = file_size(c->profile_path);

    if (len <= 0) {
        fclose(fp);
        return false;
    }

    uint8_t *data = malloc((size_t)len);

    if (data == NULL) {
        fclose(fp);
        return false;
    }

    if (fread(data, (size_t)len, 1, fp) != 1) {
        free(data);
        fclose(fp);
        return false;
    }

    fclose(fp);

    tox_options_set_savedata_type(opts, TOX_SAVEDATA_TYPE_TOX_SAVE);
    tox_options_set_savedata_data(opts, data, (size_t)len);

    Tox_Err_New err;
    c->tox = tox_new(opts, &err);
    free(data);

    if (c->tox == NULL) {
        fprintf(stderr, "carrier: tox_new() failed: %d\n", err);
        return false;
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * DHT bootstrap
 * ---------------------------------------------------------------------------*/

/* Minimal hardcoded bootstrap nodes as fallback */
static void bootstrap_default(Carrier *c)
{
    /* A few well-known Tox bootstrap nodes */
    static const struct {
        const char *ip;
        uint16_t port;
        const char *key_hex;
    } default_nodes[] = {
        {
            "tox.plastiras.org", 33445,
            "8E8B63299B3D520FB377FE5100E65E3322F7AE5B20A0ACED2981769FC5B43725"
        },
        {
            "tox2.plastiras.org", 33445,
            "B6626D386BE7E3ACA107B46F48A5C4D522D29571D492E2A2D30E01D5EAC7D538"
        },
        {
            "tox4.plastiras.org", 33445,
            "836D1DA2BE12FE0E669334E437BE3FB02ACB4F18B6415C23EA38B7A78D3D5BE4"
        },
        {
            "188.225.9.167", 33445,
            "1911341A83E02503AB1FD6561BD64AF3A9D6C3F12B5FBB656976B2E678644A67"
        },
        {
            "mother.resonator.network", 33445,
            "EF2349F351E44778B0A92729A3156870BEBBE1DB4B23137B39E3C920F8DDA96A"
        },
    };

    for (size_t i = 0; i < sizeof(default_nodes) / sizeof(default_nodes[0]); i++) {
        uint8_t key_bin[TOX_PUBLIC_KEY_SIZE];
        const char *hex = default_nodes[i].key_hex;

        for (int j = 0; j < TOX_PUBLIC_KEY_SIZE; j++) {
            unsigned int b;
            sscanf(hex + j * 2, "%2x", &b);
            key_bin[j] = (uint8_t)b;
        }

        tox_bootstrap(c->tox, default_nodes[i].ip, default_nodes[i].port,
                      key_bin, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Public API: Lifecycle
 * ---------------------------------------------------------------------------*/

Carrier *carrier_new(const char *profile_path,
                     const char *config_path,
                     const char *nodes_path)
{
    (void)config_path;  /* TODO: load libconfig settings */

    Carrier *c = calloc(1, sizeof(Carrier));

    if (c == NULL) {
        return NULL;
    }

    if (profile_path != NULL) {
        c->profile_path = strdup(profile_path);
    } else {
        c->profile_path = strdup("carrier_profile.tox");
    }

    if (nodes_path != NULL) {
        c->nodes_path = strdup(nodes_path);
    }

    c->self_connection_status = TOX_CONNECTION_NONE;

    /* Create Tox options */
    Tox_Err_Options_New opt_err;
    struct Tox_Options *opts = tox_options_new(&opt_err);

    if (opts == NULL) {
        free(c->profile_path);
        free(c->nodes_path);
        free(c);
        return NULL;
    }

    tox_options_default(opts);
    tox_options_set_experimental_groups_persistence(opts, true);

    /* Load or create profile */
    if (!load_profile(c, opts)) {
        tox_options_free(opts);
        free(c->profile_path);
        free(c->nodes_path);
        free(c);
        return NULL;
    }

    tox_options_free(opts);

    /* Initialize ToxAV */
    Toxav_Err_New av_err;
    c->av = toxav_new(c->tox, &av_err);
    c->av_initialized = (c->av != NULL);

    /* Register callbacks */
    register_callbacks(c);

    /* Bootstrap */
    bootstrap_default(c);

    /* Save new profile if it didn't exist */
    carrier_save(c);

    return c;
}

void carrier_free(Carrier *c)
{
    if (c == NULL) {
        return;
    }

    carrier_save(c);

    if (c->av != NULL) {
        toxav_kill(c->av);
    }

    if (c->tox != NULL) {
        tox_kill(c->tox);
    }

    free(c->profile_path);
    free(c->nodes_path);
    free(c);
}

int carrier_iterate(Carrier *c)
{
    if (c == NULL || c->tox == NULL) {
        return -1;
    }

    tox_iterate(c->tox, c);

    if (c->av != NULL) {
        toxav_iterate(c->av);
    }

    return 0;
}

int carrier_iteration_interval(Carrier *c)
{
    if (c == NULL || c->tox == NULL) {
        return 50;
    }

    return (int)tox_iteration_interval(c->tox);
}

void carrier_set_event_callback(Carrier *c, carrier_event_cb cb, void *userdata)
{
    if (c == NULL) {
        return;
    }

    c->event_cb = cb;
    c->event_userdata = userdata;
}

/* ---------------------------------------------------------------------------
 * Public API: Identity & status
 * ---------------------------------------------------------------------------*/

int carrier_get_id(Carrier *c)
{
    if (c == NULL) {
        return -1;
    }

    uint8_t address[TOX_ADDRESS_SIZE];
    tox_self_get_address(c->tox, address);

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_SELF_ID;

    for (int i = 0; i < TOX_ADDRESS_SIZE && i * 2 < CARRIER_MAX_ID_LENGTH - 1; i++) {
        sprintf(ev.self_id.id + i * 2, "%02X", address[i]);
    }

    carrier_emit(c, &ev);
    return 0;
}

int carrier_get_dht_info(Carrier *c)
{
    if (c == NULL) {
        return -1;
    }

    uint8_t dht_id[TOX_PUBLIC_KEY_SIZE];
    tox_self_get_dht_id(c->tox, dht_id);

    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_DHT_INFO;

    for (int i = 0; i < TOX_PUBLIC_KEY_SIZE && i * 2 < CARRIER_MAX_KEY_LENGTH - 1; i++) {
        sprintf(ev.dht_info.key + i * 2, "%02X", dht_id[i]);
    }

    ev.dht_info.port = tox_self_get_udp_port(c->tox, NULL);

    carrier_emit(c, &ev);
    return 0;
}

int carrier_bootstrap(Carrier *c, const char *host, uint16_t port,
                      const char *key_hex)
{
    if (c == NULL || host == NULL || key_hex == NULL) {
        return -1;
    }

    uint8_t key_bin[TOX_PUBLIC_KEY_SIZE];

    for (int i = 0; i < TOX_PUBLIC_KEY_SIZE; i++) {
        unsigned int b;
        sscanf(key_hex + i * 2, "%2x", &b);
        key_bin[i] = (uint8_t)b;
    }

    Tox_Err_Bootstrap err;
    tox_bootstrap(c->tox, host, port, key_bin, &err);

    if (err != TOX_ERR_BOOTSTRAP_OK) {
        carrier_emit_error(c, "Bootstrap", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_set_nick(Carrier *c, const char *nick)
{
    if (c == NULL || nick == NULL) {
        return -1;
    }

    Tox_Err_Set_Info err;
    tox_self_set_name(c->tox, (const uint8_t *)nick, strlen(nick), &err);

    if (err != TOX_ERR_SET_INFO_OK) {
        carrier_emit_error(c, "SetNick", "Failed to set nick (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_set_status(Carrier *c, int status)
{
    if (c == NULL) {
        return -1;
    }

    tox_self_set_status(c->tox, (Tox_User_Status)status);
    return 0;
}

int carrier_set_status_message(Carrier *c, const char *msg)
{
    if (c == NULL || msg == NULL) {
        return -1;
    }

    Tox_Err_Set_Info err;
    tox_self_set_status_message(c->tox, (const uint8_t *)msg, strlen(msg), &err);

    if (err != TOX_ERR_SET_INFO_OK) {
        carrier_emit_error(c, "SetStatusMessage",
                           "Failed to set status message (error %d)", err);
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API: Friends
 * ---------------------------------------------------------------------------*/

int carrier_add_friend(Carrier *c, const char *tox_id_hex, const char *message)
{
    if (c == NULL || tox_id_hex == NULL) {
        return -1;
    }

    const char *msg = (message != NULL) ? message : "Hello from Carrier";
    size_t hex_len = strlen(tox_id_hex);

    if (hex_len != TOX_ADDRESS_SIZE * 2) {
        carrier_emit_error(c, "AddFriend", "Invalid Tox ID length");
        return -1;
    }

    uint8_t address[TOX_ADDRESS_SIZE];

    for (int i = 0; i < TOX_ADDRESS_SIZE; i++) {
        unsigned int b;
        sscanf(tox_id_hex + i * 2, "%2x", &b);
        address[i] = (uint8_t)b;
    }

    Tox_Err_Friend_Add err;
    uint32_t friend_num = tox_friend_add(c->tox, address,
                                         (const uint8_t *)msg, strlen(msg), &err);

    if (err != TOX_ERR_FRIEND_ADD_OK) {
        carrier_emit_error(c, "AddFriend", "Failed to add friend (error %d)", err);
        return -1;
    }

    carrier_emit_system(c, "Friend added as #%u", friend_num);
    return (int)friend_num;
}

int carrier_accept_friend(Carrier *c, uint32_t request_id)
{
    if (c == NULL || request_id >= CARRIER_MAX_FRIEND_REQUESTS) {
        return -1;
    }

    if (!c->friend_requests[request_id].active) {
        carrier_emit_error(c, "AcceptFriend", "No such friend request: %u", request_id);
        return -1;
    }

    Tox_Err_Friend_Add err;
    uint32_t friend_num = tox_friend_add_norequest(
        c->tox, c->friend_requests[request_id].public_key, &err);

    if (err != TOX_ERR_FRIEND_ADD_OK) {
        carrier_emit_error(c, "AcceptFriend", "Failed (error %d)", err);
        return -1;
    }

    c->friend_requests[request_id].active = false;
    c->num_friend_requests--;

    carrier_emit_system(c, "Friend request %u accepted as #%u", request_id, friend_num);
    return (int)friend_num;
}

int carrier_decline_friend(Carrier *c, uint32_t request_id)
{
    if (c == NULL || request_id >= CARRIER_MAX_FRIEND_REQUESTS) {
        return -1;
    }

    if (!c->friend_requests[request_id].active) {
        carrier_emit_error(c, "DeclineFriend", "No such friend request: %u", request_id);
        return -1;
    }

    c->friend_requests[request_id].active = false;
    c->num_friend_requests--;

    carrier_emit_system(c, "Friend request %u declined", request_id);
    return 0;
}

int carrier_delete_friend(Carrier *c, uint32_t friend_id)
{
    if (c == NULL) {
        return -1;
    }

    Tox_Err_Friend_Delete err;
    tox_friend_delete(c->tox, friend_id, &err);

    if (err != TOX_ERR_FRIEND_DELETE_OK) {
        carrier_emit_error(c, "DeleteFriend", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_send_message(Carrier *c, uint32_t friend_id, const char *text)
{
    if (c == NULL || text == NULL) {
        return -1;
    }

    Tox_Err_Friend_Send_Message err;
    uint32_t receipt = tox_friend_send_message(
        c->tox, friend_id, TOX_MESSAGE_TYPE_NORMAL,
        (const uint8_t *)text, strlen(text), &err);

    if (err != TOX_ERR_FRIEND_SEND_MESSAGE_OK) {
        carrier_emit_error(c, "SendMsg", "Failed to send message (error %d)", err);
        return -1;
    }

    return (int)receipt;
}

/* ---------------------------------------------------------------------------
 * Public API: File transfers
 * ---------------------------------------------------------------------------*/

int carrier_send_file(Carrier *c, uint32_t friend_id, const char *path)
{
    if (c == NULL || path == NULL) {
        return -1;
    }

    long fsize = file_size(path);

    if (fsize < 0) {
        carrier_emit_error(c, "SendFile", "Cannot stat file: %s", path);
        return -1;
    }

    /* Extract filename from path */
    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;

    Tox_Err_File_Send err;
    uint32_t file_num = tox_file_send(c->tox, friend_id, TOX_FILE_KIND_DATA,
                                      (uint64_t)fsize,
                                      NULL, /* file_id - auto generate */
                                      (const uint8_t *)filename, strlen(filename),
                                      &err);

    if (err != TOX_ERR_FILE_SEND_OK) {
        carrier_emit_error(c, "SendFile", "Failed (error %d)", err);
        return -1;
    }

    carrier_emit_system(c, "File transfer #%u started: %s", file_num, filename);
    return (int)file_num;
}

int carrier_accept_file(Carrier *c, uint32_t friend_id, uint32_t file_id,
                        const char *save_path)
{
    (void)save_path;  /* TODO: implement file recv with save path */

    if (c == NULL) {
        return -1;
    }

    Tox_Err_File_Control err;
    tox_file_control(c->tox, friend_id, file_id, TOX_FILE_CONTROL_RESUME, &err);

    if (err != TOX_ERR_FILE_CONTROL_OK) {
        carrier_emit_error(c, "AcceptFile", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_cancel_file(Carrier *c, uint32_t friend_id, uint32_t file_id)
{
    if (c == NULL) {
        return -1;
    }

    Tox_Err_File_Control err;
    tox_file_control(c->tox, friend_id, file_id, TOX_FILE_CONTROL_CANCEL, &err);

    if (err != TOX_ERR_FILE_CONTROL_OK) {
        carrier_emit_error(c, "CancelFile", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API: Groups
 * ---------------------------------------------------------------------------*/

int carrier_create_group(Carrier *c, const char *name, bool is_public)
{
    if (c == NULL || name == NULL) {
        return -1;
    }

    Tox_Group_Privacy_State privacy = is_public
        ? TOX_GROUP_PRIVACY_STATE_PUBLIC
        : TOX_GROUP_PRIVACY_STATE_PRIVATE;

    Tox_Err_Group_New err;
    uint32_t group_num = tox_group_new(
        c->tox, privacy, (const uint8_t *)name, strlen(name),
        (const uint8_t *)name, strlen(name), &err);

    if (err != TOX_ERR_GROUP_NEW_OK) {
        carrier_emit_error(c, "CreateGroup", "Failed (error %d)", err);
        return -1;
    }

    carrier_emit_system(c, "Group #%u created: %s", group_num, name);
    return (int)group_num;
}

int carrier_join_group(Carrier *c, uint32_t friend_id)
{
    (void)friend_id;  /* TODO: implement group join from invite data */

    if (c == NULL) {
        return -1;
    }

    carrier_emit_error(c, "JoinGroup", "Not yet implemented");
    return -1;
}

int carrier_leave_group(Carrier *c, uint32_t group_id)
{
    if (c == NULL) {
        return -1;
    }

    tox_group_leave(c->tox, group_id, NULL, 0, NULL);
    return 0;
}

int carrier_send_group_message(Carrier *c, uint32_t group_id, const char *text)
{
    if (c == NULL || text == NULL) {
        return -1;
    }

    Tox_Err_Group_Send_Message err;
    tox_group_send_message(c->tox, group_id, TOX_MESSAGE_TYPE_NORMAL,
                           (const uint8_t *)text, strlen(text), &err);

    if (err != TOX_ERR_GROUP_SEND_MESSAGE_OK) {
        carrier_emit_error(c, "SendGroupMsg", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_invite_to_group(Carrier *c, uint32_t group_id, uint32_t friend_id)
{
    if (c == NULL) {
        return -1;
    }

    Tox_Err_Group_Invite_Friend err;
    tox_group_invite_friend(c->tox, group_id, friend_id, &err);

    if (err != TOX_ERR_GROUP_INVITE_FRIEND_OK) {
        carrier_emit_error(c, "InviteToGroup", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API: Audio/Video calls
 * ---------------------------------------------------------------------------*/

int carrier_call(Carrier *c, uint32_t friend_id, bool audio, bool video)
{
    if (c == NULL || c->av == NULL) {
        return -1;
    }

    Toxav_Err_Call err;
    toxav_call(c->av, friend_id, audio ? 64 : 0, video ? 5000 : 0, &err);

    if (err != TOXAV_ERR_CALL_OK) {
        carrier_emit_error(c, "Call", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_answer(Carrier *c, uint32_t friend_id, bool audio, bool video)
{
    if (c == NULL || c->av == NULL) {
        return -1;
    }

    Toxav_Err_Answer err;
    toxav_answer(c->av, friend_id, audio ? 64 : 0, video ? 5000 : 0, &err);

    if (err != TOXAV_ERR_ANSWER_OK) {
        carrier_emit_error(c, "Answer", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_hangup(Carrier *c, uint32_t friend_id)
{
    if (c == NULL || c->av == NULL) {
        return -1;
    }

    Toxav_Err_Call_Control err;
    toxav_call_control(c->av, friend_id, TOXAV_CALL_CONTROL_CANCEL, &err);

    if (err != TOXAV_ERR_CALL_CONTROL_OK) {
        carrier_emit_error(c, "Hangup", "Failed (error %d)", err);
        return -1;
    }

    return 0;
}

int carrier_send_audio(Carrier *c, uint32_t friend_id,
                       const int16_t *pcm, size_t samples,
                       uint8_t channels, uint32_t sample_rate)
{
    if (c == NULL || c->av == NULL || pcm == NULL) {
        return -1;
    }

    Toxav_Err_Send_Frame err;
    toxav_audio_send_frame(c->av, friend_id, pcm, samples,
                           channels, sample_rate, &err);

    if (err != TOXAV_ERR_SEND_FRAME_OK) {
        return -1;
    }

    return 0;
}

int carrier_send_video(Carrier *c, uint32_t friend_id,
                       const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       uint16_t width, uint16_t height)
{
    if (c == NULL || c->av == NULL) {
        return -1;
    }

    Toxav_Err_Send_Frame err;
    toxav_video_send_frame(c->av, friend_id, width, height, y, u, v, &err);

    if (err != TOXAV_ERR_SEND_FRAME_OK) {
        return -1;
    }

    return 0;
}

int carrier_set_audio_bitrate(Carrier *c, uint32_t friend_id, uint32_t bitrate)
{
    if (c == NULL || c->av == NULL) {
        return -1;
    }

    toxav_audio_set_bit_rate(c->av, friend_id, bitrate, NULL);
    return 0;
}

int carrier_set_video_bitrate(Carrier *c, uint32_t friend_id, uint32_t bitrate)
{
    if (c == NULL || c->av == NULL) {
        return -1;
    }

    toxav_video_set_bit_rate(c->av, friend_id, bitrate, NULL);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API: Pipe mode
 * ---------------------------------------------------------------------------*/

int carrier_pipe_open(Carrier *c, uint32_t friend_id)
{
    if (c == NULL) {
        return -1;
    }

    uint8_t packet[1] = { CARRIER_PIPE_OPEN_TAG };

    Tox_Err_Friend_Custom_Packet err;
    tox_friend_send_lossless_packet(c->tox, friend_id, packet, 1, &err);

    if (err != TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        /* Don't emit error — caller may be polling for connectivity */
        return -1;
    }

    return 0;
}

int carrier_pipe_write(Carrier *c, uint32_t friend_id,
                       const uint8_t *data, size_t len)
{
    if (c == NULL || data == NULL || len == 0) {
        return -1;
    }

    uint32_t max_payload = tox_max_custom_packet_size() - 1;  /* -1 for tag byte */
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = (len - sent > max_payload) ? max_payload : (len - sent);

        uint8_t *packet = malloc(chunk + 1);

        if (packet == NULL) {
            return -1;
        }

        packet[0] = CARRIER_PIPE_DATA_TAG;
        memcpy(packet + 1, data + sent, chunk);

        Tox_Err_Friend_Custom_Packet err;
        tox_friend_send_lossless_packet(c->tox, friend_id, packet, chunk + 1, &err);
        free(packet);

        if (err != TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
            if (sent > 0) {
                return (int)sent;  /* partial send */
            }

            return -1;
        }

        sent += chunk;
    }

    return (int)sent;
}

int carrier_pipe_close(Carrier *c, uint32_t friend_id)
{
    if (c == NULL) {
        return -1;
    }

    uint8_t packet[1] = { CARRIER_PIPE_EOF_TAG };

    Tox_Err_Friend_Custom_Packet err;
    tox_friend_send_lossless_packet(c->tox, friend_id, packet, 1, &err);

    if (err != TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API: Profile persistence
 * ---------------------------------------------------------------------------*/

int carrier_save(Carrier *c)
{
    if (c == NULL || c->tox == NULL || c->profile_path == NULL) {
        return -1;
    }

    size_t data_len = tox_get_savedata_size(c->tox);
    uint8_t *data = malloc(data_len);

    if (data == NULL) {
        return -1;
    }

    tox_get_savedata(c->tox, data);

    /* Write to temp file then rename for atomicity */
    size_t tmp_len = strlen(c->profile_path) + 5;
    char *tmp_path = malloc(tmp_len);

    if (tmp_path == NULL) {
        free(data);
        return -1;
    }

    snprintf(tmp_path, tmp_len, "%s.tmp", c->profile_path);

    FILE *fp = fopen(tmp_path, "wb");

    if (fp == NULL) {
        free(data);
        free(tmp_path);
        return -1;
    }

    if (fwrite(data, data_len, 1, fp) != 1) {
        fclose(fp);
        free(data);
        free(tmp_path);
        return -1;
    }

    fclose(fp);
    free(data);

    if (rename(tmp_path, c->profile_path) != 0) {
        free(tmp_path);
        return -1;
    }

    free(tmp_path);
    return 0;
}
