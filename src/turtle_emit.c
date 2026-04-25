/*  turtle_emit.c
 *
 *  Serialize v2 CarrierEvents to compact RDF Turtle (one statement per event).
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Escape a string for a Turtle literal: \, ", \n, \r. */
static void turtle_escape(FILE *out, const char *s)
{
    if (s == NULL) return;
    for (; *s; s++) {
        if      (*s == '"')  fputs("\\\"", out);
        else if (*s == '\\') fputs("\\\\", out);
        else if (*s == '\n') fputs("\\n",  out);
        else if (*s == '\r') fputs("\\r",  out);
        else                 fputc(*s, out);
    }
}

static void turtle_escape_n(FILE *out, const char *s, size_t n)
{
    if (s == NULL) return;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if      (ch == '"')  fputs("\\\"", out);
        else if (ch == '\\') fputs("\\\\", out);
        else if (ch == '\n') fputs("\\n",  out);
        else if (ch == '\r') fputs("\\r",  out);
        else                 fputc(ch, out);
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

/* Emit the leading `[] a carrier:<type> ; carrier:account "<id>"` common
 * prefix. Caller appends event-specific predicates and the closing ` .\n`. */
static void emit_header(FILE *out, const char *type, const char *account_id)
{
    fprintf(out, "[] a carrier:%s", type);
    if (account_id && account_id[0]) {
        fprintf(out, " ; carrier:account \"");
        turtle_escape(out, account_id);
        fputc('"', out);
    }
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
    if (out == NULL || ev == NULL) return;

    switch (ev->type) {
        case CARRIER_EVENT_CONNECTED:
            emit_header(out, "Connected", ev->account_id);
            break;

        case CARRIER_EVENT_DISCONNECTED:
            emit_header(out, "Disconnected", ev->account_id);
            break;

        case CARRIER_EVENT_ACCOUNT_READY:
            emit_header(out, "AccountReady", ev->account_id);
            fprintf(out, " ; carrier:selfUri \"");
            turtle_escape(out, ev->account_ready.self_uri);
            fputc('"', out);
            if (ev->account_ready.display_name[0]) {
                fprintf(out, " ; carrier:displayName \"");
                turtle_escape(out, ev->account_ready.display_name);
                fputc('"', out);
            }
            break;

        case CARRIER_EVENT_ACCOUNT_ERROR:
            emit_header(out, "AccountError", ev->account_id);
            fprintf(out, " ; carrier:cause \"");
            if (ev->account_error.cause) turtle_escape(out, ev->account_error.cause);
            fputc('"', out);
            break;

        case CARRIER_EVENT_SELF_ID:
            emit_header(out, "SelfId", ev->account_id);
            fprintf(out, " ; carrier:selfUri \"");
            turtle_escape(out, ev->self_id.self_uri);
            fputc('"', out);
            break;

        case CARRIER_EVENT_TRUST_REQUEST:
            emit_header(out, "TrustRequest", ev->account_id);
            fprintf(out, " ; carrier:contactUri \"");
            turtle_escape(out, ev->trust_request.from_uri);
            fputc('"', out);
            if (ev->trust_request.payload && ev->trust_request.payload_len > 0) {
                fprintf(out, " ; carrier:payload \"");
                turtle_escape_n(out, ev->trust_request.payload,
                                ev->trust_request.payload_len);
                fputc('"', out);
            }
            break;

        case CARRIER_EVENT_CONTACT_ONLINE:
            emit_header(out, "ContactOnline", ev->account_id);
            fprintf(out, " ; carrier:contactUri \"");
            turtle_escape(out, ev->contact_online.contact_uri);
            fputc('"', out);
            break;

        case CARRIER_EVENT_CONTACT_OFFLINE:
            emit_header(out, "ContactOffline", ev->account_id);
            fprintf(out, " ; carrier:contactUri \"");
            turtle_escape(out, ev->contact_offline.contact_uri);
            fputc('"', out);
            break;

        case CARRIER_EVENT_CONTACT_NAME:
            emit_header(out, "ContactName", ev->account_id);
            fprintf(out, " ; carrier:contactUri \"");
            turtle_escape(out, ev->contact_name.contact_uri);
            fprintf(out, "\" ; carrier:displayName \"");
            turtle_escape(out, ev->contact_name.display_name);
            fputc('"', out);
            break;

        case CARRIER_EVENT_TEXT_MESSAGE:
            emit_header(out, "TextMessage", ev->account_id);
            fprintf(out, " ; carrier:contactUri \"");
            turtle_escape(out, ev->text_message.contact_uri);
            fprintf(out, "\" ; carrier:conversationId \"");
            turtle_escape(out, ev->text_message.conversation_id);
            fprintf(out, "\" ; carrier:messageId %llu ; carrier:text \"",
                    (unsigned long long)ev->text_message.message_id);
            if (ev->text_message.text) {
                turtle_escape_n(out, ev->text_message.text, ev->text_message.text_len);
            }
            fputc('"', out);
            break;

        case CARRIER_EVENT_MESSAGE_SENT:
            emit_header(out, "MessageSent", ev->account_id);
            fprintf(out, " ; carrier:contactUri \"");
            turtle_escape(out, ev->message_sent.contact_uri);
            fprintf(out, "\" ; carrier:conversationId \"");
            turtle_escape(out, ev->message_sent.conversation_id);
            fprintf(out, "\" ; carrier:messageId %llu ; carrier:status %d",
                    (unsigned long long)ev->message_sent.message_id,
                    ev->message_sent.status);
            break;

        case CARRIER_EVENT_ERROR:
            emit_header(out, "Error", ev->account_id);
            fprintf(out, " ; carrier:command \"");
            turtle_escape(out, ev->error.command);
            fprintf(out, "\" ; carrier:class \"");
            turtle_escape(out, ev->error.class_);
            fprintf(out, "\" ; carrier:message \"");
            if (ev->error.text) turtle_escape(out, ev->error.text);
            fputc('"', out);
            break;

        case CARRIER_EVENT_SYSTEM:
            emit_header(out, "System", ev->account_id);
            fprintf(out, " ; carrier:message \"");
            if (ev->system.text) turtle_escape(out, ev->system.text);
            fputc('"', out);
            break;
    }

    emit_timestamp(out, ev->timestamp);
    fputs(" .\n", out);
    fflush(out);
}
