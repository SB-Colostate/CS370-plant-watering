/* Watering history and restore (owner: Sam). Rate limit and daily cap from the starts of the
 * last 24 h; after a restart, rebuild them and any lockout from the log. See engine.h. */
#include "analysis/engine.h"

#include "analysis/engine_internal.h"

/* ---------------------------------------------------------------- watering history */

/* How long ago history entry i was, on this boot's clock. Never negative. */
static uint64_t hist_age_ns(const struct eng *e, unsigned i)
{
    int64_t now = (int64_t)e->now_ns;
    return now > e->hist[i].t_ns ? (uint64_t)(now - e->hist[i].t_ns) : 0;
}

uint64_t engi_elapsed_ns(uint64_t now, uint64_t then)
{
    return now > then ? now - then : 0;
}

/* Forget starts that are 24 h old or more. */
void engi_history_prune(struct eng *e)
{
    unsigned k = 0;
    for (unsigned i = 0; i < e->n_hist; i++)
        if (hist_age_ns(e, i) < (uint64_t)ENG_DAY_MS * MS)
            e->hist[k++] = e->hist[i];
    e->n_hist = k;
}

void engi_history_add(struct eng *e, int64_t t_ns, uint32_t dose_ms)
{
    engi_history_prune(e);
    if (e->n_hist == ENG_HISTORY) {
        /* Cannot happen in live operation (see eng_params_check). From a corrupt log: rather
         * than forget a start, refuse all watering for a day. */
        e->capped_until_ns = e->now_ns + (uint64_t)ENG_DAY_MS * MS;
        return;
    }
    e->hist[e->n_hist++] = (struct eng_start){.t_ns = t_ns, .dose_ms = dose_ms};
}

bool engi_rate_limited(const struct eng *e)
{
    for (unsigned i = 0; i < e->n_hist; i++)
        if (hist_age_ns(e, i) < (uint64_t)e->p.min_interval_ms * MS)
            return true;
    return false;
}

/* True if one more dose would exceed the daily cap. Call after history_prune. */
bool engi_daily_capped(const struct eng *e)
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

/* A lockout seen in the log. The first one since the last ACK keeps its reason, as live. */
static void restore_lock(struct eng *e, enum eng_lock why)
{
    if (!e->r_locked) {
        e->r_locked = true;
        e->r_lock_reason = why;
    }
}

static void restore_diag(struct eng *e, int32_t value)
{
    e->r_open = false;
    switch (value) {
    case ENG_DIAG_OK:
        e->inconclusive_run = 0;
        break;
    case ENG_DIAG_CANCELLED:
        break; /* no water moved: neutral */
    case ENG_DIAG_PUMP_NO_FLOW: restore_lock(e, ENG_LOCK_PUMP_NO_FLOW); break;
    case ENG_DIAG_LEAK_OR_PROBE_OUT: restore_lock(e, ENG_LOCK_LEAK_OR_PROBE_OUT); break;
    case ENG_DIAG_LEVEL_NOT_TRACKING: restore_lock(e, ENG_LOCK_LEVEL_NOT_TRACKING); break;
    default: /* INCONCLUSIVE, or a value we can't read: count it as INCONCLUSIVE */
        e->inconclusive_run++;
        if (e->inconclusive_run >= e->p.max_inconclusive)
            restore_lock(e, ENG_LOCK_TOO_MANY_INCONCLUSIVE);
        break;
    }
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

    switch (kind) {
    case ENG_EV_WATER_INTENT:
        break; /* below */
    case ENG_EV_DIAG:
        restore_diag(e, value);
        return;
    case ENG_EV_LOCKOUT:
        e->r_open = false;
        restore_lock(e, value >= ENG_LOCK_PUMP_NO_FLOW && value <= ENG_LOCK_TOO_MANY_INCONCLUSIVE
                            ? (enum eng_lock)value
                            : ENG_LOCK_INTERNAL); /* unreadable reason: still locked */
        return;
    case ENG_EV_ACK:
        e->r_locked = false;
        e->r_lock_reason = ENG_LOCK_NONE;
        e->inconclusive_run = 0;
        return;
    case ENG_EV_NONE:
    case ENG_EV_PHASE:
    case ENG_EV_BLOCKS:
        return; /* time only */
    }
    if (e->r_open)
        restore_lock(e, ENG_LOCK_UNFINISHED_WATERING); /* the previous one never finished */
    e->r_open = true; /* closed by a later DIAG or LOCKOUT */
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

void eng_restore_done(struct eng *e, struct eng_out *out)
{
    /* Public input like any other: clear the output first and check it last. (The implicit
     * calls from the first live input pass an output that was just cleared anyway.) */
    engi_out_reset(out);
    if (!e->params_ok || e->live) {
        eng_enforce_output(e, out);
        return;
    }
    e->live = true;
    /* "Now" is the next point on the continuous clock (a reboot since the last record shows
     * up as now < r_last). Ages are lower bounds; turn them into times on this boot's clock. */
    uint64_t c_now = e->r_have ? restore_clock(e, e->now_ns) : e->now_ns;
    for (unsigned i = 0; i < e->n_pending; i++) {
        uint64_t age = c_now - e->pending[i].c_ns;
        if (age < (uint64_t)ENG_DAY_MS * MS)
            engi_history_add(e, (int64_t)e->now_ns - (int64_t)age, e->pending[i].dose_ms);
    }
    e->n_pending = 0;
    if (e->r_overflow)
        e->capped_until_ns = e->now_ns + (uint64_t)ENG_DAY_MS * MS;

    /* Faults survive restarts. A lockout already in the log is resumed without logging it
     * again; an unfinished watering is a new finding and is logged. */
    if (e->r_locked) {
        e->phase = ENG_LOCKOUT; /* eng_enforce_output below requests STOP */
        e->lock_reason = e->r_lock_reason;
    } else if (e->r_open) {
        engi_enter_lockout(e, ENG_LOCK_UNFINISHED_WATERING, out); /* crashed or rebooted mid-watering */
    } else if (e->inconclusive_run >= e->p.max_inconclusive) {
        engi_enter_lockout(e, ENG_LOCK_TOO_MANY_INCONCLUSIVE, out);
    }
    eng_enforce_output(e, out);
}
