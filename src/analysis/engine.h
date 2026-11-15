/* Watering decision engine (owner: Sam).
 *
 * The engine decides WHEN the plant needs water and WHETHER it is safe to water. It never
 * touches hardware. It returns requests (ENG_REQ_START / ENG_REQ_STOP); a separate
 * pump-control layer decides whether to carry them out, enforces the 10-minute cap on its own,
 * and reports back what actually happened (eng_on_watering_started / _stopped).
 * A request is not a fact: after START the engine waits in ENG_START_REQUESTED, and only a
 * confirmation from pump control moves it to ENG_WATERING.
 *
 * Every input function ends by checking its own output (eng_enforce_output). An unsafe request
 * (dose out of range, start while blocked, start with no durable intent, a lost critical event)
 * is replaced by STOP and the engine locks out with ENG_LOCK_INTERNAL.
 *
 * Pure code: no system calls, no clock reads, no I/O, no malloc, no globals. Time is always an
 * argument, in CLOCK_MONOTONIC nanoseconds (the clock the sampler stamps every sample with), so
 * every behavior can be tested without hardware and without sleeping.
 *
 * Commit 1 status: types, parameters, and a safe-by-default skeleton. The engine starts IDLE
 * with the "no data yet" blocks set and never requests watering. Later commits add filtering,
 * the blocks, dry detection, watering, verification, and restore from the log.
 */
#ifndef PLANT_ENGINE_H
#define PLANT_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "analysis/filters.h"
#include "store/record.h" /* struct record, SENSOR_*, REC_F_* (header only, nothing to link) */

/* ---------------------------------------------------------------- parameters */

struct eng_params {
    /* Soil probe, raw ADC counts 0..1023. Our module reads ~0 when dry and RISES when wet. */
    int32_t soil_dry_enter; /* smoothed soil <= this: the soil counts as dry */
    int32_t soil_dry_exit;  /* smoothed soil >= this: no longer dry (> dry_enter, for hysteresis) */
    int32_t soil_rail_high; /* reading pinned at or above this: probe fault */
    uint32_t dry_dwell_ms;  /* soil must stay dry this long before watering is requested */

    /* Reservoir. The HC-SR04 measures DISTANCE from the sensor down to the water surface, in mm.
     * A LARGER distance means LESS water. Convert with eng_water_height_mm(); never compare
     * raw distances when you mean "more" or "less" water. */
    int32_t level_empty_dist_mm; /* distance when the water is down at the pump inlet */
    int32_t level_full_dist_mm;  /* distance when the tank is full; must be < empty_dist */
    int32_t level_reserve_mm;    /* water height above "empty" required to start watering */
    int32_t level_hyst_mm;       /* once low, water must reach reserve + this to count as OK */
    uint32_t level_confirm;      /* consecutive filtered readings needed to change OK <-> LOW */
    uint32_t no_echo_run;        /* this many no-echo readings in a row: LEVEL_NO_ECHO */
    uint32_t soil_rail_run;      /* this many readings at the rail in a row: SOIL_RAIL (and
                                  * this many below it to clear) */

    /* Watering limits. pump_cap_ms is enforced by the pump-control layer and the supervisor;
     * the engine only guarantees it never asks for a dose that long. */
    uint32_t dose_ms;
    uint32_t pump_cap_ms;     /* <= CFG_PUMP_CAP_MS (10 min); tests may lower it */
    uint32_t min_interval_ms; /* between the starts of two waterings (>= 24 h / ENG_HISTORY) */
    uint32_t daily_dose_ms;   /* total requested dose allowed in any 24 h window */
    uint32_t start_confirm_ms; /* pump control must confirm a START within this */
    uint32_t stop_grace_ms;    /* ...and a stop (dose end or STOP) within this; dose + 2 * grace
                                * must stay below pump_cap_ms */

    /* Sensor health. */
    uint32_t stale_ms;  /* no REAL reading for this long: stale. Also the filters' max gap */
    uint32_t warmup_ms; /* fresh data needed after start before the engine trusts it */
};

/* Fills p from config.h (CFG_PUMP_CAP_MS, CFG_ENG_*). */
void eng_params_default(struct eng_params *p);
/* NULL if p is usable; otherwise a short description of the first problem found. */
const char *eng_params_check(const struct eng_params *p);

/* Water height in mm above "empty" for a measured distance. Negative means below the inlet.
 * Example: empty_dist 200, full_dist 40. Distance 190 -> 10 mm of water; distance 60 -> 140 mm. */
int32_t eng_water_height_mm(const struct eng_params *p, int32_t dist_mm);

/* ---------------------------------------------------------------- states and codes */

