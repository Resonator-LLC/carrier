/*  test_main.c — Minimal test harness for Carrier.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the GNU General Public License 3.0.
 */

#include <stdio.h>

int tests_run    = 0;
int tests_passed = 0;
int tests_failed = 0;

/* Test suites — declared in separate files */
void test_turtle_emit_all(void);

int main(void)
{
    printf("carrier test suite\n");
    printf("==================\n\n");

    test_turtle_emit_all();

    printf("\n------------------\n");
    printf("%d tests, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
