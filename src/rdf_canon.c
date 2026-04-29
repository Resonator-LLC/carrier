/*  rdf_canon.c — RDF canonicalization (RDNA2015 subset) + SHA-256 hash.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "rdf_canon.h"
#include "sha256.h"

#include <serd/serd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Dynamic string buffer
 * ---------------------------------------------------------------------------*/

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} StrBuf;

static int strbuf_push(StrBuf *sb, const char *data, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t newcap = sb->cap ? sb->cap * 2 : 256;
        while (newcap < sb->len + n + 1) newcap *= 2;
        char *p = realloc(sb->buf, newcap);
        if (!p) return -1;
        sb->buf = p;
        sb->cap = newcap;
    }
    memcpy(sb->buf + sb->len, data, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

static int strbuf_pushc(StrBuf *sb, char c) { return strbuf_push(sb, &c, 1); }

/* ---------------------------------------------------------------------------
 * Collected N-Quads lines
 * ---------------------------------------------------------------------------*/

typedef struct {
    char  **lines;
    size_t  count;
    size_t  cap;
} LineSet;

static int lineset_add(LineSet *ls, char *line)
{
    if (ls->count + 1 > ls->cap) {
        size_t newcap = ls->cap ? ls->cap * 2 : 16;
        char **p = realloc(ls->lines, newcap * sizeof(char *));
        if (!p) return -1;
        ls->lines = p;
        ls->cap = newcap;
    }
    ls->lines[ls->count++] = line;
    return 0;
}

static void lineset_free(LineSet *ls)
{
    for (size_t i = 0; i < ls->count; i++) free(ls->lines[i]);
    free(ls->lines);
    ls->lines = NULL;
    ls->count = ls->cap = 0;
}

static int lineset_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ---------------------------------------------------------------------------
 * N-Quads serialization helpers
 * ---------------------------------------------------------------------------*/

/* Escape a UTF-8 string for an N-Quads literal (no surrounding quotes). */
static int nq_escape_literal(StrBuf *sb, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '"')  { if (strbuf_push(sb, "\\\"", 2)) return -1; }
        else if (c == '\\') { if (strbuf_push(sb, "\\\\", 2)) return -1; }
        else if (c == '\n') { if (strbuf_push(sb, "\\n",  2)) return -1; }
        else if (c == '\r') { if (strbuf_push(sb, "\\r",  2)) return -1; }
        else if (c == '\t') { if (strbuf_push(sb, "\\t",  2)) return -1; }
        else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04X", (unsigned)c);
            if (strbuf_push(sb, esc, 6)) return -1;
        } else {
            if (strbuf_pushc(sb, (char)c)) return -1;
        }
    }
    return 0;
}

/* Expand a serd node to a fully-qualified URI string.
 * Returns a malloc'd string or NULL. Caller frees. */
static char *expand_uri(SerdEnv *env, const SerdNode *node)
{
    if (node->type == SERD_URI) {
        return strndup((const char *)node->buf, node->n_bytes);
    }
    if (node->type == SERD_CURIE) {
        SerdNode uri = serd_env_expand_node(env, node);
        if (uri.buf) {
            char *result = strndup((const char *)uri.buf, uri.n_bytes);
            serd_node_free(&uri);
            return result;
        }
    }
    return NULL;
}

/* Append the N-Quads representation of a node to sb. */
static int nq_write_node(StrBuf *sb, SerdEnv *env, const SerdNode *node,
                         const SerdNode *datatype, const SerdNode *lang)
{
    if (node->type == SERD_BLANK) {
        /* All blank nodes canonicalize to _:c14n0 for carrier's single-BN
         * topology. A full RDNA2015 impl would hash the neighborhood here. */
        return strbuf_push(sb, "_:c14n0", 7);
    }

    if (node->type == SERD_LITERAL) {
        if (strbuf_pushc(sb, '"')) return -1;
        if (nq_escape_literal(sb, (const char *)node->buf, node->n_bytes)) return -1;
        if (strbuf_pushc(sb, '"')) return -1;
        if (datatype && datatype->buf && datatype->n_bytes > 0) {
            char *dt = expand_uri(env, datatype);
            if (dt) {
                if (strbuf_push(sb, "^^<", 3) ||
                    strbuf_push(sb, dt, strlen(dt)) ||
                    strbuf_pushc(sb, '>')) {
                    free(dt); return -1;
                }
                free(dt);
            }
        } else if (lang && lang->buf && lang->n_bytes > 0) {
            if (strbuf_pushc(sb, '@') ||
                strbuf_push(sb, (const char *)lang->buf, lang->n_bytes)) return -1;
        }
        return 0;
    }

    /* URI or CURIE */
    char *uri = expand_uri(env, node);
    if (!uri) return -1;
    int ret = strbuf_pushc(sb, '<') ||
              strbuf_push(sb, uri, strlen(uri)) ||
              strbuf_pushc(sb, '>');
    free(uri);
    return ret ? -1 : 0;
}

