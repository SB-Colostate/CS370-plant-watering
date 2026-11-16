/* Test helper: a log with one record of each watering-engine event kind (fixture values, not
 * measurements), written through the real store API. */
#include <stdio.h>

#include "store/store.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: mkevents <dir>\n");
        return 1;
    }
    struct store s = {0};
    struct store_recovery rec;
    int rc = store_open(&s, argv[1], &rec);
    if (rc != 0)
        return rc;
    /* kind, value: intent 20 s, DIAG OK (1), LOCKOUT UNFINISHED_WATERING (5), ACK of PUMP_NO_FLOW (1),
     * PHASE WATERING (3), BLOCKS 0x41 (SOIL_STALE | RATE_LIMIT) */
    static const struct { unsigned kind; int32_t value; } ev[] = {
        {REC_EV_WATER_INTENT, 20000}, {REC_EV_DIAG, 1}, {REC_EV_LOCKOUT, 5},
        {REC_EV_ACK, 1},              {REC_EV_PHASE, 3}, {REC_EV_BLOCKS, 0x41},
    };
    for (size_t i = 0; i < sizeof ev / sizeof ev[0] && rc == 0; i++) {
        struct record r = {.sensor_id = SENSOR_SYSTEM, .flags = REC_EVENT_FLAGS(ev[i].kind), .mono_ns = 1,
                           .wall_s = 1, .value = ev[i].value};
        rc = store_append(&s, &r, STORE_DURABLE);
    }
    int crc = store_close(&s);
    return rc ? rc : crc;
}
