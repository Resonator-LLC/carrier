/*  turtle_parse.c
 *
 *  Turtle command parser for Carrier, powered by Serd.
 *  Parses full RDF 1.1 Turtle input and dispatches carrier_*() functions.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the GNU General Public License 3.0.
 */

#include "turtle_parse.h"
#include "carrier_events.h"

#include <serd/serd.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PREDICATES 8
#define MAX_VALUE_LEN  4096
#define CARRIER_NS     "http://resonator.network/v2/carrier#"
#define RDF_TYPE_URI   "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"

struct turtle_predicate {
    char name[64];
    char value[MAX_VALUE_LEN];
};

struct turtle_stmt {
    char type[64];
    struct turtle_predicate preds[MAX_PREDICATES];
    int num_preds;
};

struct parse_ctx {
    Carrier *carrier;
    struct turtle_stmt stmt;
    int result;
    SerdEnv *env;
};

/* Extract the local name from a resolved URI.
 * E.g. "http://resonator.network/v2/carrier#SetNick" → "SetNick"
 */
static const char *local_name(const char *uri)
{
    const char *hash = strrchr(uri, '#');

    if (hash) {
        return hash + 1;
    }

    const char *slash = strrchr(uri, '/');

    if (slash) {
        return slash + 1;
    }

    return uri;
}

/* Resolve a serd node to a full URI string using the environment.
 * Returns a malloc'd string that must be freed, or NULL.
 */
static char *resolve_node(struct parse_ctx *ctx, const SerdNode *node)
{
    if (node->type == SERD_URI) {
        return strndup((const char *)node->buf, node->n_bytes);
    }

    if (node->type == SERD_CURIE) {
        SerdNode uri = serd_env_expand_node(ctx->env, node);

        if (uri.buf) {
            char *result = strndup((const char *)uri.buf, uri.n_bytes);
            serd_node_free(&uri);
            return result;
        }
    }

    return NULL;
}

static SerdStatus on_prefix(void *handle, const SerdNode *name, const SerdNode *uri)
{
    struct parse_ctx *ctx = (struct parse_ctx *)handle;
    serd_env_set_prefix(ctx->env, name, uri);
    return SERD_SUCCESS;
}

static SerdStatus on_base(void *handle, const SerdNode *uri)
{
    struct parse_ctx *ctx = (struct parse_ctx *)handle;
    serd_env_set_base_uri(ctx->env, uri);
    return SERD_SUCCESS;
}

