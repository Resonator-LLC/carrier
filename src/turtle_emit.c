/*  turtle_emit.c
 *
 *  Serialize CarrierEvents to compact RDF Turtle (one line per statement).
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Escape a string for Turtle literal: replace \ with \\ and " with \" */
static void turtle_escape(FILE *out, const char *s)
{
    for (; *s; s++) {
        if (*s == '"') {
            fputs("\\\"", out);
        } else if (*s == '\\') {
            fputs("\\\\", out);
        } else if (*s == '\n') {
            fputs("\\n", out);
        } else if (*s == '\r') {
            fputs("\\r", out);
        } else {
            fputc(*s, out);
        }
    }
}

static void emit_timestamp(FILE *out, int64_t ts_ms)
{
    time_t ts = (time_t)(ts_ms / 1000);
    struct tm tm;
    gmtime_r(&ts, &tm);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    fprintf(out, " ; carrier:at \"%s\"^^xsd:dateTime", buf);
}

/* Detect whether a message payload is a Carrier Turtle statement.
 * Looks for "[] a carrier:" near the start (after optional whitespace).
 */
static int is_turtle(const char *text)
{
    const char *p = text;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    return strncmp(p, "[] a carrier:", 13) == 0;
}

/* Emit a received Turtle statement, appending receiver metadata.
 * Types are unified — no rewriting needed. Just strip the trailing dot
 * and append the extra predicates.
 */
static void emit_turtle_passthrough(FILE *out, const char *turtle,
                                    uint32_t friend_id, const char *name,
                                    int64_t ts_ms)
{
    /* Find trailing "." to strip it */
    const char *last_dot = NULL;

    for (const char *p = turtle; *p; p++) {
        if (*p == '.') {
            last_dot = p;
        }
    }

    if (last_dot != NULL) {
        fwrite(turtle, 1, (size_t)(last_dot - turtle), out);
    } else {
        fputs(turtle, out);
    }

    /* Append receiver metadata */
    fprintf(out, " ; carrier:friendId %u ; carrier:name \"", friend_id);
    turtle_escape(out, name);
    fprintf(out, "\"");
    emit_timestamp(out, ts_ms);

    fprintf(out, " .\n");
    fflush(out);
}

void turtle_emit_prefixes(FILE *out)
{
    fprintf(out, "@prefix carrier: <http://resonator.network/v2/carrier#> .\n");
    fprintf(out, "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n");
    fflush(out);
}