/* Where the engine is in the watering cycle. Numbers are stable: they will be written to disk. */
enum eng_phase {
    ENG_IDLE = 0,            /* soil not dry, or waiting */
    ENG_DRY_DWELL = 1,       /* soil dry; waiting dry_dwell_ms before asking to water */
    ENG_START_REQUESTED = 2, /* START sent; pump control has NOT confirmed the pump is running */
    ENG_WATERING = 3,        /* pump control confirmed the pump started */
    ENG_VERIFYING = 4,       /* dose over; watching both sensors to check the water arrived */
    ENG_LOCKOUT = 5,         /* a fault that needs a person; no watering until acknowledged */
};

/* Why watering is not allowed right now. Any bit set blocks a start (and, from commit 5,
 * stops a dose in progress). These clear by themselves when the condition goes away. */
enum {
    ENG_BLK_SOIL_STALE = 1u << 0,    /* no recent soil sample */
    ENG_BLK_LEVEL_STALE = 1u << 1,   /* no recent level sample */
    ENG_BLK_LEVEL_NO_ECHO = 1u << 2, /* the ultrasonic keeps getting no echo */
    ENG_BLK_SOIL_RAIL = 1u << 3,     /* soil pinned at the top rail: probe fault */
    ENG_BLK_RESERVOIR_LOW = 1u << 4, /* reservoir not confirmed OK: low, or not known yet */
    ENG_BLK_WARMUP = 1u << 5,        /* not enough fresh data since start */
    ENG_BLK_RATE_LIMIT = 1u << 6,    /* last watering started < min_interval_ms ago */
    ENG_BLK_DAILY_CAP = 1u << 7,     /* daily_dose_ms used up */
    ENG_BLK_STAYS_WET = 1u << 8,     /* soil has stayed wet too long (root-rot rule) */
};
#define ENG_BLK_ALL 0x1FFu
/* Blocks that also STOP a watering in progress. RATE_LIMIT and DAILY_CAP are not among them:
 * they are set by the start itself. */
#define ENG_BLK_SAFETY                                                                     \
    (ENG_BLK_SOIL_STALE | ENG_BLK_LEVEL_STALE | ENG_BLK_LEVEL_NO_ECHO | ENG_BLK_SOIL_RAIL | \
     ENG_BLK_RESERVOIR_LOW)

/* What the engine believes about the reservoir. Only ENG_RES_OK allows watering. */
enum eng_reservoir {
    ENG_RES_UNKNOWN = 0, /* not confirmed yet (start, or level data restarted after a gap) */
    ENG_RES_OK = 1,      /* level_confirm readings in a row at or above reserve + hyst */
    ENG_RES_LOW = 2,     /* level_confirm readings in a row below reserve */
};

/* Result of checking one watering with both sensors. */
enum eng_diag {
    ENG_DIAG_NONE = 0,
    ENG_DIAG_OK = 1,                 /* level dropped as expected and soil rose */
    ENG_DIAG_PUMP_NO_FLOW = 2,       /* level unchanged, soil unchanged */
    ENG_DIAG_LEAK_OR_PROBE_OUT = 3,  /* level dropped, soil unchanged: can't tell which */
    ENG_DIAG_LEVEL_NOT_TRACKING = 4, /* level unchanged, soil rose */
    ENG_DIAG_INCONCLUSIVE = 5,       /* changes too small to call, or a sensor went stale */
    ENG_DIAG_CHANNELING = 6,         /* reserved: not implemented until we record an episode */
};

/* Why the engine is in ENG_LOCKOUT. The first reason is kept. */
enum eng_lock {
    ENG_LOCK_NONE = 0,
    ENG_LOCK_PUMP_NO_FLOW = 1,
    ENG_LOCK_LEAK_OR_PROBE_OUT = 2,
    ENG_LOCK_LEVEL_NOT_TRACKING = 3,
    ENG_LOCK_PUMP_TIMEOUT = 4,       /* the pump-control layer's hard cap fired */
    ENG_LOCK_UNFINISHED_WATERING = 5,/* log has a watering intent with no completion */
    ENG_LOCK_ACTUATOR = 6,           /* pump-control layer could not switch */
    ENG_LOCK_UNEXPECTED_PUMP = 7,    /* pump ran without the engine asking */
    ENG_LOCK_INTERNAL = 8,           /* the engine's own output check failed: an engine bug */
};

/* ---------------------------------------------------------------- inputs from pump control */

/* Why a dose ended, as reported by the pump-control layer. */
enum eng_stop {
    ENG_STOP_DOSE_DONE = 1, /* ran the requested dose */
    ENG_STOP_REQUESTED = 2, /* stopped because the engine sent ENG_REQ_STOP */
    ENG_STOP_CAP = 3,       /* its own hard cap fired */
    ENG_STOP_ACT_FAIL = 4,  /* could not switch the pump */
};

/* ---------------------------------------------------------------- outputs */

