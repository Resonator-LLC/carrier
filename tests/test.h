/*  test.h — Shared test macros for Carrier test suite.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the GNU General Public License 3.0.
 */

#ifndef CARRIER_TEST_H
#define CARRIER_TEST_H

#include <stdio.h>
#include <string.h>

extern int tests_run;
extern int tests_passed;
extern int tests_failed;

#define TEST(name) static void name(void)

/* Per-test failure flag — set by any ASSERT that fires inside the body,
 * read by RUN_TEST to decide whether to print PASS/FAIL and bump
 * tests_passed. Reset to 0 at the start of every RUN_TEST invocation.
 * `int` (not bool) so the harness stays C89-friendly. */
extern int test_current_failed;

#define RUN_TEST(name) do {                                     \
    tests_run++;                                                \
    test_current_failed = 0;                                    \
    printf("  %-50s", #name);                                   \
    name();                                                     \
    if (test_current_failed) {                                  \
        tests_failed++;                                         \
    } else {                                                    \
        tests_passed++;                                         \
        printf("PASS\n");                                       \
    }                                                           \
} while (0)

#define ASSERT(cond) do {                                       \
    if (!(cond)) {                                              \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__,    \
               #cond);                                          \
        test_current_failed = 1;                                \
        return;                                                 \
    }                                                           \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))

#define ASSERT_STR_EQ(a, b) do {                                \
    if (strcmp((a), (b)) != 0) {                                \
        printf("FAIL\n    %s:%d:\n      expected: \"%s\"\n"     \
               "      got:      \"%s\"\n",                      \
               __FILE__, __LINE__, (b), (a));                   \
        test_current_failed = 1;                                \
        return;                                                 \
    }                                                           \
} while (0)

#define ASSERT_STR_CONTAINS(haystack, needle) do {              \
    if (strstr((haystack), (needle)) == NULL) {                 \
        printf("FAIL\n    %s:%d:\n      \"%s\"\n"               \
               "      does not contain \"%s\"\n",               \
               __FILE__, __LINE__, (haystack), (needle));       \
        test_current_failed = 1;                                \
        return;                                                 \
    }                                                           \
} while (0)

#endif /* CARRIER_TEST_H */