void turtle_emit_event(const CarrierEvent *ev, void *userdata)
{
    FILE *out = (FILE *)userdata;

    if (out == NULL || ev == NULL) {
        return;
    }

    switch (ev->type) {
        case CARRIER_EVENT_CONNECTED:
            fprintf(out, "[] a carrier:Connected ; carrier:transport \"%s\"",
                    ev->connected.transport == 1 ? "TCP" : "UDP");
            emit_timestamp(out, ev->timestamp);
            break;

        case CARRIER_EVENT_DISCONNECTED:
            fprintf(out, "[] a carrier:Disconnected");
            emit_timestamp(out, ev->timestamp);
            break;

        case CARRIER_EVENT_SELF_ID:
            fprintf(out, "[] a carrier:SelfId ; carrier:id \"%s\"", ev->self_id.id);
            break;

        case CARRIER_EVENT_TEXT_MESSAGE:
            if (is_turtle(ev->text_message.text)) {
                emit_turtle_passthrough(out, ev->text_message.text,
                                        ev->text_message.friend_id,
                                        ev->text_message.name,
                                        ev->timestamp);
                return;
            }

            fprintf(out, "[] a carrier:TextMessage ; carrier:friendId %u ; carrier:name \"",
                    ev->text_message.friend_id);
            turtle_escape(out, ev->text_message.name);
            fprintf(out, "\" ; carrier:text \"");
            turtle_escape(out, ev->text_message.text);
            fprintf(out, "\"");
            emit_timestamp(out, ev->timestamp);
            break;

        case CARRIER_EVENT_MESSAGE_SENT:
            fprintf(out, "[] a carrier:MessageSent ; carrier:friendId %u ; carrier:receipt %u",
                    ev->message_sent.friend_id, ev->message_sent.receipt);
            break;

        case CARRIER_EVENT_FRIEND_REQUEST:
            if (is_turtle(ev->friend_request.message)) {
                /* Passthrough: strip trailing dot, append requestId + key */
                const char *turtle = ev->friend_request.message;
                const char *last_dot = NULL;

                for (const char *p = turtle; *p; p++) {
                    if (*p == '.') {
                        last_dot = p;
                    }
                }

                if (last_dot) {
                    fwrite(turtle, 1, (size_t)(last_dot - turtle), out);
                } else {
                    fputs(turtle, out);
                }

                fprintf(out, " ; carrier:requestId %u ; carrier:key \"",
                        ev->friend_request.request_id);
                turtle_escape(out, ev->friend_request.key);
                fprintf(out, "\" .\n");
                fflush(out);
                return;
            }

            fprintf(out, "[] a carrier:FriendRequest ; carrier:requestId %u ; carrier:key \"",
                    ev->friend_request.request_id);
            turtle_escape(out, ev->friend_request.key);
            fprintf(out, "\" ; carrier:message \"");
            turtle_escape(out, ev->friend_request.message);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_FRIEND_ONLINE:
            fprintf(out, "[] a carrier:FriendOnline ; carrier:friendId %u ; carrier:name \"",
                    ev->friend_online.friend_id);
            turtle_escape(out, ev->friend_online.name);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_FRIEND_OFFLINE:
            fprintf(out, "[] a carrier:FriendOffline ; carrier:friendId %u",
                    ev->friend_offline.friend_id);
            break;

        case CARRIER_EVENT_NICK:
            if (is_turtle(ev->nick.name)) {
                /* Passthrough: append friendId */
                const char *turtle = ev->nick.name;
                const char *last_dot = NULL;

                for (const char *p = turtle; *p; p++) {
                    if (*p == '.') {
                        last_dot = p;
                    }
                }

                if (last_dot) {
                    fwrite(turtle, 1, (size_t)(last_dot - turtle), out);
                } else {
                    fputs(turtle, out);
                }

                fprintf(out, " ; carrier:friendId %u .\n", ev->nick.friend_id);
                fflush(out);
                return;
            }

            fprintf(out, "[] a carrier:Nick ; carrier:friendId %u ; carrier:nick \"",
                    ev->nick.friend_id);
            turtle_escape(out, ev->nick.name);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_STATUS:
            fprintf(out, "[] a carrier:Status ; carrier:friendId %u ; carrier:status %d",
                    ev->status.friend_id, ev->status.status);
            break;

        case CARRIER_EVENT_STATUS_MESSAGE:
            if (is_turtle(ev->status_message.text)) {
                /* Passthrough: append friendId */
                const char *turtle = ev->status_message.text;
                const char *last_dot = NULL;

                for (const char *p = turtle; *p; p++) {
                    if (*p == '.') {
                        last_dot = p;
                    }
                }

                if (last_dot) {
                    fwrite(turtle, 1, (size_t)(last_dot - turtle), out);
                } else {
                    fputs(turtle, out);
                }

                fprintf(out, " ; carrier:friendId %u .\n",
                        ev->status_message.friend_id);
                fflush(out);
                return;
            }

            fprintf(out, "[] a carrier:StatusMessage ; carrier:friendId %u ; carrier:text \"",
                    ev->status_message.friend_id);
            turtle_escape(out, ev->status_message.text);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_GROUP_MESSAGE:
            if (is_turtle(ev->group_message.text)) {
                /* Passthrough: strip dot, append group metadata */
                const char *turtle = ev->group_message.text;
                const char *last_dot = NULL;

                for (const char *p = turtle; *p; p++) {
                    if (*p == '.') {
                        last_dot = p;
                    }
                }

                if (last_dot) {
                    fwrite(turtle, 1, (size_t)(last_dot - turtle), out);
                } else {
                    fputs(turtle, out);
                }

                fprintf(out, " ; carrier:peerId %u ; carrier:name \"",
                        ev->group_message.peer_id);
                turtle_escape(out, ev->group_message.name);
                fprintf(out, "\"");
                emit_timestamp(out, ev->timestamp);
                fprintf(out, " .\n");
                fflush(out);
                return;
            }

            fprintf(out, "[] a carrier:GroupMessage ; carrier:groupId %u ; carrier:peerId %u ; carrier:name \"",
                    ev->group_message.group_id, ev->group_message.peer_id);
            turtle_escape(out, ev->group_message.name);
            fprintf(out, "\" ; carrier:text \"");
            turtle_escape(out, ev->group_message.text);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_GROUP_PEER_JOIN:
            fprintf(out, "[] a carrier:GroupPeerJoin ; carrier:groupId %u ; carrier:peerId %u ; carrier:name \"",
                    ev->group_peer_join.group_id, ev->group_peer_join.peer_id);
            turtle_escape(out, ev->group_peer_join.name);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_GROUP_PEER_EXIT:
            fprintf(out, "[] a carrier:GroupPeerExit ; carrier:groupId %u ; carrier:peerId %u",
                    ev->group_peer_exit.group_id, ev->group_peer_exit.peer_id);
            break;

        case CARRIER_EVENT_GROUP_INVITE:
            fprintf(out, "[] a carrier:GroupInvite ; carrier:friendId %u ; carrier:name \"",
                    ev->group_invite.friend_id);
            turtle_escape(out, ev->group_invite.name);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_GROUP_SELF_JOIN:
            fprintf(out, "[] a carrier:GroupSelfJoin ; carrier:groupId %u",
                    ev->group_self_join.group_id);
            break;

        case CARRIER_EVENT_CONFERENCE_MESSAGE:
        case CARRIER_EVENT_CONFERENCE_INVITE:
            /* TODO */
            return;

        case CARRIER_EVENT_FILE_TRANSFER:
            fprintf(out, "[] a carrier:FileTransfer ; carrier:friendId %u ; carrier:fileId %u ; carrier:size %lu ; carrier:filename \"",
                    ev->file_transfer.friend_id, ev->file_transfer.file_id,
                    (unsigned long)ev->file_transfer.file_size);
            turtle_escape(out, ev->file_transfer.filename);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_FILE_SEND_STARTED:
            fprintf(out, "[] a carrier:SendFileStarted ; carrier:friendId %u ; carrier:fileId %u ; carrier:size %lu ; carrier:filename \"",
                    ev->file_send_started.friend_id, ev->file_send_started.file_id,
                    (unsigned long)ev->file_send_started.file_size);
            turtle_escape(out, ev->file_send_started.filename);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_FILE_PROGRESS:
            fprintf(out, "[] a carrier:FileProgress ; carrier:friendId %u ; carrier:fileId %u ; carrier:progress %.4f ; carrier:bytesTransferred %lu ; carrier:direction \"%s\"",
                    ev->file_progress.friend_id, ev->file_progress.file_id,
                    ev->file_progress.progress,
                    (unsigned long)ev->file_progress.bytes_transferred,
                    ev->file_progress.outbound ? "out" : "in");
            break;

        case CARRIER_EVENT_FILE_COMPLETE:
            fprintf(out, "[] a carrier:FileComplete ; carrier:friendId %u ; carrier:fileId %u ; carrier:direction \"%s\" ; carrier:cancelled %s",
                    ev->file_complete.friend_id, ev->file_complete.file_id,
                    ev->file_complete.outbound ? "out" : "in",
                    ev->file_complete.cancelled ? "true" : "false");
            break;

        case CARRIER_EVENT_CALL:
            fprintf(out, "[] a carrier:Call ; carrier:friendId %u ; carrier:audio %s ; carrier:video %s",
                    ev->call.friend_id,
                    ev->call.audio ? "true" : "false",
                    ev->call.video ? "true" : "false");
            break;

        case CARRIER_EVENT_CALL_STATE:
            fprintf(out, "[] a carrier:CallState ; carrier:friendId %u ; carrier:state %u",
                    ev->call_state.friend_id, ev->call_state.state);
            break;

        case CARRIER_EVENT_PIPE:
            fprintf(out, "[] a carrier:Pipe ; carrier:friendId %u",
                    ev->pipe.friend_id);
            break;

        case CARRIER_EVENT_PIPE_DATA:
            /* Binary data — not serialized to Turtle */
            return;

        case CARRIER_EVENT_PIPE_EOF:
            fprintf(out, "[] a carrier:PipeEof ; carrier:friendId %u",
                    ev->pipe_eof.friend_id);
            break;

        case CARRIER_EVENT_AUDIO_FRAME:
        case CARRIER_EVENT_VIDEO_FRAME:
            /* Binary data — not serialized to Turtle */
            return;

        case CARRIER_EVENT_DHT_INFO:
            fprintf(out, "[] a carrier:DhtInfo ; carrier:key \"%s\" ; carrier:port %u",
                    ev->dht_info.key, ev->dht_info.port);
            break;

        case CARRIER_EVENT_ERROR:
            fprintf(out, "[] a carrier:Error ; carrier:cmd \"");
            turtle_escape(out, ev->error.cmd);
            fprintf(out, "\" ; carrier:message \"");
            turtle_escape(out, ev->error.text);
            fprintf(out, "\"");
            break;

        case CARRIER_EVENT_SYSTEM:
            fprintf(out, "[] a carrier:System ; carrier:message \"");
            turtle_escape(out, ev->system.text);
            fprintf(out, "\"");
            break;
    }

    fprintf(out, " .\n");
    fflush(out);
}
