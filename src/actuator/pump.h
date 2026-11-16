/* Pump control (owner: Sam): the layer that actually switches the pump, with its own safety
 * rules. It does not use the watering engine and must not depend on it: the engine only
 * REQUESTS start/stop; this decides what the relay does.
 *
 * Pure logic. Switching goes through a backend (pump_sim.c now, labelled SIMULATED; GPIO later,
 * once there is hardware to test it on). Time is passed in (CLOCK_MONOTONIC ns).
 *
 * Rules, none of which ask the engine:
 *  - OFF first: pump_init() switches the relay OFF before anything else (a crashed process may
 *    have left it on), and pump_shutdown() switches it OFF on the way out.
 *  - A run ends at the first of: its dose; pump_stop(); cap_ms after the relay switched ON
 *    (CFG_PUMP_CAP_MS, 10 min, whatever the dose); or hub_timeout_ms without hearing from the hub
 *    (any start/stop or pump_hub_alive()).
 *  - Every call checks those limits FIRST, so whenever a call returns with the relay on, the run
 *    is under the cap and the hub was heard within the timeout. pump_tick() at >= 1 Hz keeps
 *    checking when nothing else happens.
 *  - START is refused unless OFF, not shut down, not FAULT, and 0 < dose_ms < cap_ms. A START
 *    while already ON is ignored: it never extends a run.
 *  - Time going backwards while ON counts as "too long": OFF at once.
 *  - If switching OFF fails the state is FAULT: OFF is retried on every call and every START is
 *    refused until the next pump_init() (a process restart).
 *
 * What it cannot do: anything while nobody calls it. If the process running it hangs, the
 * supervisor (and in the end a hardware cutoff) must turn the pump off. The hub-silence rule only
 * protects against a hung HUB if pump control runs in its own process, not inside the hub;
 * where it runs is still to be decided with Jim (docs/engine-hub-integration.md §8). */
#ifndef PLANT_PUMP_H
#define PLANT_PUMP_H

#include <stdbool.h>
#include <stdint.h>

struct pump_backend {
    int (*set)(void *ctx, bool on); /* switch the relay; 0 on success, -1 on failure */
    void *ctx;
};

struct pump_params {
    uint32_t cap_ms;         /* hard limit per run; <= CFG_PUMP_CAP_MS */
    uint32_t hub_timeout_ms; /* hub silent this long while ON: OFF */
};

void pump_params_default(struct pump_params *p);
const char *pump_params_check(const struct pump_params *p); /* NULL if usable */

enum pump_state {
    PUMP_OFF = 0,
    PUMP_ON = 1,
    PUMP_FAULT = 2, /* an OFF failed: retrying; no START until pump_init() */
};

/* Why a run ended (or a START was refused). */
enum pump_end {
    PUMP_END_NONE = 0,
    PUMP_END_DOSE_DONE = 1,     /* ran its dose */
    PUMP_END_STOPPED = 2,       /* pump_stop() */
    PUMP_END_CAP = 3,           /* reached cap_ms (or time went backwards) */
    PUMP_END_HUB_SILENT = 4,    /* no word from the hub for hub_timeout_ms */
    PUMP_END_SHUTDOWN = 5,      /* pump_shutdown() */
    PUMP_END_SWITCH_FAILED = 6, /* the backend failed to switch */
    PUMP_END_REFUSED = 7,       /* START refused (bad dose, FAULT, shut down) */
};

/* What to tell the hub after a call. The hub turns it into eng_on_watering_started/_stopped. */
struct pump_report {
    enum { PUMP_REP_NONE = 0, PUMP_REP_STARTED, PUMP_REP_STOPPED } kind;
    uint32_t ran_ms;   /* PUMP_REP_STOPPED: how long the relay was on */
    enum pump_end why; /* PUMP_REP_STOPPED */
};

/* Private: read through the accessors. */
struct pump {
    struct pump_params p;
    struct pump_backend b;
    enum pump_state state;
    bool params_ok, shut_down;
    bool relay_maybe_on;  /* switched ON, or an OFF failed: not known to be off */
    uint64_t on_since_ns; /* relay switched ON */
    uint32_t dose_ms;
    uint64_t last_heard_ns;
    uint64_t runs, refused, ignored_starts, switch_failures;
};

/* Switches OFF first. Returns 0, or -1 if the params are invalid (every START refused) or the
 * OFF failed (FAULT). */
int pump_init(struct pump *pc, const struct pump_params *p, const struct pump_backend *b, uint64_t now_ns);
void pump_start(struct pump *pc, uint64_t now_ns, uint32_t dose_ms, struct pump_report *rep);
void pump_stop(struct pump *pc, uint64_t now_ns, struct pump_report *rep);
void pump_hub_alive(struct pump *pc, uint64_t now_ns, struct pump_report *rep);
void pump_tick(struct pump *pc, uint64_t now_ns, struct pump_report *rep);
void pump_shutdown(struct pump *pc, uint64_t now_ns, struct pump_report *rep);

enum pump_state pump_state(const struct pump *pc);
bool pump_relay_maybe_on(const struct pump *pc);
const char *pump_end_name(enum pump_end why);

#endif