/* ---------------------------------------------------------------------------
 * Serd callbacks
 * ---------------------------------------------------------------------------*/

struct canon_ctx {
    SerdEnv *env;
    LineSet  lines;
    int      error;
};

static SerdStatus canon_on_prefix(void *handle,
                                   const SerdNode *name,
                                   const SerdNode *uri)
{
    struct canon_ctx *ctx = (struct canon_ctx *)handle;
    serd_env_set_prefix(ctx->env, name, uri);
    return SERD_SUCCESS;
}

static SerdStatus canon_on_base(void *handle, const SerdNode *uri)
{
    struct canon_ctx *ctx = (struct canon_ctx *)handle;
    serd_env_set_base_uri(ctx->env, uri);
    return SERD_SUCCESS;
}

static SerdStatus canon_on_statement(void *handle,
                                      SerdStatementFlags flags,
                                      const SerdNode *graph,
                                      const SerdNode *subject,
                                      const SerdNode *predicate,
                                      const SerdNode *object,
                                      const SerdNode *object_datatype,
                                      const SerdNode *object_lang)
{
    (void)flags; (void)graph;

    struct canon_ctx *ctx = (struct canon_ctx *)handle;
    if (ctx->error) return SERD_SUCCESS;

    StrBuf sb = {0};

    if (nq_write_node(&sb, ctx->env, subject, NULL, NULL)  ||
        strbuf_pushc(&sb, ' ')                              ||
        nq_write_node(&sb, ctx->env, predicate, NULL, NULL) ||
        strbuf_pushc(&sb, ' ')                              ||
        nq_write_node(&sb, ctx->env, object, object_datatype, object_lang) ||
        strbuf_push(&sb, " .\n", 3)) {
        free(sb.buf);
        ctx->error = 1;
        return SERD_SUCCESS;
    }

    if (lineset_add(&ctx->lines, sb.buf) < 0) {
        free(sb.buf);
        ctx->error = 1;
        return SERD_SUCCESS;
    }
    /* sb.buf ownership transferred to lineset */
    return SERD_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------------*/

int rdf_canon_hash(const char *turtle, size_t len, uint8_t out[32])
{
    (void)len;  /* serd_reader_read_string takes NUL-terminated string */

    struct canon_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.env = serd_env_new(NULL);
    if (!ctx.env) return -1;

    /* Pre-declare the prefixes carrier emits so callers don't need @prefix. */
    {
        SerdNode n, u;
        n = serd_node_from_string(SERD_LITERAL, (const uint8_t *)"carrier");
        u = serd_node_from_string(SERD_URI,
                (const uint8_t *)"http://resonator.network/v2/carrier#");
        serd_env_set_prefix(ctx.env, &n, &u);

        n = serd_node_from_string(SERD_LITERAL, (const uint8_t *)"xsd");
        u = serd_node_from_string(SERD_URI,
                (const uint8_t *)"http://www.w3.org/2001/XMLSchema#");
        serd_env_set_prefix(ctx.env, &n, &u);

        n = serd_node_from_string(SERD_LITERAL, (const uint8_t *)"rdf");
        u = serd_node_from_string(SERD_URI,
                (const uint8_t *)"http://www.w3.org/1999/02/22-rdf-syntax-ns#");
        serd_env_set_prefix(ctx.env, &n, &u);
    }

    SerdReader *reader = serd_reader_new(SERD_TURTLE, &ctx, NULL,
                                         canon_on_base, canon_on_prefix,
                                         canon_on_statement, NULL);
    if (!reader) {
        serd_env_free(ctx.env);
        return -1;
    }

    SerdStatus st = serd_reader_read_string(reader, (const uint8_t *)turtle);
    serd_reader_free(reader);
    serd_env_free(ctx.env);

    if (ctx.error || (st != SERD_SUCCESS && st != SERD_FAILURE)) {
        lineset_free(&ctx.lines);
        return -1;
    }

    if (ctx.lines.count == 0) {
        SHA256Ctx sha;
        sha256_init(&sha);
        sha256_final(&sha, out);
        lineset_free(&ctx.lines);
        return 0;
    }

    qsort(ctx.lines.lines, ctx.lines.count, sizeof(char *), lineset_cmp);

    SHA256Ctx sha;
    sha256_init(&sha);
    for (size_t i = 0; i < ctx.lines.count; i++) {
        sha256_update(&sha, (const uint8_t *)ctx.lines.lines[i],
                      strlen(ctx.lines.lines[i]));
    }
    sha256_final(&sha, out);

    lineset_free(&ctx.lines);
    return 0;
}