enum eng_request {
    ENG_REQ_NONE = 0,
    ENG_REQ_START = 1, /* please run one dose of dose_ms. A request only: pump control may
                        * refuse, and nothing is running until eng_on_watering_started. */
    ENG_REQ_STOP = 2,  /* please make sure the pump is off, now */
};

/* Things the hub should log. Mapped to log records by the hub (commit 8, with Jim). */
enum eng_event_kind {
    ENG_EV_NONE = 0,         /* for eng_restore_event only: a log record that is not ours */
    ENG_EV_PHASE = 1,        /* value = new enum eng_phase */
    ENG_EV_BLOCKS = 2,       /* value = new block mask */
    ENG_EV_WATER_INTENT = 3, /* value = dose_ms; durable, must be on disk BEFORE the pump starts */
    ENG_EV_DIAG = 4,         /* value = enum eng_diag */
    ENG_EV_LOCKOUT = 5,      /* value = enum eng_lock; durable */
};

#define ENG_MAX_EVENTS 8
/* Slots only critical events may use. One call emits at most one of each critical kind
 * (eng_check_output enforces that), so critical events can never be crowded out. */
#define ENG_RESERVED_EVENTS 3

struct eng_event {
    enum eng_event_kind kind;
    int32_t value;
    bool durable; /* must reach disk (fdatasync) before the hub acts on the request */
};

/* Filled by every input function (it is cleared first). The hub appends the events in order,
 * then passes the request to pump control. If either dropped count is nonzero the hub must log
 * an ERROR line itself: those events never reached the log. */
struct eng_out {
    enum eng_request request;
    uint32_t dose_ms;          /* set only with ENG_REQ_START; always in (0, pump cap) */
    unsigned n_events;
    unsigned dropped_info;     /* PHASE/BLOCKS events that did not fit: missing from the log only */
    unsigned dropped_critical; /* INTENT/DIAG/LOCKOUT that did not fit: never expected; forces STOP */
    struct eng_event ev[ENG_MAX_EVENTS];
};

/* WATER_INTENT, DIAG and LOCKOUT are critical: they may use the reserved slots. */
bool eng_event_is_critical(enum eng_event_kind k);
/* Appends one event. Non-critical events may use only ENG_MAX_EVENTS - ENG_RESERVED_EVENTS
 * slots. Returns false (and counts the drop) if the event did not fit. */
bool eng_out_push(struct eng_out *out, enum eng_event_kind kind, int32_t value, bool durable);

/* ---------------------------------------------------------------- the engine */

#define ENG_DAY_MS 86400000u
/* Watering starts remembered for the rate limit and the daily cap (last 24 h). With
 * min_interval_ms >= ENG_DAY_MS / ENG_HISTORY, live operation can never fill it. */
#define ENG_HISTORY 32

struct eng_start {
    int64_t t_ns;     /* when it was requested, on THIS boot's monotonic clock (restored
                       * starts from before this boot can be negative) */
    uint32_t dose_ms; /* dose counted against the daily cap */
};

/* A start read back from the log, timed on a "continuous" clock that joins boots end to end. */
struct eng_restored {
    uint64_t c_ns;
    uint32_t dose_ms;
};

/* All fields are private: read them through the accessors below. Public only so the hub can
 * keep the engine on its stack. */
struct eng {
    struct eng_params p;
    enum eng_phase phase;
    uint32_t blocks;
    enum eng_lock lock_reason;
    uint64_t now_ns; /* latest time seen from any input */

    bool params_ok;

    struct soil_filter soil;
    uint32_t soil_rail_run; /* raw readings in a row at or above soil_rail_high */
    uint32_t soil_ok_run;   /* raw readings in a row below it */
    bool soil_rail;

    struct level_filter level;
    enum eng_reservoir res;
    uint32_t res_low_run, res_ok_run; /* filtered readings in a row below / above the reserve */
    uint64_t level_resets_seen;      /* level.resets when we last looked */

    bool warm_started, warm_done;
    uint64_t warm_start_ns;

    bool soil_dry;         /* smoothed soil is dry (with hysteresis) and trustworthy */
    uint64_t dry_since_ns; /* when the current dry stretch began (valid in ENG_DRY_DWELL) */

    struct eng_start hist[ENG_HISTORY]; /* starts in the last 24 h, oldest first */
    unsigned n_hist;
    uint64_t capped_until_ns; /* history overflowed (corrupt log?): treat as capped until then */

    /* Restore (before live input): see eng_restore_event. */
    bool live;                /* live input has started: restore is closed */
    bool r_have;              /* r_last is set */
    uint64_t r_last;          /* monotonic time of the last record replayed */
    uint64_t r_offset;        /* added to monotonic times to get the continuous clock */
    struct eng_restored pending[ENG_HISTORY];
    unsigned n_pending;
    bool r_overflow;
    uint64_t restored;        /* WATER_INTENT events accepted */

