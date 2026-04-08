/*  test_turtle_emit.c — Tests for Turtle event serialization.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the GNU General Public License 3.0.
 */

#include "test.h"
#include "carrier.h"
#include "turtle_emit.h"

#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/

/* Emit an event to a string buffer using tmpfile */
static char *emit_to_string(const CarrierEvent *ev)
{
    FILE *f = tmpfile();

    if (!f) return NULL;

    turtle_emit_event(ev, f);

    long len = ftell(f);
    rewind(f);

    char *buf = (char *)calloc(1, (size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    fclose(f);

    return buf;
}

static char *emit_prefixes(void)
{
    FILE *f = tmpfile();

    if (!f) return NULL;

    turtle_emit_prefixes(f);

    long len = ftell(f);
    rewind(f);

    char *buf = (char *)calloc(1, (size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    fclose(f);

    return buf;
}

/* ---------------------------------------------------------------------------
 * Tests: prefix declarations
 * ---------------------------------------------------------------------------*/

TEST(test_emit_prefixes)
{
    char *out = emit_prefixes();
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "@prefix carrier:");
    ASSERT_STR_CONTAINS(out, "<http://resonator.network/v2/carrier#>");
    ASSERT_STR_CONTAINS(out, "@prefix xsd:");
    ASSERT_STR_CONTAINS(out, "<http://www.w3.org/2001/XMLSchema#>");
    free(out);
}

/* ---------------------------------------------------------------------------
 * Tests: event serialization
 * ---------------------------------------------------------------------------*/

TEST(test_emit_connected_udp)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_CONNECTED;
    ev.connected.transport = 0;
    ev.timestamp = 1711180800000LL; /* 2024-03-23T08:00:00Z */

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:Connected");
    ASSERT_STR_CONTAINS(out, "carrier:transport \"UDP\"");
    ASSERT_STR_CONTAINS(out, "carrier:at \"");
    ASSERT_STR_CONTAINS(out, " .\n");
    free(out);
}

TEST(test_emit_connected_tcp)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_CONNECTED;
    ev.connected.transport = 1;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "carrier:transport \"TCP\"");
    free(out);
}

TEST(test_emit_disconnected)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_DISCONNECTED;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:Disconnected");
    ASSERT_STR_CONTAINS(out, " .\n");
    free(out);
}

TEST(test_emit_self_id)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_SELF_ID;
    snprintf(ev.self_id.id, sizeof(ev.self_id.id), "ABCDEF1234567890");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:SelfId");
    ASSERT_STR_CONTAINS(out, "carrier:id \"ABCDEF1234567890\"");
    free(out);
}

TEST(test_emit_text_message)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_TEXT_MESSAGE;
    ev.text_message.friend_id = 3;
    snprintf(ev.text_message.name, sizeof(ev.text_message.name), "Alice");
    snprintf(ev.text_message.text, sizeof(ev.text_message.text), "Hello world");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:TextMessage");
    ASSERT_STR_CONTAINS(out, "carrier:friendId 3");
    ASSERT_STR_CONTAINS(out, "carrier:name \"Alice\"");
    ASSERT_STR_CONTAINS(out, "carrier:text \"Hello world\"");
    free(out);
}

TEST(test_emit_text_message_escaping)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_TEXT_MESSAGE;
    ev.text_message.friend_id = 0;
    snprintf(ev.text_message.name, sizeof(ev.text_message.name), "Bob \"The\" Builder");
    snprintf(ev.text_message.text, sizeof(ev.text_message.text), "line1\nline2\\end");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "Bob \\\"The\\\" Builder");
    ASSERT_STR_CONTAINS(out, "line1\\nline2\\\\end");
    free(out);
}

TEST(test_emit_friend_request)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_FRIEND_REQUEST;
    ev.friend_request.request_id = 7;
    snprintf(ev.friend_request.key, sizeof(ev.friend_request.key), "DEADBEEF");
    snprintf(ev.friend_request.message, sizeof(ev.friend_request.message), "Add me!");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:FriendRequest");
    ASSERT_STR_CONTAINS(out, "carrier:requestId 7");
    ASSERT_STR_CONTAINS(out, "carrier:key \"DEADBEEF\"");
    ASSERT_STR_CONTAINS(out, "carrier:message \"Add me!\"");
    free(out);
}

TEST(test_emit_friend_online)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_FRIEND_ONLINE;
    ev.friend_online.friend_id = 2;
    snprintf(ev.friend_online.name, sizeof(ev.friend_online.name), "Charlie");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:FriendOnline");
    ASSERT_STR_CONTAINS(out, "carrier:friendId 2");
    ASSERT_STR_CONTAINS(out, "carrier:name \"Charlie\"");
    free(out);
}

TEST(test_emit_friend_offline)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_FRIEND_OFFLINE;
    ev.friend_offline.friend_id = 5;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:FriendOffline");
    ASSERT_STR_CONTAINS(out, "carrier:friendId 5");
    free(out);
}

