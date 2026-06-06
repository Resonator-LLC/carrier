/*  test_contact_restored.c — Turtle emit test for CARRIER_EVENT_CONTACT_RESTORED.
 *
 *  The full Tox-era test_turtle_emit.c is excluded from the build (still
 *  pending a v2 rewrite); this file covers ONLY the ContactRestored wire
 *  shape that ISSUE-127 added. The contract under test:
 *
 *    1. ContactRestored always serializes carrier:contactUri AND
 *       carrier:displayName, even when displayName is empty. Empty
 *       displayName renders as `carrier:displayName ""` so consumers can
 *       pattern-match on the predicate without a presence check.
 *    2. Standard turtle escaping applies to the displayName literal.
 *    3. Output ends with " .\n" like every other carrier turtle line.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "test.h"
#include "carrier.h"
#include "turtle_emit.h"

#include <stdlib.h>
#include <string.h>

/* Render an event to a heap-owned C string via tmpfile(); caller frees. */
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

TEST(test_contact_restored_with_display_name)
{
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_CONTACT_RESTORED;
    ev.timestamp = 1748000000000LL;
    snprintf(ev.account_id, sizeof(ev.account_id), "c314a87070bc4c74");
    snprintf(ev.contact_restored.contact_uri,
             sizeof(ev.contact_restored.contact_uri),
             "4748d985a10c8e84990f592a0ec0232efb733293");
    snprintf(ev.contact_restored.display_name,
             sizeof(ev.contact_restored.display_name),
             "bob");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:ContactRestored");
    ASSERT_STR_CONTAINS(out, "carrier:account \"c314a87070bc4c74\"");
    ASSERT_STR_CONTAINS(out,
        "carrier:contactUri \"4748d985a10c8e84990f592a0ec0232efb733293\"");
    ASSERT_STR_CONTAINS(out, "carrier:displayName \"bob\"");
    /* CMP-002 — blocked is structurally present; an un-banned restore is
     * "false". */
    ASSERT_STR_CONTAINS(out, "carrier:blocked \"false\"");

    /* Line must terminate with " .\n" — same convention as every other
     * carrier turtle line, so concatenated streams parse one-line-per-event. */
    size_t len = strlen(out);
    ASSERT(len >= 3);
    ASSERT(out[len - 1] == '\n');
    ASSERT(out[len - 2] == '.');

    free(out);
}

TEST(test_contact_restored_banned_emits_blocked_true)
{
    /* CMP-002 — a libjami-banned contact (the user blocked them in a prior
     * session) is replayed at AccountReady with blocked=true so consumers
     * re-hydrate their render gate / blocklist from the durable ban rather
     * than from process-local state. */
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_CONTACT_RESTORED;
    ev.timestamp = 1748000000000LL;
    snprintf(ev.account_id, sizeof(ev.account_id), "c314a87070bc4c74");
    snprintf(ev.contact_restored.contact_uri,
             sizeof(ev.contact_restored.contact_uri),
             "4748d985a10c8e84990f592a0ec0232efb733293");
    ev.contact_restored.blocked = true;

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:ContactRestored");
    ASSERT_STR_CONTAINS(out, "carrier:blocked \"true\"");

    free(out);
}

TEST(test_contact_restored_empty_display_name_still_emits_predicate)
{
    /* The whole point of the fix: a trusted contact whose cached vCard
     * is empty (or missing FN) must STILL produce a ContactRestored on
     * the wire, with displayName as the empty literal. Consumers gate
     * on the predicate's presence and render the bare URI when the
     * value is empty. */
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_CONTACT_RESTORED;
    ev.timestamp = 1748000000000LL;
    snprintf(ev.account_id, sizeof(ev.account_id), "c314a87070bc4c74");
    snprintf(ev.contact_restored.contact_uri,
             sizeof(ev.contact_restored.contact_uri),
             "51a5757140adc1aaa511ac55597cf53883489157");
    /* display_name stays as the zeroed buffer => empty C string. */

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "[] a carrier:ContactRestored");
    ASSERT_STR_CONTAINS(out,
        "carrier:contactUri \"51a5757140adc1aaa511ac55597cf53883489157\"");
    /* Exact empty-string form — the contract is `displayName ""`, NOT
     * omitted, NOT a typed-empty like `displayName <>`. */
    ASSERT_STR_CONTAINS(out, "carrier:displayName \"\"");

    free(out);
}

TEST(test_contact_restored_escapes_display_name)
{
    /* Make sure the turtle_escape() path is wired for displayName —
     * a peer chose a name with a double quote, must come out backslash-
     * escaped. */
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_CONTACT_RESTORED;
    ev.timestamp = 1748000000000LL;
    snprintf(ev.account_id, sizeof(ev.account_id), "c314a87070bc4c74");
    snprintf(ev.contact_restored.contact_uri,
             sizeof(ev.contact_restored.contact_uri),
             "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    snprintf(ev.contact_restored.display_name,
             sizeof(ev.contact_restored.display_name),
             "Bob \"The\" Builder");

    char *out = emit_to_string(&ev);
    ASSERT(out != NULL);
    ASSERT_STR_CONTAINS(out, "carrier:displayName \"Bob \\\"The\\\" Builder\"");

    free(out);
}

/* ---------------------------------------------------------------------------
 * Suite registrar — called from test_main.c.
 * ---------------------------------------------------------------------------*/

void test_contact_restored_all(void)
{
    printf("\nturtle_emit/contact_restored\n");
    printf("----------------------------\n");

    RUN_TEST(test_contact_restored_with_display_name);
    RUN_TEST(test_contact_restored_empty_display_name_still_emits_predicate);
    RUN_TEST(test_contact_restored_escapes_display_name);
    RUN_TEST(test_contact_restored_banned_emits_blocked_true);
}
