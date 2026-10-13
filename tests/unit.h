/* Minimal unit test helpers. */
#ifndef VTESSE_TESTS_UNIT_H
#define VTESSE_TESTS_UNIT_H

#include <stdio.h>
#include <string.h>

static int unit_failures;
static int unit_checks;

#define CHECK(cond) do { \
    unit_checks++; \
    if (!(cond)) { \
        unit_failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long long va_ = (long long)(a), vb_ = (long long)(b); \
    unit_checks++; \
    if (va_ != vb_) { \
        unit_failures++; \
        fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", \
                __FILE__, __LINE__, #a, #b, va_, vb_); \
    } \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
    const char *sa_ = (a), *sb_ = (b); \
    unit_checks++; \
    if (strcmp(sa_, sb_) != 0) { \
        unit_failures++; \
        fprintf(stderr, "%s:%d: %s == %s failed: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, #a, #b, sa_, sb_); \
    } \
} while (0)

#define RUN(test) do { \
    int before_ = unit_failures; \
    test(); \
    printf("%s  %s\n", unit_failures == before_ ? "PASS" : "FAIL", #test); \
} while (0)

#define UNIT_SUMMARY() ( \
    printf("%d checks, %d failure(s)\n", unit_checks, unit_failures), \
    unit_failures ? 1 : 0)

#endif
