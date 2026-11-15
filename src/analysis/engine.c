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
    p->start_confirm_ms = CFG_ENG_START_CONFIRM_MS;
    p->stop_grace_ms = CFG_ENG_STOP_GRACE_MS;
    p->verify_window_ms = CFG_ENG_VERIFY_WINDOW_MS;
    p->level_noise_mm = CFG_ENG_LEVEL_NOISE_MM;
    p->level_drop_min_mm = CFG_ENG_LEVEL_DROP_MIN_MM;
    p->soil_noise = CFG_ENG_SOIL_NOISE;
    p->soil_rise_min = CFG_ENG_SOIL_RISE_MIN;
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
    if (p->min_interval_ms < ENG_DAY_MS / ENG_HISTORY)
        return "min_interval_ms too short: more starts per day than ENG_HISTORY can hold";
    if (p->daily_dose_ms < p->dose_ms)
        return "daily_dose_ms must allow at least one dose";
    if (p->stale_ms == 0)
        return "stale_ms is 0";
    if (p->start_confirm_ms == 0)
        return "start_confirm_ms is 0";
    /* The engine must ask for STOP, and give up on pump control, before the cap. */
    if (p->stop_grace_ms == 0 || (uint64_t)p->dose_ms + 2u * p->stop_grace_ms >= p->pump_cap_ms)
        return "stop_grace_ms must be > 0 with dose_ms + 2 * stop_grace_ms < pump_cap_ms";
    if (p->verify_window_ms == 0)
        return "verify_window_ms is 0";
    /* A gap between "noise" and "changed" keeps borderline readings out of both. */
    if (p->level_noise_mm < 0 || p->level_drop_min_mm <= p->level_noise_mm)
        return "level_drop_min_mm must be > level_noise_mm >= 0";
    if (p->soil_noise < 0 || p->soil_rise_min <= p->soil_noise)
        return "soil_rise_min must be > soil_noise >= 0";
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

/* ---------------------------------------------------------------- watering history */

#define MS 1000000ull /* ns per ms */

/* How long ago history entry i was, on this boot's clock. Never negative. */
static uint64_t hist_age_ns(const struct eng *e, unsigned i)
{
    int64_t now = (int64_t)e->now_ns;
    return now > e->hist[i].t_ns ? (uint64_t)(now - e->hist[i].t_ns) : 0;
}

static uint64_t elapsed_ns(uint64_t now, uint64_t then)
{
    return now > then ? now - then : 0;
}

/* Forget starts that are 24 h old or more. */
static void history_prune(struct eng *e)
{
    unsigned k = 0;
    for (unsigned i = 0; i < e->n_hist; i++)
        if (hist_age_ns(e, i) < (uint64_t)ENG_DAY_MS * MS)
            e->hist[k++] = e->hist[i];
    e->n_hist = k;
}

static void history_add(struct eng *e, int64_t t_ns, uint32_t dose_ms)
{
    history_prune(e);
    if (e->n_hist == ENG_HISTORY) {
        /* Cannot happen in live operation (see eng_params_check). From a corrupt log: rather
         * than forget a start, refuse all watering for a day. */
        e->capped_until_ns = e->now_ns + (uint64_t)ENG_DAY_MS * MS;
        return;
    }
    e->hist[e->n_hist++] = (struct eng_start){.t_ns = t_ns, .dose_ms = dose_ms};
}

static bool rate_limited(const struct eng *e)
{
    for (unsigned i = 0; i < e->n_hist; i++)
        if (hist_age_ns(e, i) < (uint64_t)e->p.min_interval_ms * MS)
            return true;
    return false;
}

/* True if one more dose would exceed the daily cap. Call after history_prune. */
static bool daily_capped(const struct eng *e)
{
    if (e->now_ns < e->capped_until_ns)
        return true;
    uint64_t used = 0;
    for (unsigned i = 0; i < e->n_hist; i++)
        used += e->hist[i].dose_ms;
    return used + e->p.dose_ms > e->p.daily_dose_ms;
}

