/*  turtle_parse.c
 *
 *  Turtle command parser for Carrier, powered by Serd.
 *  Parses RDF 1.1 Turtle input and dispatches carrier_*() calls.
 *
 *  Command surface: CreateAccount, LoadAccount, GetId, SetNick,
 *  SendTrustRequest, AcceptTrustRequest, DiscardTrustRequest, RemoveContact,
 *  SendMsg (1:1), Quit, plus M3 multi-party Swarm commands —
 *  CreateConversation (+ CreateGroup alias), SendConversationMsg,
 *  AcceptConversationRequest, DeclineConversationRequest, InviteContact,
 *  RemoveConversation. See arch/jami-migration.md §4 for the rename map.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
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

/* Extract the local name from a resolved URI. */
static const char *local_name(const char *uri)
{
    const char *hash = strrchr(uri, '#');
    if (hash) return hash + 1;

    const char *slash = strrchr(uri, '/');
    if (slash) return slash + 1;

    return uri;
}

/* Resolve a serd node to a full URI string. Caller must free. */
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
    (void)flags; (void)graph; (void)subject;
    (void)object_datatype; (void)object_lang;

    struct parse_ctx *ctx = (struct parse_ctx *)handle;
    struct turtle_stmt *stmt = &ctx->stmt;

    char *pred_uri = resolve_node(ctx, predicate);
    if (pred_uri == NULL) return SERD_SUCCESS;

    if (strcmp(pred_uri, RDF_TYPE_URI) == 0) {
        char *type_uri = resolve_node(ctx, object);
        if (type_uri) {
            snprintf(stmt->type, sizeof(stmt->type), "%s", local_name(type_uri));
            free(type_uri);
        }
        free(pred_uri);
        return SERD_SUCCESS;
    }

    if (stmt->num_preds < MAX_PREDICATES) {
        struct turtle_predicate *p = &stmt->preds[stmt->num_preds];
        snprintf(p->name, sizeof(p->name), "%s", local_name(pred_uri));

        if (object->type == SERD_LITERAL) {
            snprintf(p->value, sizeof(p->value), "%.*s",
                     (int)object->n_bytes, object->buf);
        } else {
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
 * Dispatch
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

/* Every account-scoped command needs a `carrier:account`. Returns the value
 * or emits an Error event and returns NULL. */
static const char *require_account(Carrier *c, const struct turtle_stmt *stmt,
                                   const char *command)
{
    const char *account = find_pred(stmt, "account");
    if (account == NULL || account[0] == '\0') {
        carrier_emit_error(c, command, "MissingField",
                           "command %s requires carrier:account", command);
        return NULL;
    }
    return account;
}

static int dispatch_statement(Carrier *c, const struct turtle_stmt *stmt)
{
    if (stmt->type[0] == '\0') {
        return 0;  /* Probably a prefix declaration */
    }

    /* --- Account lifecycle --- */

    if (strcmp(stmt->type, "CreateAccount") == 0) {
        const char *name = find_pred(stmt, "displayName");
        char new_id[CARRIER_ACCOUNT_ID_LEN];
        if (carrier_create_account(c, name, new_id) != 0) {
            carrier_emit_error(c, "CreateAccount", "LibjamiFailure",
                               "carrier_create_account failed");
        }
        return 0;
    }

    if (strcmp(stmt->type, "LoadAccount") == 0) {
        const char *account = require_account(c, stmt, "LoadAccount");
        if (!account) return 0;
        if (carrier_load_account(c, account) != 0) {
            carrier_emit_error(c, "LoadAccount", "NoSuchAccount",
                               "no account %s", account);
        }
        return 0;
    }

    /* --- Identity --- */

    if (strcmp(stmt->type, "GetId") == 0) {
        const char *account = require_account(c, stmt, "GetId");
        if (!account) return 0;
        carrier_get_id(c, account);
        return 0;
    }

    if (strcmp(stmt->type, "SetNick") == 0) {
        const char *account = require_account(c, stmt, "SetNick");
        if (!account) return 0;
        const char *nick = find_pred(stmt, "nick");
        if (!nick) nick = find_pred(stmt, "displayName");
        carrier_set_nick(c, account, nick ? nick : "");
        return 0;
    }

    /* --- Trust --- */

    if (strcmp(stmt->type, "SendTrustRequest") == 0) {
        const char *account = require_account(c, stmt, "SendTrustRequest");
        if (!account) return 0;
        const char *uri = find_pred(stmt, "contactUri");
        if (!uri) {
            carrier_emit_error(c, "SendTrustRequest", "MissingField",
                               "carrier:contactUri required");
            return 0;
        }
        const char *msg = find_pred(stmt, "message");
        carrier_send_trust_request(c, account, uri, msg);
        return 0;
    }

    if (strcmp(stmt->type, "AcceptTrustRequest") == 0) {
        const char *account = require_account(c, stmt, "AcceptTrustRequest");
        if (!account) return 0;
        const char *uri = find_pred(stmt, "contactUri");
        if (!uri) {
            carrier_emit_error(c, "AcceptTrustRequest", "MissingField",
                               "carrier:contactUri required");
            return 0;
        }
        carrier_accept_trust_request(c, account, uri);
        return 0;
    }

    if (strcmp(stmt->type, "DiscardTrustRequest") == 0) {
        const char *account = require_account(c, stmt, "DiscardTrustRequest");
        if (!account) return 0;
        const char *uri = find_pred(stmt, "contactUri");
        if (!uri) {
            carrier_emit_error(c, "DiscardTrustRequest", "MissingField",
                               "carrier:contactUri required");
            return 0;
        }
        carrier_discard_trust_request(c, account, uri);
        return 0;
    }

    if (strcmp(stmt->type, "RemoveContact") == 0) {
        const char *account = require_account(c, stmt, "RemoveContact");
        if (!account) return 0;
        const char *uri = find_pred(stmt, "contactUri");
        if (!uri) {
            carrier_emit_error(c, "RemoveContact", "MissingField",
                               "carrier:contactUri required");
            return 0;
        }
        carrier_remove_contact(c, account, uri);
        return 0;
    }

    /* --- Messaging --- */

    if (strcmp(stmt->type, "SendMsg") == 0) {
        const char *account = require_account(c, stmt, "SendMsg");
        if (!account) return 0;
        const char *uri = find_pred(stmt, "contactUri");
        const char *text = find_pred(stmt, "text");
        if (!uri || !text) {
            carrier_emit_error(c, "SendMsg", "MissingField",
                               "carrier:contactUri and carrier:text required");
            return 0;
        }
        carrier_send_message(c, account, uri, text);
        return 0;
    }

    /* --- Swarm conversations (multi-party) --- */

    /* `CreateGroup` is the v0.2 vocabulary name; `CreateConversation` is
     * accepted as an alias since both libjami and the C API spell it that
     * way. Either dispatches to carrier_create_conversation. */
    if (strcmp(stmt->type, "CreateConversation") == 0 ||
        strcmp(stmt->type, "CreateGroup") == 0) {
        const char *account = require_account(c, stmt, stmt->type);
        if (!account) return 0;
        const char *privacy = find_pred(stmt, "privacy");
        char new_id[CARRIER_CONVERSATION_ID_LEN];
        if (carrier_create_conversation(c, account, privacy, new_id) != 0) {
            carrier_emit_error(c, stmt->type, "LibjamiFailure",
                               "carrier_create_conversation failed");
        }
        return 0;
    }

    if (strcmp(stmt->type, "SendConversationMsg") == 0) {
        const char *account = require_account(c, stmt, "SendConversationMsg");
        if (!account) return 0;
        const char *conv = find_pred(stmt, "conversationId");
        const char *text = find_pred(stmt, "text");
        if (!conv || !text) {
            carrier_emit_error(c, "SendConversationMsg", "MissingField",
                               "carrier:conversationId and carrier:text required");
            return 0;
        }
        carrier_send_conversation_message(c, account, conv, text);
        return 0;
    }

    if (strcmp(stmt->type, "AcceptConversationRequest") == 0) {
        const char *account = require_account(c, stmt, "AcceptConversationRequest");
        if (!account) return 0;
        const char *conv = find_pred(stmt, "conversationId");
        if (!conv) {
            carrier_emit_error(c, "AcceptConversationRequest", "MissingField",
                               "carrier:conversationId required");
            return 0;
        }
        carrier_accept_conversation_request(c, account, conv);
        return 0;
    }

    if (strcmp(stmt->type, "DeclineConversationRequest") == 0) {
        const char *account = require_account(c, stmt, "DeclineConversationRequest");
        if (!account) return 0;
        const char *conv = find_pred(stmt, "conversationId");
        if (!conv) {
            carrier_emit_error(c, "DeclineConversationRequest", "MissingField",
                               "carrier:conversationId required");
            return 0;
        }
        carrier_decline_conversation_request(c, account, conv);
        return 0;
    }

    if (strcmp(stmt->type, "InviteContact") == 0) {
        const char *account = require_account(c, stmt, "InviteContact");
        if (!account) return 0;
        const char *conv = find_pred(stmt, "conversationId");
        const char *uri = find_pred(stmt, "contactUri");
        if (!conv || !uri) {
            carrier_emit_error(c, "InviteContact", "MissingField",
                               "carrier:conversationId and carrier:contactUri required");
            return 0;
        }
        carrier_invite_to_conversation(c, account, conv, uri);
        return 0;
    }

    if (strcmp(stmt->type, "RemoveConversation") == 0) {
        const char *account = require_account(c, stmt, "RemoveConversation");
        if (!account) return 0;
        const char *conv = find_pred(stmt, "conversationId");
        if (!conv) {
            carrier_emit_error(c, "RemoveConversation", "MissingField",
                               "carrier:conversationId required");
            return 0;
        }
        carrier_remove_conversation(c, account, conv);
        return 0;
    }

    /* --- Reactions --- */

    if (strcmp(stmt->type, "SendReaction") == 0) {
        const char *account = require_account(c, stmt, "SendReaction");
        if (!account) return 0;
        const char *conv = find_pred(stmt, "conversationId");
        const char *msg  = find_pred(stmt, "messageId");
        const char *react = find_pred(stmt, "reaction");
        if (!conv || !msg || !react) {
            carrier_emit_error(c, "SendReaction", "MissingField",
                               "carrier:conversationId, carrier:messageId, "
                               "and carrier:reaction required");
            return 0;
        }
        carrier_send_reaction(c, account, conv, msg, react);
        return 0;
    }

    /* --- Meta --- */

    if (strcmp(stmt->type, "Quit") == 0) {
        return 1;
    }

    carrier_emit_error(c, stmt->type, "UnknownCommand",
                       "no such command in carrier vocabulary");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------------*/

int turtle_parse_and_execute(Carrier *c, const char *line)
{
    if (c == NULL || line == NULL) return -1;

    struct parse_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.carrier = c;
    ctx.env = serd_env_new(NULL);

    /* Pre-declare carrier:/xsd: so callers don't have to send @prefix lines
     * before each command. Matches the prefixes emitted by turtle_emit. */
    {
        SerdNode carrier_name = serd_node_from_string(SERD_LITERAL,
            (const uint8_t *)"carrier");
        SerdNode carrier_uri  = serd_node_from_string(SERD_URI,
            (const uint8_t *)CARRIER_NS);
        serd_env_set_prefix(ctx.env, &carrier_name, &carrier_uri);

        SerdNode xsd_name = serd_node_from_string(SERD_LITERAL,
            (const uint8_t *)"xsd");
        SerdNode xsd_uri  = serd_node_from_string(SERD_URI,
            (const uint8_t *)"http://www.w3.org/2001/XMLSchema#");
        serd_env_set_prefix(ctx.env, &xsd_name, &xsd_uri);
    }

    SerdReader *reader = serd_reader_new(SERD_TURTLE, &ctx, NULL,
                                         on_base, on_prefix, on_statement, NULL);
    if (reader == NULL) {
        serd_env_free(ctx.env);
        return -1;
    }

    SerdStatus st = serd_reader_read_string(reader, (const uint8_t *)line);
    int ret = 0;

    if (st == SERD_SUCCESS || st == SERD_FAILURE) {
        ret = dispatch_statement(c, &ctx.stmt);
    } else {
        carrier_emit_error(c, "Parse", "InvalidTurtle",
                           "serd error %d parsing input", (int)st);
        ret = -1;
    }

    serd_reader_free(reader);
    serd_env_free(ctx.env);
    return ret;
}
