/* Step-1 smoke test: proves the harness, flags, and sanitizers work on the target. */
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "unit.h"

static void test_config_sane(void)
{
    CHECK(CFG_SYNC_MAX_RECORDS > 0);
    CHECK(CFG_SYNC_MAX_AGE_MS > 0);
    CHECK(CFG_BACKOFF_MIN_MS <= CFG_BACKOFF_MAX_MS);
    CHECK(CFG_LINE_MAX >= (int)sizeof "SOIL,1023,DIST,9999.9\r\n");
}

static void test_heap_roundtrip(void)
{
    /* Touches the heap so `make asan` exercises the ASan allocator, not just compiles. */
    char *p = malloc(16);
    CHECK(p != NULL);
    if (p) {
        memset(p, 'x', 16);
        CHECK(p[15] == 'x');
        free(p);
    }
}

int main(void)
{
    RUN(test_config_sane);
    RUN(test_heap_roundtrip);
    return unit_done("test_smoke");
}
