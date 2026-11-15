/* Watering decision engine (owner: Sam). See engine.h for the contract.
 * Pure: the only headers used are for types and constants. No system calls in this file. */
#include "analysis/engine.h"

#include <string.h>

#include "common/config.h"

/* ---------------------------------------------------------------- parameters */

void eng_params_default(struct eng_params *p)
{
    memset(p, 0, sizeof *p);
    p->soil_dry_enter = CFG_ENG_SOIL_DRY_ENTER;
    p->soil_dry_exit = CFG_ENG_SOIL_DRY_EXIT;
    p->soil_rail_high = CFG_ENG_SOIL_RAIL_HIGH;
    p->dry_dwell_ms = CFG_ENG_DRY_DWELL_MS;
    p->level_empty_dist_mm = CFG_ENG_LEVEL_EMPTY_DIST_MM;
    p->level_full_dist_mm = CFG_ENG_LEVEL_FULL_DIST_MM;
    p->level_reserve_mm = CFG_ENG_LEVEL_RESERVE_MM;
    p->level_hyst_mm = CFG_ENG_LEVEL_HYST_MM;
    p->level_confirm = CFG_ENG_LEVEL_CONFIRM;
    p->no_echo_run = CFG_ENG_NO_ECHO_RUN;
    p->soil_rail_run = CFG_ENG_SOIL_RAIL_RUN;
    p->dose_ms = CFG_ENG_DOSE_MS;
    p->pump_cap_ms = CFG_PUMP_CAP_MS;
    p->min_interval_ms = CFG_ENG_MIN_INTERVAL_MS;
    p->daily_dose_ms = CFG_ENG_DAILY_DOSE_MS;
    p->stale_ms = CFG_ENG_STALE_MS;
    p->warmup_ms = CFG_ENG_WARMUP_MS;
}

const char *eng_params_check(const struct eng_params *p)
{
    if (p->soil_dry_enter < 0)
        return "soil_dry_enter < 0";
    if (p->soil_dry_exit <= p->soil_dry_enter)
        return "soil_dry_exit must be > soil_dry_enter (hysteresis)";
    if (p->soil_rail_high <= p->soil_dry_exit || p->soil_rail_high > 1023)
        return "soil_rail_high must be in (soil_dry_exit, 1023]";
    if (p->dry_dwell_ms == 0)
        return "dry_dwell_ms is 0";
    if (p->level_full_dist_mm <= 0)
        return "level_full_dist_mm must be > 0";
    /* Larger distance = less water, so "empty" is the FARTHER of the two. */
    if (p->level_empty_dist_mm <= p->level_full_dist_mm)
        return "level_empty_dist_mm must be > level_full_dist_mm (larger distance = less water)";
    if (p->level_reserve_mm <= 0 ||
        p->level_reserve_mm >= p->level_empty_dist_mm - p->level_full_dist_mm)
        return "level_reserve_mm must be in (0, empty_dist - full_dist)";
    if (p->level_hyst_mm < 0 ||
        p->level_reserve_mm + p->level_hyst_mm >= p->level_empty_dist_mm - p->level_full_dist_mm)
        return "level_hyst_mm must be >= 0 and reserve + hyst below a full tank";
    if (p->level_confirm == 0)
        return "level_confirm is 0";
    if (p->no_echo_run == 0)
        return "no_echo_run is 0";
    if (p->soil_rail_run == 0)
        return "soil_rail_run is 0";
    if (p->pump_cap_ms == 0 || p->pump_cap_ms > CFG_PUMP_CAP_MS)
        return "pump_cap_ms must be in (0, CFG_PUMP_CAP_MS]";
    if (p->dose_ms == 0 || p->dose_ms >= p->pump_cap_ms)
        return "dose_ms must be in (0, pump_cap_ms)";
    if (p->min_interval_ms < p->dose_ms)
        return "min_interval_ms must be >= dose_ms";
    if (p->daily_dose_ms < p->dose_ms)
        return "daily_dose_ms must allow at least one dose";
    if (p->stale_ms == 0)
        return "stale_ms is 0";
    return NULL;
}

int32_t eng_water_height_mm(const struct eng_params *p, int32_t dist_mm)
{
    return p->level_empty_dist_mm - dist_mm;
}