/* ---------------------------------------------------------------- restore */

/* The continuous clock: monotonic time plus an offset that grows at every reboot (time going
 * backwards), so that it never runs backwards itself and never runs ahead of real time. */
static uint64_t restore_clock(struct eng *e, uint64_t t_ns)
{
    if (e->r_have && t_ns < e->r_last)
        e->r_offset += e->r_last - t_ns; /* reboot: join the boots, assume no time between */
    e->r_have = true;
    e->r_last = t_ns;
    return t_ns + e->r_offset;
}

void eng_restore_event(struct eng *e, enum eng_event_kind kind, int32_t value, uint64_t t_ns)
{
    if (!e->params_ok || e->live)
        return;
    uint64_t c = restore_clock(e, t_ns);

    /* Already known to be 24 h old (lower bound): it no longer counts. */
    unsigned k = 0;
    for (unsigned i = 0; i < e->n_pending; i++)
        if (c - e->pending[i].c_ns < (uint64_t)ENG_DAY_MS * MS)
            e->pending[k++] = e->pending[i];
    e->n_pending = k;

    if (kind != ENG_EV_WATER_INTENT)
        return;
    uint32_t dose = (value <= 0 || (uint32_t)value >= e->p.pump_cap_ms)
                        ? e->p.pump_cap_ms /* unreadable: assume the worst */
                        : (uint32_t)value;
    if (e->n_pending == ENG_HISTORY) {
        e->r_overflow = true; /* more than a day can hold: corrupt log */
        return;
    }
    e->pending[e->n_pending++] = (struct eng_restored){.c_ns = c, .dose_ms = dose};
    e->restored++;
}

void eng_restore_done(struct eng *e)
{
    if (!e->params_ok || e->live)
        return;
    e->live = true;
    /* "Now" is the next point on the continuous clock (a reboot since the last record shows
     * up as now < r_last). Ages are lower bounds; turn them into times on this boot's clock. */
    uint64_t c_now = e->r_have ? restore_clock(e, e->now_ns) : e->now_ns;
    for (unsigned i = 0; i < e->n_pending; i++) {
        uint64_t age = c_now - e->pending[i].c_ns;
        if (age < (uint64_t)ENG_DAY_MS * MS)
            history_add(e, (int64_t)e->now_ns - (int64_t)age, e->pending[i].dose_ms);
    }
    e->n_pending = 0;
    if (e->r_overflow)
        e->capped_until_ns = e->now_ns + (uint64_t)ENG_DAY_MS * MS;
}

/* ---------------------------------------------------------------- watering decision */

bool eng_needs_water(const struct eng *e)
{
    return e->phase == ENG_DRY_DWELL &&
           elapsed_ns(e->now_ns, e->dry_since_ns) >= (uint64_t)e->p.dry_dwell_ms * MS;
}

/* Our module reads LOW when dry. Dry when the smoothed value falls to soil_dry_enter; no longer
 * dry only when it rises back to soil_dry_exit. In between, nothing changes (no flapping).
 * If the soil reading can't be trusted (stale or at the rail), the soil is NOT dry. */
static void update_dryness(struct eng *e)
{
    if (e->blocks & (ENG_BLK_SOIL_STALE | ENG_BLK_SOIL_RAIL)) {
        e->soil_dry = false;
        return;
    }
    int32_t v = soil_filter_value(&e->soil);
    if (v <= e->p.soil_dry_enter)
        e->soil_dry = true;
    else if (v >= e->p.soil_dry_exit)
        e->soil_dry = false;
}

/* The one place a start is requested. The durable intent goes first: if it can't be logged,
 * nothing is requested. Counted against the limits at request time (refused starts count too). */
