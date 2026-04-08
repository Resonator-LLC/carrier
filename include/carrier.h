/*  carrier.h
 *
 *  Carrier — Cross-platform C library for the Tox protocol.
 *  Part of the Resonator project.
 *
 *  Copyright (c) 2025-2026 Resonator LLC
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef CARRIER_H
#define CARRIER_H

#define CARRIER_VERSION_MAJOR 2
#define CARRIER_VERSION_MINOR 0
#define CARRIER_VERSION_PATCH 0
#define CARRIER_VERSION_STRING "2.0.0"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Carrier Carrier;

/* ---------------------------------------------------------------------------
 * Event types
 * ---------------------------------------------------------------------------*/

typedef enum {
    CARRIER_EVENT_CONNECTED,
    CARRIER_EVENT_DISCONNECTED,
    CARRIER_EVENT_SELF_ID,
    CARRIER_EVENT_TEXT_MESSAGE,
    CARRIER_EVENT_MESSAGE_SENT,
    CARRIER_EVENT_FRIEND_REQUEST,
    CARRIER_EVENT_FRIEND_ONLINE,
    CARRIER_EVENT_FRIEND_OFFLINE,
    CARRIER_EVENT_NICK,
    CARRIER_EVENT_STATUS,
    CARRIER_EVENT_STATUS_MESSAGE,
    CARRIER_EVENT_GROUP_MESSAGE,
    CARRIER_EVENT_GROUP_PEER_JOIN,
    CARRIER_EVENT_GROUP_PEER_EXIT,
    CARRIER_EVENT_GROUP_INVITE,
    CARRIER_EVENT_GROUP_SELF_JOIN,
    CARRIER_EVENT_CONFERENCE_MESSAGE,
    CARRIER_EVENT_CONFERENCE_INVITE,
    CARRIER_EVENT_FILE_TRANSFER,
    CARRIER_EVENT_FILE_PROGRESS,
    CARRIER_EVENT_FILE_COMPLETE,
    CARRIER_EVENT_CALL,
    CARRIER_EVENT_CALL_STATE,
    CARRIER_EVENT_AUDIO_FRAME,
    CARRIER_EVENT_VIDEO_FRAME,
    CARRIER_EVENT_PIPE,
    CARRIER_EVENT_PIPE_DATA,
    CARRIER_EVENT_PIPE_EOF,
    CARRIER_EVENT_DHT_INFO,
    CARRIER_EVENT_ERROR,
    CARRIER_EVENT_SYSTEM,
} CarrierEventType;

/* ---------------------------------------------------------------------------
 * Event data
 * ---------------------------------------------------------------------------*/

#define CARRIER_MAX_NAME_LENGTH     128
#define CARRIER_MAX_MESSAGE_LENGTH  4096
#define CARRIER_MAX_ID_LENGTH       128
#define CARRIER_MAX_KEY_LENGTH      128

typedef struct {
    CarrierEventType type;
    int64_t timestamp;

    union {
        struct { int transport; } connected;

        struct { char id[CARRIER_MAX_ID_LENGTH]; } self_id;

        struct {
            uint32_t friend_id;
            char name[CARRIER_MAX_NAME_LENGTH];
            char text[CARRIER_MAX_MESSAGE_LENGTH];
        } text_message;

        struct { uint32_t friend_id; uint32_t receipt; } message_sent;

        struct {
            uint32_t request_id;
            char key[CARRIER_MAX_KEY_LENGTH];
            char message[CARRIER_MAX_MESSAGE_LENGTH];
        } friend_request;

        struct {
            uint32_t friend_id;
            char name[CARRIER_MAX_NAME_LENGTH];
        } friend_online;

        struct { uint32_t friend_id; } friend_offline;

        struct {
            uint32_t friend_id;
            char name[CARRIER_MAX_NAME_LENGTH];
        } nick;

        struct {
            uint32_t friend_id;
            int status;
        } status;

        struct {
            uint32_t friend_id;
            char text[CARRIER_MAX_MESSAGE_LENGTH];
        } status_message;

        struct {
            uint32_t group_id;
            uint32_t peer_id;
            char name[CARRIER_MAX_NAME_LENGTH];
            char text[CARRIER_MAX_MESSAGE_LENGTH];
        } group_message;

        struct {
            uint32_t group_id;
            uint32_t peer_id;
            char name[CARRIER_MAX_NAME_LENGTH];
        } group_peer_join;

        struct {
            uint32_t group_id;
            uint32_t peer_id;
            char name[CARRIER_MAX_NAME_LENGTH];
        } group_peer_exit;

        struct {
            uint32_t friend_id;
            uint32_t group_id;
            char name[CARRIER_MAX_NAME_LENGTH];
        } group_invite;

        struct { uint32_t group_id; } group_self_join;

        struct {
            uint32_t friend_id;
            uint32_t file_id;
            uint64_t file_size;
            char filename[CARRIER_MAX_NAME_LENGTH];
        } file_transfer;

        struct {
            uint32_t friend_id;
            uint32_t file_id;
            double progress;
        } file_progress;

        struct {
            uint32_t friend_id;
            uint32_t file_id;
        } file_complete;

        struct {
            uint32_t friend_id;
            bool audio;
            bool video;
        } call;

        struct {
            uint32_t friend_id;
            uint32_t state;
        } call_state;

        struct {
            uint32_t friend_id;
            const int16_t *pcm;
            size_t samples;
            uint8_t channels;
            uint32_t sample_rate;
        } audio_frame;

        struct {
            uint32_t friend_id;
            uint16_t width;
            uint16_t height;
            const uint8_t *y;
            const uint8_t *u;
            const uint8_t *v;
        } video_frame;

        struct { uint32_t friend_id; } pipe;

        struct {
            uint32_t friend_id;
            const uint8_t *data;
            size_t len;
        } pipe_data;

        struct { uint32_t friend_id; } pipe_eof;

        struct {
            char key[CARRIER_MAX_KEY_LENGTH];
            uint16_t port;
        } dht_info;

        struct { char text[CARRIER_MAX_MESSAGE_LENGTH]; } system;

        struct {
            char cmd[64];
            char text[CARRIER_MAX_MESSAGE_LENGTH];
        } error;
    };
} CarrierEvent;