/* ---------------------------------------------------------------- output buffer */

bool eng_event_is_critical(enum eng_event_kind k)
{
    return k == ENG_EV_WATER_INTENT || k == ENG_EV_DIAG || k == ENG_EV_LOCKOUT;
}

bool eng_out_push(struct eng_out *out, enum eng_event_kind kind, int32_t value, bool durable)
{
    bool critical = eng_event_is_critical(kind);
    unsigned limit = critical ? ENG_MAX_EVENTS : ENG_MAX_EVENTS - ENG_RESERVED_EVENTS;
    if (out->n_events >= limit) {
        if (critical)
            out->dropped_critical++;
        else
            out->dropped_info++;
        return false;
    }
    out->ev[out->n_events++] = (struct eng_event){.kind = kind, .value = value, .durable = durable};
    return true;
}

static void out_reset(struct eng_out *out)
{
    memset(out, 0, sizeof *out);
}

/* ---------------------------------------------------------------- output check */

static unsigned count_kind(const struct eng_out *out, enum eng_event_kind k)
{
    unsigned n = 0;
    for (unsigned i = 0; i < out->n_events; i++)
        n += out->ev[i].kind == k;
    return n;
}

const char *eng_check_output(const struct eng *e, const struct eng_out *out)
{
    if (out->n_events > ENG_MAX_EVENTS)
        return "n_events > ENG_MAX_EVENTS";
    if (out->dropped_critical > 0)
        return "a critical event did not fit";
    if (count_kind(out, ENG_EV_WATER_INTENT) > 1 || count_kind(out, ENG_EV_DIAG) > 1 ||
        count_kind(out, ENG_EV_LOCKOUT) > 1)
        return "more than one critical event of the same kind";

    if (out->request != ENG_REQ_START)
        return out->dose_ms == 0 ? NULL : "dose_ms set without a START request";

    /* START: re-check everything, even what eng_params_check already guaranteed. */
    if (out->dose_ms == 0)
        return "START with dose_ms 0";
    if (out->dose_ms >= e->p.pump_cap_ms || out->dose_ms >= CFG_PUMP_CAP_MS)
        return "START dose_ms not below the pump cap";
    if (e->phase != ENG_START_REQUESTED)
        return "START outside ENG_START_REQUESTED";
    if (e->blocks != 0)
        return "START while blocked";
    bool intent = false;
    for (unsigned i = 0; i < out->n_events; i++)
        if (out->ev[i].kind == ENG_EV_WATER_INTENT && out->ev[i].durable &&
            out->ev[i].value == (int32_t)out->dose_ms)
            intent = true;
    if (!intent)
        return "START without a durable WATER_INTENT for the same dose";
    return NULL;
}

/* ---------------------------------------------------------------- state helpers */

static void note_time(struct eng *e, uint64_t t)
{
    if (t > e->now_ns)
        e->now_ns = t;
}

static void set_phase(struct eng *e, enum eng_phase ph, struct eng_out *out)
{
    if (e->phase == ph)
        return;
    e->phase = ph;
    eng_out_push(out, ENG_EV_PHASE, (int32_t)ph, false);
}

/* Latches the first reason, asks for the pump to be off, and stays there until acknowledged.
 * LOCKOUT is critical, so it always fits (reserved slots); it goes first only so the log shows
 * the reason before the phase change. */
static void enter_lockout(struct eng *e, enum eng_lock why, struct eng_out *out)
{
    out->request = ENG_REQ_STOP;
    out->dose_ms = 0;
    if (e->phase == ENG_LOCKOUT)
        return;
    e->lock_reason = why;
    eng_out_push(out, ENG_EV_LOCKOUT, (int32_t)why, true);
    set_phase(e, ENG_LOCKOUT, out);
}

void eng_enforce_output(struct eng *e, struct eng_out *out)
{
    if (eng_check_output(e, out) == NULL)
        return;
    e->internal_faults++;
    /* Never pass an unsafe request on. If the engine was already locked out, enter_lockout
     * keeps the earlier reason; it still forces STOP. */
    enter_lockout(e, ENG_LOCK_INTERNAL, out);
}

/* ---------------------------------------------------------------- lifecycle */