TEST(test_emit_nick)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_NICK;
    ev.nick.friend_id = 1;
    snprintf(ev.nick.name, sizeof(ev.nick.name), "NewNick");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:Nick");
    ASSERT_STR_CONTAINS(out, "carrier:friendId 1");
    ASSERT_STR_CONTAINS(out, "carrier:nick \"NewNick\"");
    free(out);
}

TEST(test_emit_status)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_STATUS;
    ev.status.friend_id = 4;
    ev.status.status = 2;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:Status");
    ASSERT_STR_CONTAINS(out, "carrier:friendId 4");
    ASSERT_STR_CONTAINS(out, "carrier:status 2");
    free(out);
}

TEST(test_emit_error)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_ERROR;
    snprintf(ev.error.cmd, sizeof(ev.error.cmd), "FriendRequest");
    snprintf(ev.error.text, sizeof(ev.error.text), "Invalid Tox ID");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:Error");
    ASSERT_STR_CONTAINS(out, "carrier:cmd \"FriendRequest\"");
    ASSERT_STR_CONTAINS(out, "carrier:message \"Invalid Tox ID\"");
    free(out);
}

TEST(test_emit_system)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_SYSTEM;
    snprintf(ev.system.text, sizeof(ev.system.text), "Friend added as #0");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:System");
    ASSERT_STR_CONTAINS(out, "carrier:message \"Friend added as #0\"");
    free(out);
}

TEST(test_emit_file_transfer)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_FILE_TRANSFER;
    ev.file_transfer.friend_id = 1;
    ev.file_transfer.file_id = 42;
    ev.file_transfer.file_size = 1048576;
    snprintf(ev.file_transfer.filename, sizeof(ev.file_transfer.filename), "photo.jpg");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:FileTransfer");
    ASSERT_STR_CONTAINS(out, "carrier:friendId 1");
    ASSERT_STR_CONTAINS(out, "carrier:fileId 42");
    ASSERT_STR_CONTAINS(out, "carrier:filename \"photo.jpg\"");
    free(out);
}

TEST(test_emit_call)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_CALL;
    ev.call.friend_id = 0;
    ev.call.audio = true;
    ev.call.video = false;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:Call");
    ASSERT_STR_CONTAINS(out, "carrier:audio true");
    ASSERT_STR_CONTAINS(out, "carrier:video false");
    free(out);
}

TEST(test_emit_dht_info)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_DHT_INFO;
    snprintf(ev.dht_info.key, sizeof(ev.dht_info.key), "ABC123");
    ev.dht_info.port = 33445;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:DhtInfo");
    ASSERT_STR_CONTAINS(out, "carrier:key \"ABC123\"");
    ASSERT_STR_CONTAINS(out, "carrier:port 33445");
    free(out);
}

TEST(test_emit_pipe_eof)
{
    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_PIPE_EOF;
    ev.pipe_eof.friend_id = 8;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:PipeEof");
    ASSERT_STR_CONTAINS(out, "carrier:friendId 8");
    free(out);
}

TEST(test_emit_null_safety)
{
    /* Should not crash */
    turtle_emit_event(NULL, stdout);

    CarrierEvent ev = {0};
    ev.type = CARRIER_EVENT_CONNECTED;
    turtle_emit_event(&ev, NULL);
}

TEST(test_emit_all_lines_end_with_dot_newline)
{
    /* Verify several event types all produce lines ending with " .\n" */
    CarrierEventType types[] = {
        CARRIER_EVENT_CONNECTED,
        CARRIER_EVENT_DISCONNECTED,
        CARRIER_EVENT_SELF_ID,
        CARRIER_EVENT_FRIEND_OFFLINE,
        CARRIER_EVENT_ERROR,
        CARRIER_EVENT_SYSTEM,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        CarrierEvent ev = {0};
        ev.type = types[i];

        char *out = emit_to_string(&ev);
        ASSERT(out != NULL);

        size_t len = strlen(out);
        ASSERT(len >= 3);
        ASSERT(out[len - 1] == '\n');
        ASSERT(out[len - 2] == '.');

        free(out);
    }
}

/* ---------------------------------------------------------------------------
 * Suite runner
 * ---------------------------------------------------------------------------*/

void test_turtle_emit_all(void)
{
    printf("[turtle_emit]\n");

    RUN_TEST(test_emit_prefixes);
    RUN_TEST(test_emit_connected_udp);
    RUN_TEST(test_emit_connected_tcp);
    RUN_TEST(test_emit_disconnected);
    RUN_TEST(test_emit_self_id);
    RUN_TEST(test_emit_text_message);
    RUN_TEST(test_emit_text_message_escaping);
    RUN_TEST(test_emit_friend_request);
    RUN_TEST(test_emit_friend_online);
    RUN_TEST(test_emit_friend_offline);
    RUN_TEST(test_emit_nick);
    RUN_TEST(test_emit_status);
    RUN_TEST(test_emit_error);
    RUN_TEST(test_emit_system);
    RUN_TEST(test_emit_file_transfer);
    RUN_TEST(test_emit_call);
    RUN_TEST(test_emit_dht_info);
    RUN_TEST(test_emit_pipe_eof);
    RUN_TEST(test_emit_null_safety);
    RUN_TEST(test_emit_all_lines_end_with_dot_newline);
}