static SerdStatus on_statement(void *handle,
                               SerdStatementFlags flags,
                               const SerdNode *graph,
                               const SerdNode *subject,
                               const SerdNode *predicate,
                               const SerdNode *object,
                               const SerdNode *object_datatype,
                               const SerdNode *object_lang)
{
    (void)flags;
    (void)graph;
    (void)subject;
    (void)object_datatype;
    (void)object_lang;

    struct parse_ctx *ctx = (struct parse_ctx *)handle;
    struct turtle_stmt *stmt = &ctx->stmt;

    /* Resolve predicate URI */
    char *pred_uri = resolve_node(ctx, predicate);

    if (pred_uri == NULL) {
        return SERD_SUCCESS;
    }

    /* Check if this is rdf:type (the "a" keyword) */
    if (strcmp(pred_uri, RDF_TYPE_URI) == 0) {
        /* Object is the type — resolve it */
        char *type_uri = resolve_node(ctx, object);

        if (type_uri) {
            snprintf(stmt->type, sizeof(stmt->type), "%s", local_name(type_uri));
            free(type_uri);
        }

        free(pred_uri);
        return SERD_SUCCESS;
    }

    /* Regular predicate — extract local name and object value */
    if (stmt->num_preds < MAX_PREDICATES) {
        struct turtle_predicate *p = &stmt->preds[stmt->num_preds];
        snprintf(p->name, sizeof(p->name), "%s", local_name(pred_uri));

        if (object->type == SERD_LITERAL) {
            snprintf(p->value, sizeof(p->value), "%.*s",
                     (int)object->n_bytes, object->buf);
        } else {
            /* URI or CURIE or blank node — resolve or use raw */
            char *obj_uri = resolve_node(ctx, object);

            if (obj_uri) {
                snprintf(p->value, sizeof(p->value), "%s", local_name(obj_uri));
                free(obj_uri);
            } else {
                snprintf(p->value, sizeof(p->value), "%.*s",
                         (int)object->n_bytes, object->buf);
            }
        }

        stmt->num_preds++;
    }

    free(pred_uri);
    return SERD_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Dispatch: map parsed statement to carrier_*() calls
 * Same logic as before, just fed by serd instead of hand-rolled parser
 * ---------------------------------------------------------------------------*/

static const char *find_pred(const struct turtle_stmt *stmt, const char *name)
{
    for (int i = 0; i < stmt->num_preds; i++) {
        if (strcmp(stmt->preds[i].name, name) == 0) {
            return stmt->preds[i].value;
        }
    }

    return NULL;
}

static uint32_t find_pred_uint(const struct turtle_stmt *stmt, const char *name,
                               uint32_t default_val)
{
    const char *v = find_pred(stmt, name);

    if (v == NULL) {
        return default_val;
    }

    return (uint32_t)strtoul(v, NULL, 10);
}

static int dispatch_statement(Carrier *c, const struct turtle_stmt *stmt,
                              const char *raw_turtle)
{
    if (stmt->type[0] == '\0') {
        return 0;  /* No type — probably a prefix declaration */
    }

    if (strcmp(stmt->type, "SelfId") == 0) {
        return carrier_get_id(c);
    }

    if (strcmp(stmt->type, "DhtInfo") == 0) {
        return carrier_get_dht_info(c);
    }

    if (strcmp(stmt->type, "Bootstrap") == 0) {
        const char *host = find_pred(stmt, "host");
        uint32_t port = find_pred_uint(stmt, "port", 33445);
        const char *key = find_pred(stmt, "key");

        if (host == NULL || key == NULL) {
            carrier_emit_error(c, "Bootstrap", "Missing carrier:host or carrier:key");
            return -1;
        }

        return carrier_bootstrap(c, host, (uint16_t)port, key);
    }

    if (strcmp(stmt->type, "Nick") == 0) {
        const char *nick = find_pred(stmt, "nick");

        if (nick == NULL) {
            carrier_emit_error(c, "Nick", "Missing carrier:nick");
            return -1;
        }

        return carrier_set_nick(c, nick);
    }

    if (strcmp(stmt->type, "Status") == 0) {
        const char *status = find_pred(stmt, "status");

        if (status == NULL) {
            carrier_emit_error(c, "Status", "Missing carrier:status");
            return -1;
        }

        int s = 0;

        if (strcmp(status, "away") == 0) {
            s = 1;
        } else if (strcmp(status, "busy") == 0) {
            s = 2;
        }

        return carrier_set_status(c, s);
    }

    if (strcmp(stmt->type, "StatusMessage") == 0) {
        const char *msg = find_pred(stmt, "message");

        if (msg == NULL) {
            msg = find_pred(stmt, "text");
        }

        if (msg == NULL) {
            carrier_emit_error(c, "StatusMessage", "Missing carrier:message or carrier:text");
            return -1;
        }

        return carrier_set_status_message(c, raw_turtle);
    }

    if (strcmp(stmt->type, "FriendRequest") == 0) {
        const char *id = find_pred(stmt, "id");

        if (id == NULL) {
            carrier_emit_error(c, "FriendRequest", "Missing carrier:id");
            return -1;
        }

        return carrier_add_friend(c, id, raw_turtle);
    }

    if (strcmp(stmt->type, "FriendAccept") == 0) {
        uint32_t req_id = find_pred_uint(stmt, "requestId", UINT32_MAX);

        if (req_id == UINT32_MAX) {
            carrier_emit_error(c, "FriendAccept", "Missing carrier:requestId");
            return -1;
        }

        return carrier_accept_friend(c, req_id);
    }

    if (strcmp(stmt->type, "FriendDecline") == 0) {
        uint32_t req_id = find_pred_uint(stmt, "requestId", UINT32_MAX);
        return carrier_decline_friend(c, req_id);
    }

    if (strcmp(stmt->type, "TextMessage") == 0) {
        uint32_t fid = find_pred_uint(stmt, "friendId", UINT32_MAX);
        const char *text = find_pred(stmt, "text");

        if (fid == UINT32_MAX || text == NULL) {
            carrier_emit_error(c, "TextMessage", "Missing carrier:friendId or carrier:text");
            return -1;
        }

        return carrier_send_message(c, fid, raw_turtle);
    }

    if (strcmp(stmt->type, "GroupMessage") == 0) {
        uint32_t gid = find_pred_uint(stmt, "groupId", UINT32_MAX);
        const char *text = find_pred(stmt, "text");

        if (gid == UINT32_MAX || text == NULL) {
            carrier_emit_error(c, "GroupMessage", "Missing carrier:groupId or carrier:text");
            return -1;
        }

        return carrier_send_group_message(c, gid, raw_turtle);
    }

    if (strcmp(stmt->type, "FileTransfer") == 0) {
        uint32_t fid = find_pred_uint(stmt, "friendId", UINT32_MAX);
        const char *path = find_pred(stmt, "path");

        if (fid == UINT32_MAX || path == NULL) {
            carrier_emit_error(c, "FileTransfer", "Missing carrier:friendId or carrier:path");
            return -1;
        }

        return carrier_send_file(c, fid, path);
    }

    if (strcmp(stmt->type, "FileAccept") == 0) {
        uint32_t fid = find_pred_uint(stmt, "friendId", UINT32_MAX);
        uint32_t file_id = find_pred_uint(stmt, "fileId", UINT32_MAX);
        const char *path = find_pred(stmt, "path");
        return carrier_accept_file(c, fid, file_id, path);
    }

    if (strcmp(stmt->type, "Group") == 0) {
        const char *name = find_pred(stmt, "name");
        const char *privacy = find_pred(stmt, "privacy");
        bool is_public = (privacy != NULL && strcmp(privacy, "public") == 0);
        return carrier_create_group(c, name ? name : "Group", is_public);
    }

    if (strcmp(stmt->type, "GroupLeave") == 0) {
        uint32_t gid = find_pred_uint(stmt, "groupId", UINT32_MAX);
        return carrier_leave_group(c, gid);
    }

    if (strcmp(stmt->type, "Call") == 0) {
        uint32_t fid = find_pred_uint(stmt, "friendId", UINT32_MAX);
        const char *audio = find_pred(stmt, "audio");
        const char *video = find_pred(stmt, "video");
        return carrier_call(c, fid,
                            audio == NULL || strcmp(audio, "true") == 0,
                            video != NULL && strcmp(video, "true") == 0);
    }

    if (strcmp(stmt->type, "CallAnswer") == 0) {
        uint32_t fid = find_pred_uint(stmt, "friendId", UINT32_MAX);
        const char *audio = find_pred(stmt, "audio");
        const char *video = find_pred(stmt, "video");
        return carrier_answer(c, fid,
                              audio == NULL || strcmp(audio, "true") == 0,
                              video != NULL && strcmp(video, "true") == 0);
    }

    if (strcmp(stmt->type, "CallHangup") == 0) {
        uint32_t fid = find_pred_uint(stmt, "friendId", UINT32_MAX);
        return carrier_hangup(c, fid);
    }

    if (strcmp(stmt->type, "Pipe") == 0) {
        uint32_t fid = find_pred_uint(stmt, "friendId", UINT32_MAX);

        if (fid == UINT32_MAX) {
            carrier_emit_error(c, "Pipe", "Missing carrier:friendId");
            return -1;
        }

        carrier_pipe_open(c, fid);
        return 2;  /* Signal to enter pipe mode */
    }

    if (strcmp(stmt->type, "Save") == 0) {
        return carrier_save(c);
    }

    if (strcmp(stmt->type, "Quit") == 0) {
        return 1;  /* Signal to exit */
    }

    carrier_emit_error(c, "Parse", "Unknown command type: %s", stmt->type);
    return -2;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

int turtle_parse_and_execute(Carrier *c, const char *line)
{
    if (c == NULL || line == NULL) {
        return -1;
    }

    /* Skip empty lines and comments */
    const char *p = line;

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (*p == '\0' || *p == '#') {
        return 0;
    }

    struct parse_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.carrier = c;
    ctx.result = 0;

    /* Create serd environment for prefix resolution */
    ctx.env = serd_env_new(NULL);

    /* Pre-register our carrier prefix */
    SerdNode carrier_prefix = serd_node_from_string(SERD_LITERAL,
        (const uint8_t *)"carrier");
    SerdNode carrier_uri = serd_node_from_string(SERD_URI,
        (const uint8_t *)CARRIER_NS);
    serd_env_set_prefix(ctx.env, &carrier_prefix, &carrier_uri);

    /* Also register xsd prefix */
    SerdNode xsd_prefix = serd_node_from_string(SERD_LITERAL,
        (const uint8_t *)"xsd");
    SerdNode xsd_uri = serd_node_from_string(SERD_URI,
        (const uint8_t *)"http://www.w3.org/2001/XMLSchema#");
    serd_env_set_prefix(ctx.env, &xsd_prefix, &xsd_uri);

    /* Create serd reader */
    SerdReader *reader = serd_reader_new(
        SERD_TURTLE, &ctx, NULL,
        on_base, on_prefix, on_statement, NULL);

    /* Parse the input */
    serd_reader_read_string(reader, (const uint8_t *)line);

    /* Dispatch accumulated statement */
    if (ctx.stmt.type[0] != '\0') {
        ctx.result = dispatch_statement(c, &ctx.stmt, line);
    }

    serd_reader_free(reader);
    serd_env_free(ctx.env);

    return ctx.result;
}
