/*  test_main.c — Minimal test harness for Carrier.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the GNU General Public License 3.0.
 */

#include <stdio.h>

int tests_run    = 0;
int tests_passed = 0;
int tests_failed = 0;
int test_current_failed = 0;

/* Test suites — declared in separate files.
 *
 * test_turtle_emit_all (in tests/test_turtle_emit.c) targets the
 * pre-Jami CarrierEvent layout and is excluded from the build pending a
 * v2 rewrite. The Makefile's TEST_C_SRC list reflects that. */
void test_vcard_utils_all(void);
void test_rdf_canon_all(void);

int main(void)
{
    printf("carrier test suite\n");
    printf("==================\n\n");

    test_vcard_utils_all();
    test_rdf_canon_all();

    printf("\n------------------\n");
    printf("%d tests, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