int eng_init(struct eng *e, const struct eng_params *p, uint64_t now_ns)
{
    memset(e, 0, sizeof *e);
    e->now_ns = now_ns;
    e->p = *p;
    if (eng_params_check(p) != NULL) {
        /* Unusable configuration: make sure this engine can never ask for water. */
        e->phase = ENG_LOCKOUT;
        e->blocks = ENG_BLK_ALL;
        return -1;
    }
    e->params_ok = true;
    e->phase = ENG_IDLE;
    uint64_t stale_ns = (uint64_t)p->stale_ms * 1000000u;
    soil_filter_init(&e->soil, stale_ns);
    level_filter_init(&e->level, stale_ns);
    e->res = ENG_RES_UNKNOWN;
    /* Nothing has been measured yet, so nothing is known to be safe. */
    e->blocks = ENG_BLK_SOIL_STALE | ENG_BLK_LEVEL_STALE | ENG_BLK_RESERVOIR_LOW | ENG_BLK_WARMUP;
    e->lock_reason = ENG_LOCK_NONE;
    return 0;
}

/* ---------------------------------------------------------------- inputs */

/* Soil pinned at the top rail (shorted or wet electronics) is a probe fault. It takes
 * soil_rail_run readings in a row to set and the same number of normal readings to clear,
 * so one odd reading neither raises nor hides it. */
static void soil_rail_update(struct eng *e, int32_t raw)
{
    if (raw >= e->p.soil_rail_high) {
        e->soil_rail_run++;
        e->soil_ok_run = 0;
    } else {
        e->soil_ok_run++;
        e->soil_rail_run = 0;
    }
    if (e->soil_rail_run >= e->p.soil_rail_run)
        e->soil_rail = true;
    else if (e->soil_ok_run >= e->p.soil_rail_run)
        e->soil_rail = false;
}

/* Reservoir state from the FILTERED distance. Changing OK <-> LOW takes level_confirm filtered
 * readings in a row; the median already removed single stray echoes, so one low reading is
 * ignored but a tank that stays low is caught within a few seconds. */
static void reservoir_update(struct eng *e)
{
    if (e->level.resets != e->level_resets_seen) {
        /* Level data restarted after a gap: anything could have happened meanwhile. A LOW tank
         * stays LOW; an OK tank must be confirmed again. */
        e->level_resets_seen = e->level.resets;
        e->res_low_run = e->res_ok_run = 0;
        if (e->res == ENG_RES_OK)
            e->res = ENG_RES_UNKNOWN;
    }
    if (!level_filter_ready(&e->level))
        return;

    int32_t dist = level_filter_dist_mm(&e->level);
    if (dist < e->p.level_full_dist_mm) {
        /* Closer than a full tank: a hand, a splash, or the sensor's dead zone, not water.
         * It confirms nothing in either direction. */
        e->res_low_run = e->res_ok_run = 0;
        return;
    }
    int32_t water = eng_water_height_mm(&e->p, dist); /* larger distance -> less water */
    if (water < e->p.level_reserve_mm) {
        e->res_low_run++;
        e->res_ok_run = 0;
    } else if (water >= e->p.level_reserve_mm + e->p.level_hyst_mm) {
        e->res_ok_run++;
        e->res_low_run = 0;
    } else {
        e->res_low_run = e->res_ok_run = 0; /* in the hysteresis band: keep the current state */
    }
    if (e->res_low_run >= e->p.level_confirm)
        e->res = ENG_RES_LOW;
    else if (e->res_ok_run >= e->p.level_confirm)
        e->res = ENG_RES_OK;
}

/* Feeds a valid sample to its filter. Returns false (caller counts it as rejected) if r is not
 * one; a rejected record never reaches a filter, so it cannot make a sensor look alive. */
static bool take_sample(struct eng *e, const struct record *r)
{
    if (r->flags & REC_F_EVENT)
        return false;
    if (r->sensor_id == SENSOR_SOIL) {
        if (r->flags != 0 || r->value < 0 || r->value > 1023)
            return false;
        soil_rail_update(e, r->value);
        soil_filter_add(&e->soil, r->value, r->mono_ns);
        return true;
    }
    if (r->sensor_id == SENSOR_LEVEL) {
        if ((r->flags & ~REC_F_NO_ECHO) != 0)
            return false;
        if (r->flags & REC_F_NO_ECHO) {
            level_filter_add_no_echo(&e->level); /* not a distance */
            return true;
        }
        if (r->value < 0)
            return false;
        level_filter_add_echo(&e->level, r->value, r->mono_ns);
        reservoir_update(e);
        return true;
    }
    return false;
}