    /* The current watering. */
    uint64_t requested_ns;      /* START requested */
    uint64_t started_ns;        /* pump control confirmed the start */
    bool stop_requested;        /* we asked for STOP and it is not confirmed yet */
    uint64_t stop_requested_ns;
    uint32_t ran_ms;            /* as reported when the dose ended */
    enum eng_stop stop_why;

    uint64_t samples;            /* accepted samples */
    uint64_t rejected;           /* records that were not a valid soil/level sample */
    uint64_t unexpected_reports; /* pump-control reports that did not match our phase */
    uint64_t internal_faults;    /* outputs rejected by eng_enforce_output */
};

/* Returns 0. Returns -1 if the params are invalid: the engine is then left in ENG_LOCKOUT with
 * every block set, so it can never request watering. */
int eng_init(struct eng *e, const struct eng_params *p, uint64_t now_ns);

/* A soil or level record, exactly as the hub stored it. Event records and malformed samples
 * are counted in `rejected` and otherwise ignored. */
void eng_on_sample(struct eng *e, const struct record *r, struct eng_out *out);

/* After a restart, the hub replays the log so that a restart (or reboot) cannot reset the rate
 * limit or the daily cap. Call after eng_init and before any live input:
 *   - eng_restore_event() for log records, oldest first. kind is the engine event the record
 *     holds, or ENG_EV_NONE for any other record (samples, START...): only its time is used.
 *     Only WATER_INTENT changes the limits. Pass every WATER_INTENT in the log, unfiltered.
 *   - eng_restore_done() once at the end. (The first live input does it if the hub forgets.)
 * t_ns is the record's CLOCK_MONOTONIC time, which restarts at every boot. The engine sees a
 * reboot wherever the time goes backwards, and joins the boots end to end, counting the time
 * between them as zero. That gives a LOWER BOUND on how long ago each start was, so a past start
 * can only look more recent than it really was: safe, and not much more cautious than needed.
 * The more records the hub replays (not only intents), the tighter the bound.
 * A dose that is <= 0 or >= the pump cap is unreadable: it counts as a full pump cap. */
void eng_restore_event(struct eng *e, enum eng_event_kind kind, int32_t value, uint64_t t_ns);
void eng_restore_done(struct eng *e);

/* Call at least once per second. Drives the time-based rules. */
void eng_tick(struct eng *e, uint64_t now_ns, struct eng_out *out);

/* Reports from the pump-control layer: what actually happened, as opposed to what was requested.
 * "Started" is expected only in ENG_START_REQUESTED. A start the engine did not ask for, or a
 * stop saying the pump ran when the engine had not asked, is ENG_LOCK_UNEXPECTED_PUMP. */
void eng_on_watering_started(struct eng *e, uint64_t now_ns, struct eng_out *out);
void eng_on_watering_stopped(struct eng *e, uint64_t now_ns, uint32_t ran_ms, enum eng_stop why,
                             struct eng_out *out);

/* NULL if `out` is safe to hand to the hub; otherwise what is wrong with it. Rules:
 *  - no critical event was dropped, and at most one of each critical kind was emitted;
 *  - START only from ENG_START_REQUESTED, with no blocks set, with 0 < dose_ms < pump_cap_ms
 *    and dose_ms < CFG_PUMP_CAP_MS (checked again here even though the params were checked),
 *    and with a durable WATER_INTENT carrying the same dose_ms;
 *  - dose_ms is 0 unless the request is START. */
const char *eng_check_output(const struct eng *e, const struct eng_out *out);
/* Called last by every input function. If eng_check_output fails: the request becomes STOP,
 * the engine enters ENG_LOCKOUT with ENG_LOCK_INTERNAL, and internal_faults is counted.
 * Public so tests can hand it deliberately bad outputs. */
void eng_enforce_output(struct eng *e, struct eng_out *out);

enum eng_phase eng_phase(const struct eng *e);
uint32_t eng_blocks(const struct eng *e);
enum eng_lock eng_lock_reason(const struct eng *e);
enum eng_reservoir eng_reservoir(const struct eng *e);
/* The DECISION that the plant needs water: soil dry for dry_dwell_ms in ENG_DRY_DWELL. It does not
 * mean a start was requested: that also needs every block clear (sensors, reservoir, rate limit,
 * daily cap), and the request itself is only a request. */
bool eng_needs_water(const struct eng *e);

/* Short stable names for logs and plantctl. Unknown values give "?". */
const char *eng_phase_name(enum eng_phase ph);
const char *eng_block_name(uint32_t one_bit);
const char *eng_diag_name(enum eng_diag d);
const char *eng_lock_name(enum eng_lock l);
const char *eng_request_name(enum eng_request r);
const char *eng_reservoir_name(enum eng_reservoir r);

#endif