static void request_start(struct eng *e, struct eng_out *out)
{
    uint32_t dose = e->p.dose_ms;
    if (!eng_out_push(out, ENG_EV_WATER_INTENT, (int32_t)dose, true))
        return;
    history_add(e, (int64_t)e->now_ns, dose);
    e->requested_ns = e->now_ns;
    e->stop_requested = false;
    /* "Before" for the verification: filtered values, taken while both sensors are trusted. */
    e->soil_before = soil_filter_value(&e->soil);
    e->soil_peak = e->soil_before;
    e->water_before_mm = eng_water_height_mm(&e->p, level_filter_dist_mm(&e->level));
    e->verify_data_lost = false;
    e->verify_level_drop_mm = e->verify_soil_rise = 0;
    set_phase(e, ENG_START_REQUESTED, out);
    out->request = ENG_REQ_START;
    out->dose_ms = dose;
}

/* Ask for STOP now and on every call until pump control confirms it. */
static void ask_stop(struct eng *e, struct eng_out *out)
{
    out->request = ENG_REQ_STOP;
    out->dose_ms = 0;
    if (!e->stop_requested) {
        e->stop_requested = true;
        e->stop_requested_ns = e->now_ns;
    }
}

static void end_watering(struct eng *e)
{
    e->stop_requested = false;
}

/* ---------------------------------------------------------------- verification */

#define DATA_BLOCKS \
    (ENG_BLK_SOIL_STALE | ENG_BLK_LEVEL_STALE | ENG_BLK_LEVEL_NO_ECHO | ENG_BLK_SOIL_RAIL)

enum band { BAND_NONE, BAND_CHANGED, BAND_UNCLEAR };

/* Within +-noise: no change. At least min: changed. Anything else, including a change the
 * wrong way (water level rose, soil got drier), is too unclear to call. */
static enum band change_band(int32_t change, int32_t noise, int32_t min)
{
    if (change >= min)
        return BAND_CHANGED;
    if (change >= -noise && change <= noise)
        return BAND_NONE;
    return BAND_UNCLEAR;
}

enum eng_diag eng_classify(const struct eng_params *p, int32_t level_drop_mm, int32_t soil_rise)
{
    enum band level = change_band(level_drop_mm, p->level_noise_mm, p->level_drop_min_mm);
    enum band soil = change_band(soil_rise, p->soil_noise, p->soil_rise_min);
    if (level == BAND_UNCLEAR || soil == BAND_UNCLEAR)
        return ENG_DIAG_INCONCLUSIVE;
    if (level == BAND_CHANGED && soil == BAND_CHANGED)
        return ENG_DIAG_OK;                 /* water left the tank and reached the soil */
    if (level == BAND_NONE && soil == BAND_NONE)
        return ENG_DIAG_PUMP_NO_FLOW;       /* nothing moved: pump, relay or line */
    if (level == BAND_CHANGED)
        return ENG_DIAG_LEAK_OR_PROBE_OUT;  /* water left, soil didn't notice */
    return ENG_DIAG_LEVEL_NOT_TRACKING;     /* soil wetter, tank unchanged: gauge not tracking */
}

/* While watering and verifying: remember the wettest smoothed soil, and whether either sensor
 * ever stopped being trustworthy. */
static void track_verification(struct eng *e)
{
    if (e->blocks & DATA_BLOCKS) {
        e->verify_data_lost = true;
        return;
    }
    int32_t v = soil_filter_value(&e->soil);
    if (v > e->soil_peak)
        e->soil_peak = v;
}

static void finish_verification(struct eng *e, enum eng_diag d, struct eng_out *out)
{
    e->last_diag = d;
    eng_out_push(out, ENG_EV_DIAG, (int32_t)d, true);
    switch (d) {
    case ENG_DIAG_PUMP_NO_FLOW: enter_lockout(e, ENG_LOCK_PUMP_NO_FLOW, out); break;
    case ENG_DIAG_LEAK_OR_PROBE_OUT: enter_lockout(e, ENG_LOCK_LEAK_OR_PROBE_OUT, out); break;
    case ENG_DIAG_LEVEL_NOT_TRACKING: enter_lockout(e, ENG_LOCK_LEVEL_NOT_TRACKING, out); break;
    case ENG_DIAG_OK:
    case ENG_DIAG_INCONCLUSIVE:
    case ENG_DIAG_NONE:
    case ENG_DIAG_CHANNELING:
        set_phase(e, ENG_IDLE, out); /* the limits still count this watering */
        break;
    }
}

