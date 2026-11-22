/* Hub glue (owner: Sam; for Jim's review): connects the hub and its store to the watering engine
 * (src/analysis) and to pump control (src/actuator). Follows docs/engine-hub-integration.md.
 *
 * INTERIM DESIGN: pump control runs INSIDE the hub process with a SIMULATED relay
 * (actuator/pump_simfile.c), so that a full watering cycle can be tested on a PC through the real
 * hub and store. This is not the final design. Before a real pump is connected, pump control must
 * run as its own process under the supervisor: its hub-silence cutoff cannot protect against a
 * hung hub while it lives inside the hub (docs/engine-hub-integration.md §6a). */
#ifndef PLANT_WATER_H
#define PLANT_WATER_H

#include <stdbool.h>

#include "actuator/pump.h"
#include "actuator/pump_simfile.h"
#include "analysis/engine.h"
#include "store/store.h"

struct water {
    struct pump_simfile relay;
    struct pump pc;
    bool pump_ready;
    struct eng e;
    struct store *s;
    bool engine_ready;
    unsigned long events_logged, replayed;
};

/* FIRST at startup, before the store is opened: switch the (simulated) relay OFF. A crashed hub
 * may have left it on. Returns 0, or EXIT_SYSERR if OFF could not be confirmed (do not run). */
int water_pump_off(struct water *w, const char *dir);

/* After store_open: start the engine, replay the whole log into it, finish the restore and act
 * on its result (it may lock out). test_timings: short timings for tests only. Returns 0,
 * EXIT_USAGE or EXIT_SYSERR. */
int water_open(struct water *w, struct store *s, const char *dir, bool test_timings);

/* After a sample has been appended to the store: the same record. */
int water_on_sample(struct water *w, const struct record *r);

/* Once per pass of the hub's loop (at least once a second). */
int water_tick(struct water *w);

/* On the way out, before store_close: pump OFF, and the engine told about it. */
int water_close(struct water *w);

#endif
