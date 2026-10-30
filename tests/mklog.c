/* Test helper: write START + n soil + n level records (fixture values, not measurements). */
#include <stdio.h>
#include <stdlib.h>

#include "store/store.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mklog <dir> <n>\n");
        return 1;
    }
    int n = atoi(argv[2]);
    struct store s = {0};
    struct store_recovery rec;
    int rc = store_open(&s, argv[1], &rec);
    if (rc != 0)
        return rc;
    for (int i = 0; i < n; i++) {
        struct record soil = {.sensor_id = SENSOR_SOIL, .mono_ns = 1, .wall_s = 1, .value = 400 + i};
        /* Last level sample is a no-echo, so status has to show it as such. */
        struct record level = {.sensor_id = SENSOR_LEVEL, .mono_ns = 1, .wall_s = 1, .value = 1230 + i};
        if (i == n - 1)
            level.flags = REC_F_NO_ECHO;
        if ((rc = store_append(&s, &soil, 0)) != 0 || (rc = store_append(&s, &level, 0)) != 0)
            break;
    }
    int crc = store_close(&s);
    return rc ? rc : crc;
}
