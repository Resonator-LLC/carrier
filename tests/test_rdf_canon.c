/*  test_rdf_canon.c — Unit tests for rdf_canon_hash, including the
 *  stderr-silencing fix for ISSUE-104 (carrier_send_message used to leak
 *  "error: (string):1:12: bad verb" via serd's default error sink when a
 *  plain-text chat body was hashed as best-effort).
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "test.h"
#include "rdf_canon.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Capture stderr written during `body` into a malloc'd NUL-terminated
 * string and stash it in *out. Caller frees. Uses dup2 to redirect fd 2
 * through a pipe; lifted from the standard POSIX recipe. */
#define WITH_STDERR_CAPTURE(out, body) do {                              \
    fflush(stderr);                                                      \
    int saved_stderr = dup(STDERR_FILENO);                               \
    int pipefd[2];                                                       \
    int pipe_rc = pipe(pipefd);                                          \
    ASSERT(pipe_rc == 0);                                                \
    int flags = fcntl(pipefd[0], F_GETFL, 0);                            \
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);                       \
    dup2(pipefd[1], STDERR_FILENO);                                      \
    close(pipefd[1]);                                                    \
                                                                         \
    body                                                                 \
                                                                         \
    fflush(stderr);                                                      \
    dup2(saved_stderr, STDERR_FILENO);                                   \
    close(saved_stderr);                                                 \
                                                                         \
    char  cap_buf[4096];                                                 \
    ssize_t cap_n = read(pipefd[0], cap_buf, sizeof(cap_buf) - 1);       \
    close(pipefd[0]);                                                    \
    if (cap_n < 0) cap_n = 0;                                            \
    cap_buf[cap_n] = '\0';                                               \
    *(out) = strdup(cap_buf);                                            \
} while (0)

TEST(test_rdf_canon_hash_valid_turtle_succeeds)
{
    const char *turtle =
        "[] a <http://example.org/Thing> ; <http://example.org/p> \"v\" .";
    uint8_t hash[32];
    char *captured = NULL;
    WITH_STDERR_CAPTURE(&captured, {
        int rc = rdf_canon_hash(turtle, strlen(turtle), hash);
        ASSERT_EQ(rc, 0);
    });
    ASSERT(captured != NULL);
    ASSERT_STR_EQ(captured, "");
    free(captured);
}

TEST(test_rdf_canon_hash_plain_text_returns_minus_one_silently)
{
    /* This is the carrier_send_message chat-body path: text isn't Turtle.
     * Pre-fix, serd's default error sink wrote "error: (string):1:12: bad
     * verb" to stderr on every send. Post-fix, the silent sink eats it. */
    const char *text = "hello from alice over jami";
    uint8_t hash[32];
    char *captured = NULL;
    WITH_STDERR_CAPTURE(&captured, {
        int rc = rdf_canon_hash(text, strlen(text), hash);
        ASSERT_EQ(rc, -1);
    });
    ASSERT(captured != NULL);
    ASSERT_STR_EQ(captured, "");
    free(captured);
}

TEST(test_rdf_canon_hash_bad_verb_silenced)
{
    /* A body containing a bare colon (e.g. "hello: world") tricks serd
     * into looking for a CURIE-like predicate, then bails with "bad
     * verb" because there's no preceding subject. This is the exact
     * shape of the stderr leak observed in alice's antenna log on
     * 2026-05-13 — pre-fix it printed "error: (string):1:12: bad verb"
     * on every send whose body matched this pattern. */
    const char *text = "hello world: this is a chat message";
    uint8_t hash[32];
    char *captured = NULL;
    WITH_STDERR_CAPTURE(&captured, {
        int rc = rdf_canon_hash(text, strlen(text), hash);
        ASSERT_EQ(rc, -1);
    });
    ASSERT(captured != NULL);
    ASSERT(strstr(captured, "bad verb") == NULL);
    ASSERT_STR_EQ(captured, "");
    free(captured);
}

TEST(test_rdf_canon_hash_empty_input_stays_silent)
{
    /* Empty input is an edge case — serd treats it as a syntax error and
     * we return -1. The contract we care about for ISSUE-104 is just
     * that stderr stays clean; the rc itself is implementation-defined
     * and not exercised by any real caller. */
    uint8_t hash[32];
    char *captured = NULL;
    WITH_STDERR_CAPTURE(&captured, {
        (void)rdf_canon_hash("", 0, hash);
    });
    ASSERT(captured != NULL);
    ASSERT_STR_EQ(captured, "");
    free(captured);
}

void test_rdf_canon_all(void)
{
    printf("\nrdf_canon\n");
    printf("---------\n");
    RUN_TEST(test_rdf_canon_hash_valid_turtle_succeeds);
    RUN_TEST(test_rdf_canon_hash_plain_text_returns_minus_one_silently);
    RUN_TEST(test_rdf_canon_hash_bad_verb_silenced);
    RUN_TEST(test_rdf_canon_hash_empty_input_stays_silent);
}