/* Recomputes the sensor blocks from scratch at the current time. Default is blocked: each bit
 * clears only when its sensor positively shows it is fine. */
static void update_blocks(struct eng *e, struct eng_out *out)
{
    if (!e->params_ok)
        return; /* stays ENG_BLK_ALL */
    uint64_t stale_ns = (uint64_t)e->p.stale_ms * 1000000u;
    bool soil_fresh = soil_filter_ready(&e->soil) && e->now_ns - e->soil.newest_ns <= stale_ns;
    bool level_fresh = level_filter_ready(&e->level) && e->now_ns - e->level.newest_ns <= stale_ns;

    uint32_t b = e->blocks & (ENG_BLK_RATE_LIMIT | ENG_BLK_DAILY_CAP | ENG_BLK_STAYS_WET);
    if (!soil_fresh)
        b |= ENG_BLK_SOIL_STALE;
    if (!level_fresh)
        b |= ENG_BLK_LEVEL_STALE;
    if (e->level.no_echo_run >= e->p.no_echo_run)
        b |= ENG_BLK_LEVEL_NO_ECHO;
    if (e->soil_rail)
        b |= ENG_BLK_SOIL_RAIL;
    if (e->res != ENG_RES_OK)
        b |= ENG_BLK_RESERVOIR_LOW;

    /* Warm-up: both sensors fresh, without a break, for warmup_ms. Only once, after start. */
    if (!e->warm_done) {
        if (soil_fresh && level_fresh) {
            if (!e->warm_started) {
                e->warm_started = true;
                e->warm_start_ns = e->now_ns;
            }
            if (e->now_ns - e->warm_start_ns >= (uint64_t)e->p.warmup_ms * 1000000u)
                e->warm_done = true;
        } else {
            e->warm_started = false;
        }
    }
    if (!e->warm_done)
        b |= ENG_BLK_WARMUP;

    if (b != e->blocks) {
        e->blocks = b;
        eng_out_push(out, ENG_EV_BLOCKS, (int32_t)b, false);
    }
}

void eng_on_sample(struct eng *e, const struct record *r, struct eng_out *out)
{
    out_reset(out);
    /* An engine with invalid params never set up its filters: it ignores every sample. */
    if (e->params_ok && take_sample(e, r)) {
        e->samples++;
        note_time(e, r->mono_ns);
    } else {
        e->rejected++;
    }
    update_blocks(e, out);
    eng_enforce_output(e, out);
}

void eng_tick(struct eng *e, uint64_t now_ns, struct eng_out *out)
{
    out_reset(out);
    note_time(e, now_ns);
    update_blocks(e, out); /* a sensor that has gone quiet turns stale here */
    eng_enforce_output(e, out);
}

void eng_on_watering_started(struct eng *e, uint64_t now_ns, struct eng_out *out)
{
    out_reset(out);
    note_time(e, now_ns);
    if (e->phase == ENG_START_REQUESTED) {
        /* Confirmation of our own request: ENG_WATERING, timers etc. come in commit 5. */
    } else {
        /* The pump is running and we did not just ask for it (idle, already watering,
         * verifying, or locked out): stop it and wait for a person. */
        e->unexpected_reports++;
        enter_lockout(e, ENG_LOCK_UNEXPECTED_PUMP, out);
    }
    eng_enforce_output(e, out);
}

void eng_on_watering_stopped(struct eng *e, uint64_t now_ns, uint32_t ran_ms, enum eng_stop why,
                             struct eng_out *out)
{
    out_reset(out);
    note_time(e, now_ns);
    (void)why; /* used from commit 5 */
    if (e->phase == ENG_START_REQUESTED || e->phase == ENG_WATERING) {
        /* Our dose ended, or pump control refused it: handled in commit 5. */
    } else {
        e->unexpected_reports++;
        /* A stop with ran_ms == 0 is harmless (e.g. confirming a STOP we sent while idle).
         * A stop saying the pump actually ran means water moved without our request. */
        if (ran_ms > 0)
            enter_lockout(e, ENG_LOCK_UNEXPECTED_PUMP, out);
    }
    eng_enforce_output(e, out);
}