static void watch_verify(struct eng *e, struct eng_out *out)
{
    track_verification(e);
    if (elapsed_ns(e->now_ns, e->verify_start_ns) < (uint64_t)e->p.verify_window_ms * MS)
        return;
    /* Only a full dose with trustworthy data the whole time can be judged. */
    bool full_dose = e->stop_why == ENG_STOP_DOSE_DONE && e->ran_ms >= e->p.dose_ms;
    if (!full_dose || e->verify_data_lost) {
        finish_verification(e, ENG_DIAG_INCONCLUSIVE, out);
        return;
    }
    int32_t water_after = eng_water_height_mm(&e->p, level_filter_dist_mm(&e->level));
    e->verify_level_drop_mm = e->water_before_mm - water_after;
    e->verify_soil_rise = e->soil_peak - e->soil_before;
    finish_verification(e, eng_classify(&e->p, e->verify_level_drop_mm, e->verify_soil_rise), out);
}

/* START sent, not confirmed yet. */
static void watch_start(struct eng *e, struct eng_out *out)
{
    track_verification(e);
    if (e->blocks & ENG_BLK_SAFETY)
        ask_stop(e, out); /* something went wrong before it even started: cancel */
    if (elapsed_ns(e->now_ns, e->requested_ns) > (uint64_t)e->p.start_confirm_ms * MS)
        enter_lockout(e, ENG_LOCK_ACTUATOR, out); /* no answer: we don't know what the pump is doing */
    else if (e->stop_requested)
        out->request = ENG_REQ_STOP;
}

/* Pump confirmed running. */
static void watch_watering(struct eng *e, struct eng_out *out)
{
    track_verification(e);
    uint64_t on_ns = elapsed_ns(e->now_ns, e->started_ns);
    if (on_ns >= (uint64_t)e->p.pump_cap_ms * MS) {
        /* Backstop: unreachable with valid params (see eng_params_check), kept on purpose. */
        enter_lockout(e, ENG_LOCK_PUMP_TIMEOUT, out);
        return;
    }
    if (e->blocks & ENG_BLK_SAFETY)
        ask_stop(e, out); /* a sensor became unreliable, or the reservoir ran low */
    if (on_ns > ((uint64_t)e->p.dose_ms + e->p.stop_grace_ms) * MS)
        ask_stop(e, out); /* pump control should have ended the dose by now */
    if (e->stop_requested) {
        out->request = ENG_REQ_STOP;
        if (elapsed_ns(e->now_ns, e->stop_requested_ns) > (uint64_t)e->p.stop_grace_ms * MS)
            enter_lockout(e, ENG_LOCK_ACTUATOR, out); /* asked to stop, and it didn't */
    }
}