/* ---------------------------------------------------------------------------
 * Callback
 * ---------------------------------------------------------------------------*/

typedef void (*carrier_event_cb)(const CarrierEvent *event, void *userdata);

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

/*
 * Create a new Carrier instance.
 *
 * profile_path: path to .tox profile file (created if doesn't exist)
 * config_path:  path to config file, or NULL for defaults
 * nodes_path:   path to DHT nodes JSON file, or NULL to skip bootstrap
 *
 * Returns NULL on failure.
 */
Carrier *carrier_new(const char *profile_path,
                     const char *config_path,
                     const char *nodes_path);

/*
 * Free a Carrier instance and all associated resources.
 * Saves the profile before shutting down.
 */
void carrier_free(Carrier *c);

/*
 * Process Tox events. Must be called periodically.
 * Returns 0 on success, -1 on error.
 */
int carrier_iterate(Carrier *c);

/*
 * Suggested interval (in ms) between carrier_iterate() calls.
 */
int carrier_iteration_interval(Carrier *c);

/* ---------------------------------------------------------------------------
 * Event subscription
 * ---------------------------------------------------------------------------*/

void carrier_set_event_callback(Carrier *c, carrier_event_cb cb, void *userdata);

/* ---------------------------------------------------------------------------
 * Identity & status
 * ---------------------------------------------------------------------------*/

int carrier_get_id(Carrier *c);
int carrier_get_dht_info(Carrier *c);
int carrier_bootstrap(Carrier *c, const char *host, uint16_t port,
                      const char *key_hex);
int carrier_set_nick(Carrier *c, const char *nick);
int carrier_set_status(Carrier *c, int status);
int carrier_set_status_message(Carrier *c, const char *msg);

/* ---------------------------------------------------------------------------
 * Friends
 * ---------------------------------------------------------------------------*/

int carrier_add_friend(Carrier *c, const char *tox_id, const char *message);
int carrier_accept_friend(Carrier *c, uint32_t request_id);
int carrier_decline_friend(Carrier *c, uint32_t request_id);
int carrier_delete_friend(Carrier *c, uint32_t friend_id);
int carrier_send_message(Carrier *c, uint32_t friend_id, const char *text);

/* ---------------------------------------------------------------------------
 * File transfers
 * ---------------------------------------------------------------------------*/

int carrier_send_file(Carrier *c, uint32_t friend_id, const char *path);
int carrier_accept_file(Carrier *c, uint32_t friend_id, uint32_t file_id,
                        const char *save_path);
int carrier_cancel_file(Carrier *c, uint32_t friend_id, uint32_t file_id);

/* ---------------------------------------------------------------------------
 * Groups
 * ---------------------------------------------------------------------------*/

int carrier_create_group(Carrier *c, const char *name, bool is_public);
int carrier_join_group(Carrier *c, uint32_t friend_id);
int carrier_leave_group(Carrier *c, uint32_t group_id);
int carrier_send_group_message(Carrier *c, uint32_t group_id, const char *text);
int carrier_invite_to_group(Carrier *c, uint32_t group_id, uint32_t friend_id);

/* ---------------------------------------------------------------------------
 * Audio/Video calls (signaling + raw PCM/frames, no hardware access)
 * ---------------------------------------------------------------------------*/

int carrier_call(Carrier *c, uint32_t friend_id, bool audio, bool video);
int carrier_answer(Carrier *c, uint32_t friend_id, bool audio, bool video);
int carrier_hangup(Carrier *c, uint32_t friend_id);

int carrier_send_audio(Carrier *c, uint32_t friend_id,
                       const int16_t *pcm, size_t samples,
                       uint8_t channels, uint32_t sample_rate);

int carrier_send_video(Carrier *c, uint32_t friend_id,
                       const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       uint16_t width, uint16_t height);

int carrier_set_audio_bitrate(Carrier *c, uint32_t friend_id, uint32_t bitrate);
int carrier_set_video_bitrate(Carrier *c, uint32_t friend_id, uint32_t bitrate);

/* ---------------------------------------------------------------------------
 * Pipe mode (raw bidirectional data over Tox lossless packets)
 * ---------------------------------------------------------------------------*/

/*
 * Open a pipe to a friend. Incoming data arrives as CARRIER_EVENT_PIPE_DATA,
 * EOF as CARRIER_EVENT_PIPE_EOF.
 */
int carrier_pipe_open(Carrier *c, uint32_t friend_id);

/*
 * Send raw bytes through the pipe. Automatically chunked to fit
 * lossless packet size limits. Returns bytes sent, or -1 on error.
 */
int carrier_pipe_write(Carrier *c, uint32_t friend_id,
                       const uint8_t *data, size_t len);

/*
 * Signal EOF — no more data will be sent on this pipe.
 */
int carrier_pipe_close(Carrier *c, uint32_t friend_id);

/* ---------------------------------------------------------------------------
 * Profile persistence
 * ---------------------------------------------------------------------------*/

int carrier_save(Carrier *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CARRIER_H */