/* ---------------------------------------------------------------- accessors and names */

enum eng_phase eng_phase(const struct eng *e) { return e->phase; }
uint32_t eng_blocks(const struct eng *e) { return e->blocks; }
enum eng_lock eng_lock_reason(const struct eng *e) { return e->lock_reason; }
enum eng_reservoir eng_reservoir(const struct eng *e) { return e->res; }

const char *eng_phase_name(enum eng_phase ph)
{
    switch (ph) {
    case ENG_IDLE: return "IDLE";
    case ENG_DRY_DWELL: return "DRY_DWELL";
    case ENG_START_REQUESTED: return "START_REQUESTED";
    case ENG_WATERING: return "WATERING";
    case ENG_VERIFYING: return "VERIFYING";
    case ENG_LOCKOUT: return "LOCKOUT";
    }
    return "?";
}

const char *eng_block_name(uint32_t one_bit)
{
    switch (one_bit) {
    case ENG_BLK_SOIL_STALE: return "SOIL_STALE";
    case ENG_BLK_LEVEL_STALE: return "LEVEL_STALE";
    case ENG_BLK_LEVEL_NO_ECHO: return "LEVEL_NO_ECHO";
    case ENG_BLK_SOIL_RAIL: return "SOIL_RAIL";
    case ENG_BLK_RESERVOIR_LOW: return "RESERVOIR_LOW";
    case ENG_BLK_WARMUP: return "WARMUP";
    case ENG_BLK_RATE_LIMIT: return "RATE_LIMIT";
    case ENG_BLK_DAILY_CAP: return "DAILY_CAP";
    case ENG_BLK_STAYS_WET: return "STAYS_WET";
    }
    return "?";
}

const char *eng_diag_name(enum eng_diag d)
{
    switch (d) {
    case ENG_DIAG_NONE: return "NONE";
    case ENG_DIAG_OK: return "OK";
    case ENG_DIAG_PUMP_NO_FLOW: return "PUMP_NO_FLOW";
    case ENG_DIAG_LEAK_OR_PROBE_OUT: return "LEAK_OR_PROBE_OUT";
    case ENG_DIAG_LEVEL_NOT_TRACKING: return "LEVEL_NOT_TRACKING";
    case ENG_DIAG_INCONCLUSIVE: return "INCONCLUSIVE";
    case ENG_DIAG_CHANNELING: return "CHANNELING";
    }
    return "?";
}

const char *eng_lock_name(enum eng_lock l)
{
    switch (l) {
    case ENG_LOCK_NONE: return "NONE";
    case ENG_LOCK_PUMP_NO_FLOW: return "PUMP_NO_FLOW";
    case ENG_LOCK_LEAK_OR_PROBE_OUT: return "LEAK_OR_PROBE_OUT";
    case ENG_LOCK_LEVEL_NOT_TRACKING: return "LEVEL_NOT_TRACKING";
    case ENG_LOCK_PUMP_TIMEOUT: return "PUMP_TIMEOUT";
    case ENG_LOCK_UNFINISHED_WATERING: return "UNFINISHED_WATERING";
    case ENG_LOCK_ACTUATOR: return "ACTUATOR";
    case ENG_LOCK_UNEXPECTED_PUMP: return "UNEXPECTED_PUMP";
    case ENG_LOCK_INTERNAL: return "INTERNAL";
    }
    return "?";
}

const char *eng_request_name(enum eng_request r)
{
    switch (r) {
    case ENG_REQ_NONE: return "NONE";
    case ENG_REQ_START: return "START";
    case ENG_REQ_STOP: return "STOP";
    }
    return "?";
}

const char *eng_reservoir_name(enum eng_reservoir r)
{
    switch (r) {
    case ENG_RES_UNKNOWN: return "UNKNOWN";
    case ENG_RES_OK: return "OK";
    case ENG_RES_LOW: return "LOW";
    }
    return "?";
}
