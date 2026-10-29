/* Minimal test harness: CHECK keeps going after a failure so one run reports every problem. */
#ifndef PLANT_UNIT_H
#define PLANT_UNIT_H

#include <stdio.h>

static int unit_checks;
static int unit_fails;

#define CHECK(cond)                                                          \
    do {                                                                     \
        unit_checks++;                                                       \
        if (!(cond)) {                                                       \
            unit_fails++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define RUN(fn)                          \
    do {                                 \
        printf("  %s\n", #fn);           \
        fn();                            \
    } while (0)

static inline int unit_done(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, unit_checks, unit_fails);
    return unit_fails ? 1 : 0;
}

#endif
