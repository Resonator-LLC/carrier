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

#define RUN_TEST(name) do {                                     \
    tests_run++;                                                \
    printf("  %-50s", #name);                                   \
    name();                                                     \
    tests_passed++;                                             \
    printf("PASS\n");                                           \
} while (0)

#define ASSERT(cond) do {                                       \
    if (!(cond)) {                                              \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__,    \
               #cond);                                          \
        tests_failed++;                                         \
        return;                                                 \
    }                                                           \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))

#define ASSERT_STR_EQ(a, b) do {                                \
    if (strcmp((a), (b)) != 0) {                                \
        printf("FAIL\n    %s:%d:\n      expected: \"%s\"\n"     \
               "      got:      \"%s\"\n",                      \
               __FILE__, __LINE__, (b), (a));                   \
        tests_failed++;                                         \
        return;                                                 \
    }                                                           \
} while (0)

#define ASSERT_STR_CONTAINS(haystack, needle) do {              \
    if (strstr((haystack), (needle)) == NULL) {                 \
        printf("FAIL\n    %s:%d:\n      \"%s\"\n"               \
               "      does not contain \"%s\"\n",               \
               __FILE__, __LINE__, (haystack), (needle));       \
        tests_failed++;                                         \
        return;                                                 \
    }                                                           \
} while (0)

#endif /* CARRIER_TEST_H */