static void decide(struct eng *e, struct eng_out *out)
{
    if (e->phase == ENG_LOCKOUT) {
        out->request = ENG_REQ_STOP; /* on every call, for as long as it lasts */
        out->dose_ms = 0;
        return;
    }
    if (!e->params_ok)
        return;
    update_dryness(e);
    switch (e->phase) {
    case ENG_IDLE:
        if (e->soil_dry) {
            e->dry_since_ns = e->now_ns;
            set_phase(e, ENG_DRY_DWELL, out);
        }
        break;
    case ENG_DRY_DWELL:
        if (!e->soil_dry)
            set_phase(e, ENG_IDLE, out); /* wet again or untrustworthy: the dwell starts over */
        else if (eng_needs_water(e) && e->blocks == 0)
            request_start(e, out);
        break;
    case ENG_START_REQUESTED:
        watch_start(e, out);
        break;
    case ENG_WATERING:
        watch_watering(e, out);
        break;
    case ENG_VERIFYING:
        watch_verify(e, out);
        break;
    case ENG_LOCKOUT:
        break; /* handled above */
    }
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

    uint32_t b = e->blocks & ENG_BLK_STAYS_WET; /* set by a later commit */
    history_prune(e);
    if (rate_limited(e))
        b |= ENG_BLK_RATE_LIMIT;
    if (daily_capped(e))
        b |= ENG_BLK_DAILY_CAP;
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
    eng_restore_done(e); /* no-op once live */
    /* An engine with invalid params never set up its filters: it ignores every sample. */
    if (e->params_ok && take_sample(e, r)) {
        e->samples++;
        note_time(e, r->mono_ns);
    } else {
        e->rejected++;
    }
    update_blocks(e, out);
    decide(e, out);
    eng_enforce_output(e, out);
}

void eng_tick(struct eng *e, uint64_t now_ns, struct eng_out *out)
{
    out_reset(out);
    eng_restore_done(e); /* no-op once live */
    note_time(e, now_ns);
    update_blocks(e, out); /* a sensor that has gone quiet turns stale here */
    decide(e, out);
    eng_enforce_output(e, out);
}

void eng_on_watering_started(struct eng *e, uint64_t now_ns, struct eng_out *out)
{
    out_reset(out);
    note_time(e, now_ns);
    if (e->phase == ENG_START_REQUESTED) {
        /* The confirmation of our own request: only now is the pump actually running. */
        e->started_ns = e->now_ns;
        set_phase(e, ENG_WATERING, out);
        if (e->stop_requested)
            out->request = ENG_REQ_STOP; /* we cancelled before it started: still stop */
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
    if (e->phase == ENG_START_REQUESTED) {
        if (ran_ms == 0 && e->stop_requested) {
            end_watering(e); /* our cancel, confirmed before any water moved */
            set_phase(e, ENG_IDLE, out);
        } else {
            /* Refused, failed, or ran without confirming the start first. */
            enter_lockout(e, ENG_LOCK_ACTUATOR, out);
        }
    } else if (e->phase == ENG_WATERING) {
        e->ran_ms = ran_ms;
        e->stop_why = why;
        bool we_asked = e->stop_requested; /* read before end_watering() clears it */
        end_watering(e);
        if (why == ENG_STOP_CAP || ran_ms >= e->p.pump_cap_ms)
            enter_lockout(e, ENG_LOCK_PUMP_TIMEOUT, out);
        else if (why == ENG_STOP_ACT_FAIL || ran_ms > e->p.dose_ms + e->p.stop_grace_ms ||
                 (why == ENG_STOP_REQUESTED && !we_asked))
            enter_lockout(e, ENG_LOCK_ACTUATOR, out); /* failed, overran, or the story is wrong */
        else {
            e->verify_start_ns = e->now_ns;
            set_phase(e, ENG_VERIFYING, out);
        }
    } else {
        e->unexpected_reports++;
        /* A stop with ran_ms == 0 is harmless (e.g. confirming a STOP we sent while idle).
         * A stop saying the pump actually ran means water moved without our request. */
        if (ran_ms > 0)
            enter_lockout(e, ENG_LOCK_UNEXPECTED_PUMP, out);
        else if (e->phase == ENG_LOCKOUT)
            out->request = ENG_REQ_STOP;
    }
    eng_enforce_output(e, out);
}

/* ---------------------------------------------------------------- accessors and names */

enum eng_phase eng_phase(const struct eng *e) { return e->phase; }
uint32_t eng_blocks(const struct eng *e) { return e->blocks; }
enum eng_lock eng_lock_reason(const struct eng *e) { return e->lock_reason; }
enum eng_reservoir eng_reservoir(const struct eng *e) { return e->res; }
enum eng_diag eng_last_diag(const struct eng *e) { return e->last_diag; }

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
