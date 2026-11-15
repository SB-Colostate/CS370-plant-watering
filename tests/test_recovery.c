/* Watering engine, commit 7: repeated INCONCLUSIVE results, unfinished waterings, and lockouts
 * that must survive restarts. Synthetic fixtures. Every event the engine emits is appended to an
 * in-memory "log" with its time, as the hub would save it; a restart is a fresh engine that
 * replays that log (same clock for a hub restart, a clock that starts over for a reboot).
 *
 * Params as in test_verify.c (dose 20 s, verify window 120 s, >= 1 h between starts), plus
 * daily cap 200 s (room for 10 doses) and lockout after 3 INCONCLUSIVE in a row. */
#include <limits.h>
#include <string.h>

#include "analysis/engine.h"
#include "unit.h"

#define SEC 1000000000ull
#define MIN (60 * SEC)
#define HOUR (3600 * SEC)
#define DRY 20
#define FULL 100

/* ---------------------------------------------------------------- the log */

struct logrec {
    uint64_t mono;
    enum eng_event_kind kind;
    int32_t value;
    bool durable;
};
static struct logrec LOG[4096];
static int nlog;

static void log_add(uint64_t t, enum eng_event_kind k, int32_t v, bool durable)
{
    if (nlog < (int)(sizeof LOG / sizeof LOG[0]))
        LOG[nlog++] = (struct logrec){.mono = t, .kind = k, .value = v, .durable = durable};
}

static int count_in_log(enum eng_event_kind k, int32_t v)
{
    int n = 0;
    for (int i = 0; i < nlog; i++)
        n += LOG[i].kind == k && LOG[i].value == v;
    return n;
}

/* ---------------------------------------------------------------- engine + fake pump control */

static struct eng E;
static uint64_t T;
static bool pump_on;
static uint64_t pump_on_since;
static int starts;
static int t_pump; /* seconds since the current watering's pump start; -1 before the first */
static bool pump_confirms = true; /* false: accepts START but never switches on */

typedef void (*shape_fn)(int t, int32_t *soil, int32_t *dist);
static shape_fn shape;

static void apply(const struct eng_out *o);

static void safe(const struct eng_out *o)
{
    const char *why = eng_check_output(&E, o);
    CHECK(why == NULL);
    if (why)
        fprintf(stderr, "    unsafe output: %s\n", why);
    CHECK(o->dropped_info == 0 && o->dropped_critical == 0 && E.internal_faults == 0);
    if (eng_phase(&E) == ENG_LOCKOUT)
        CHECK(o->request == ENG_REQ_STOP);
    for (unsigned i = 0; i < o->n_events; i++)
        log_add(T, o->ev[i].kind, o->ev[i].value, o->ev[i].durable);
}

static void handle(const struct eng_out *o)
{
    safe(o);
    apply(o);
}

static void apply(const struct eng_out *o)
{
    struct eng_out r;
    if (o->request == ENG_REQ_START) {
        starts++;
        if (!pump_confirms)
            return;
        pump_on = true;
        pump_on_since = T;
        t_pump = 0;
        eng_on_watering_started(&E, T, &r);
        handle(&r);
    } else if (o->request == ENG_REQ_STOP && pump_on) {
        pump_on = false;
        eng_on_watering_stopped(&E, T, (uint32_t)((T - pump_on_since) / 1000000u), ENG_STOP_REQUESTED, &r);
        handle(&r);
    }
}

static struct eng_params test_params(void)
{
    struct eng_params p;
    eng_params_default(&p);
    p.soil_dry_enter = 40;
    p.soil_dry_exit = 80;
    p.dry_dwell_ms = 60000;
    p.warmup_ms = 10000;
    p.stale_ms = 5000;
    p.level_empty_dist_mm = 200;
    p.level_full_dist_mm = 40;
    p.level_reserve_mm = 20;
    p.level_hyst_mm = 10;
    p.dose_ms = 20000;
    p.min_interval_ms = 3600000;
    p.daily_dose_ms = 200000;
    p.verify_window_ms = 120000;
    p.level_noise_mm = 5;
    p.level_drop_min_mm = 10;
    p.soil_noise = 10;
    p.soil_rise_min = 30;
    p.max_inconclusive = 3;
    return p;
}

/* A fresh engine at monotonic time t0, replaying the whole log (pass replay = false for the very
 * first start). Returns the output of eng_restore_done, already handled. */
static struct eng_out boot(uint64_t t0, bool replay)
{
    struct eng_params p = test_params();
    CHECK(eng_init(&E, &p, t0) == 0);
    T = t0;
    pump_on = false; /* the supervisor forced it OFF when the old hub died */
    t_pump = -1;
    starts = 0;
    pump_confirms = true;
    if (!replay) {
        nlog = 0;
        shape = NULL;
    }
    int n = nlog; /* replay what was there before this boot */
    for (int i = 0; replay && i < n; i++)
        eng_restore_event(&E, LOG[i].kind, LOG[i].value, LOG[i].mono);
    struct eng_out o;
    eng_restore_done(&E, &o);
    handle(&o);
    return o;
}

static void step(void)
{
    T += SEC;
    if (pump_on && T - pump_on_since >= 20 * SEC) {
        struct eng_out r;
        pump_on = false;
        eng_on_watering_stopped(&E, T, 20000, ENG_STOP_DOSE_DONE, &r);
        handle(&r);
    }
    int32_t soil = DRY, dist = FULL;
    if (t_pump >= 0 && t_pump < 150 && shape)
        shape(t_pump, &soil, &dist);
    if (t_pump >= 0)
        t_pump++;
    struct record r = {.sensor_id = SENSOR_SOIL, .mono_ns = T, .value = soil};
    struct eng_out o;
    eng_on_sample(&E, &r, &o);
    handle(&o);
    r = (struct record){.sensor_id = SENSOR_LEVEL, .mono_ns = T, .value = dist};
    eng_on_sample(&E, &r, &o);
    handle(&o);
    eng_tick(&E, T, &o);
    handle(&o);
    if ((T / SEC) % 60 == 0)
        log_add(T, ENG_EV_NONE, 0, false); /* the real log has samples between events */
}

/* Run (dry plant, healthy sensors) until `n` more diagnoses, a lockout, or `limit_s`. */
static void run_diagnoses(int n, shape_fn s, int limit_s)
{
    shape = s;
    int want = count_in_log(ENG_EV_DIAG, ENG_DIAG_OK) + count_in_log(ENG_EV_DIAG, ENG_DIAG_INCONCLUSIVE) + n;
    for (int i = 0; i < limit_s && eng_phase(&E) != ENG_LOCKOUT; i++) {
        step();
        if (count_in_log(ENG_EV_DIAG, ENG_DIAG_OK) + count_in_log(ENG_EV_DIAG, ENG_DIAG_INCONCLUSIVE) >= want)
            return;
    }
}

static void steps(int n)
{
    for (int i = 0; i < n; i++)
        step();
}

static bool locked(enum eng_lock why) { return eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == why; }

/* What the hub does: ask, save the ACK (handle() appends it to LOG), then confirm the save. */
static struct eng_out ack_and_save(void)
{
    struct eng_out o, s;
    eng_acknowledge(&E, T, &o);
    handle(&o);
    eng_ack_saved(&E, T, &s);
    handle(&s);
    return o;
}

/* Shapes, from the pump start (as in test_verify.c). 100 -> 120 mm distance = 20 mm drop. */
static int32_t dist_drop(int t, int drop) { return FULL + (t < 20 ? drop * t / 20 : drop); }
static void ok_shape(int t, int32_t *s, int32_t *d) { *s = t < 10 ? DRY : 150; *d = dist_drop(t, 20); }
static void unclear_shape(int t, int32_t *s, int32_t *d) { *s = t < 10 ? DRY : 150; *d = dist_drop(t, 7); }
static void noflow_shape(int t, int32_t *s, int32_t *d) { (void)t; *s = DRY; *d = FULL; }

#define TWO_HOURS (2 * 3600)

/* ================================================================ repeated INCONCLUSIVE */

static void test_three_inconclusive_in_a_row_lock_out(void)
{
    boot(0, false);
    run_diagnoses(1, unclear_shape, TWO_HOURS);
    CHECK(eng_last_diag(&E) == ENG_DIAG_INCONCLUSIVE && eng_phase(&E) != ENG_LOCKOUT);
    run_diagnoses(1, unclear_shape, TWO_HOURS);
    CHECK(E.inconclusive_run == 2 && eng_phase(&E) != ENG_LOCKOUT);
    run_diagnoses(1, unclear_shape, TWO_HOURS);
    CHECK(locked(ENG_LOCK_TOO_MANY_INCONCLUSIVE));
    CHECK(count_in_log(ENG_EV_LOCKOUT, ENG_LOCK_TOO_MANY_INCONCLUSIVE) == 1);
    int before = starts;
    steps(3 * 3600);
    CHECK(starts == before); /* no more watering */
}

static void test_ok_in_between_resets_the_run(void)
{
    boot(0, false);
    run_diagnoses(2, unclear_shape, 2 * TWO_HOURS);
    run_diagnoses(1, ok_shape, TWO_HOURS);
    CHECK(eng_last_diag(&E) == ENG_DIAG_OK && E.inconclusive_run == 0);
    run_diagnoses(2, unclear_shape, 2 * TWO_HOURS);
    CHECK(eng_phase(&E) != ENG_LOCKOUT && E.inconclusive_run == 2);
}

static void test_inconclusive_run_survives_hub_restart(void)
{
    boot(0, false);
    run_diagnoses(2, unclear_shape, 2 * TWO_HOURS);
    boot(T + 10 * SEC, true); /* hub restart: same clock */
    CHECK(E.inconclusive_run == 2 && eng_phase(&E) != ENG_LOCKOUT);
    run_diagnoses(1, unclear_shape, TWO_HOURS);
    CHECK(locked(ENG_LOCK_TOO_MANY_INCONCLUSIVE));
}

static void test_inconclusive_run_survives_reboot(void)
{
    boot(0, false);
    run_diagnoses(2, unclear_shape, 2 * TWO_HOURS);
    steps(30 * 60); /* stays up a while (the rate limit holds the next watering back) */
    boot(40 * SEC, true); /* reboot: clock starts over */
    CHECK(E.inconclusive_run == 2);
    run_diagnoses(1, unclear_shape, TWO_HOURS);
    CHECK(locked(ENG_LOCK_TOO_MANY_INCONCLUSIVE));
}

static void test_restore_counts_and_cancels(void)
{
    /* From the log alone: a cancelled start is neutral, an unreadable DIAG counts as inconclusive. */
    struct eng_params p = test_params();
    struct eng_out o;
    eng_init(&E, &p, 10 * HOUR);
    int32_t seq[] = {ENG_DIAG_INCONCLUSIVE, ENG_DIAG_CANCELLED, 99 /* unreadable */};
    for (int i = 0; i < 3; i++) {
        eng_restore_event(&E, ENG_EV_WATER_INTENT, 20000, (uint64_t)(i + 1) * 2 * HOUR);
        eng_restore_event(&E, ENG_EV_DIAG, seq[i], (uint64_t)(i + 1) * 2 * HOUR + 3 * MIN);
    }
    eng_restore_done(&E, &o);
    CHECK(E.inconclusive_run == 2 && eng_phase(&E) != ENG_LOCKOUT);
}

/* ================================================================ unfinished watering */

static void crash_when(enum eng_phase ph)
{
    boot(0, false);
    shape = ok_shape;
    for (int i = 0; i < 400 && eng_phase(&E) != ph; i++)
        step();
    CHECK(eng_phase(&E) == ph);
}

static void check_unfinished_after_restart(uint64_t t0)
{
    struct eng_out o = boot(t0, true);
    CHECK(locked(ENG_LOCK_UNFINISHED_WATERING));
    CHECK(o.request == ENG_REQ_STOP);
    bool logged = false;
    for (unsigned i = 0; i < o.n_events; i++)
        if (o.ev[i].kind == ENG_EV_LOCKOUT && o.ev[i].value == ENG_LOCK_UNFINISHED_WATERING && o.ev[i].durable)
            logged = true;
    CHECK(logged); /* a new finding: written to the log */
    steps(2 * 3600);
    CHECK(starts == 0);
}

static void test_crash_mid_dose(void)
{
    crash_when(ENG_WATERING);
    steps(5); /* pump running... then the hub dies */
    check_unfinished_after_restart(T + 5 * SEC);
}

static void test_crash_before_start_confirmed(void)
{
    boot(0, false);
    pump_confirms = false; /* the start is requested and never confirmed... */
    for (int i = 0; i < 400 && eng_phase(&E) != ENG_START_REQUESTED; i++)
        step();
    CHECK(eng_phase(&E) == ENG_START_REQUESTED); /* ...and the hub dies within the 3 s window */
    check_unfinished_after_restart(T + 5 * SEC);
}

static void test_crash_while_verifying(void)
{
    crash_when(ENG_VERIFYING); /* the pump finished, but nobody checked the water arrived */
    check_unfinished_after_restart(T + 5 * SEC);
}

static void test_reboot_mid_dose(void)
{
    crash_when(ENG_WATERING);
    check_unfinished_after_restart(30 * SEC); /* power cut: clock starts over */
}

static void test_second_restart_stays_locked_without_relogging(void)
{
    crash_when(ENG_WATERING);
    check_unfinished_after_restart(T + 5 * SEC);
    struct eng_out o = boot(T + 5 * SEC, true);
    CHECK(locked(ENG_LOCK_UNFINISHED_WATERING) && o.request == ENG_REQ_STOP);
    CHECK(o.n_events == 0); /* already in the log: resumed, not logged again */
    CHECK(count_in_log(ENG_EV_LOCKOUT, ENG_LOCK_UNFINISHED_WATERING) == 1);
}

/* ================================================================ lockouts survive restarts */

static void test_fault_lockout_survives_restart_and_reboot(void)
{
    boot(0, false);
    run_diagnoses(1, noflow_shape, TWO_HOURS);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW));
    struct eng_out o = boot(T + 10 * SEC, true);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW) && o.request == ENG_REQ_STOP && o.n_events == 0);
    steps(2 * 3600);
    CHECK(starts == 0);
    boot(20 * SEC, true); /* and after a reboot */
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW));
}

static void test_lockout_survives_even_if_lockout_record_lost(void)
{
    boot(0, false);
    run_diagnoses(1, noflow_shape, TWO_HOURS);
    for (int i = 0; i < nlog; i++)
        if (LOG[i].kind == ENG_EV_LOCKOUT)
            LOG[i].kind = ENG_EV_NONE; /* power cut right after the DIAG was saved */
    boot(T + 10 * SEC, true);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW)); /* derived from the DIAG */

    boot(0, false);
    run_diagnoses(3, unclear_shape, 3 * TWO_HOURS);
    for (int i = 0; i < nlog; i++)
        if (LOG[i].kind == ENG_EV_LOCKOUT)
            LOG[i].kind = ENG_EV_NONE;
    boot(T + 10 * SEC, true);
    CHECK(locked(ENG_LOCK_TOO_MANY_INCONCLUSIVE)); /* derived from the run of DIAGs */
}

static void test_unreadable_lock_reason_still_locks(void)
{
    struct eng_params p = test_params();
    struct eng_out o;
    eng_init(&E, &p, HOUR);
    eng_restore_event(&E, ENG_EV_LOCKOUT, 1234, MIN);
    eng_restore_done(&E, &o);
    CHECK(locked(ENG_LOCK_INTERNAL) && o.request == ENG_REQ_STOP);
}

/* ================================================================ acknowledging */

static void test_ack_clears_lockout_but_not_limits(void)
{
    boot(0, false);
    run_diagnoses(1, noflow_shape, TWO_HOURS);
    uint64_t started = 0;
    for (int i = 0; i < nlog; i++)
        if (LOG[i].kind == ENG_EV_WATER_INTENT)
            started = LOG[i].mono;
    struct eng_out o = ack_and_save();
    CHECK(eng_phase(&E) == ENG_IDLE && eng_lock_reason(&E) == ENG_LOCK_NONE);
    CHECK(o.n_events >= 1 && count_in_log(ENG_EV_ACK, ENG_LOCK_PUMP_NO_FLOW) == 1);
    bool durable = false;
    for (unsigned i = 0; i < o.n_events; i++)
        if (o.ev[i].kind == ENG_EV_ACK)
            durable = o.ev[i].durable;
    CHECK(durable);
    /* The rate limit still counts the watering that failed. */
    shape = ok_shape;
    while (starts == 1 && T < started + 2 * HOUR)
        step();
    CHECK(starts == 2 && T >= started + HOUR);
}

static void test_ack_survives_restart(void)
{
    boot(0, false);
    run_diagnoses(1, noflow_shape, TWO_HOURS);
    ack_and_save();
    boot(T + 10 * SEC, true);
    CHECK(eng_phase(&E) != ENG_LOCKOUT);
}

static void test_new_fault_after_ack_is_restored(void)
{
    boot(0, false);
    run_diagnoses(1, noflow_shape, TWO_HOURS);
    ack_and_save();

    shape = ok_shape;
    for (int i = 0; i < 3 * 3600 && eng_phase(&E) != ENG_WATERING; i++)
        step();
    CHECK(eng_phase(&E) == ENG_WATERING);
    boot(T + 5 * SEC, true); /* crash mid-dose after the ack */
    CHECK(locked(ENG_LOCK_UNFINISHED_WATERING)); /* the new fault, not the acknowledged one */
}

static void test_ack_resets_inconclusive_run(void)
{
    boot(0, false);
    run_diagnoses(3, unclear_shape, 3 * TWO_HOURS);
    CHECK(locked(ENG_LOCK_TOO_MANY_INCONCLUSIVE));
    ack_and_save();
    CHECK(E.inconclusive_run == 0);
    run_diagnoses(1, unclear_shape, TWO_HOURS);
    CHECK(eng_phase(&E) != ENG_LOCKOUT && E.inconclusive_run == 1);
    boot(T + 10 * SEC, true);
    CHECK(eng_phase(&E) != ENG_LOCKOUT && E.inconclusive_run == 1); /* the same after a restart */
}

static void test_ack_does_nothing_until_saved(void)
{
    boot(0, false);
    run_diagnoses(1, noflow_shape, TWO_HOURS);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW));
    struct eng_out o;
    eng_acknowledge(&E, T, &o);
    handle(&o);
    CHECK(o.request == ENG_REQ_STOP && E.ack_pending);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW)); /* still locked: the hub hasn't confirmed the save */
    int before = starts;
    shape = ok_shape;
    steps(3 * 3600); /* hours later, dry soil, interval long over: still no watering */
    CHECK(starts == before && locked(ENG_LOCK_PUMP_NO_FLOW));

    /* The save failed and the hub restarted: the ACK is not in the log, so still locked. */
    for (int i = 0; i < nlog; i++)
        if (LOG[i].kind == ENG_EV_ACK)
            LOG[i].kind = ENG_EV_NONE;
    boot(T + 10 * SEC, true);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW) && !E.ack_pending);
}

static void test_ack_saved_without_ack_does_nothing(void)
{
    boot(0, false);
    run_diagnoses(1, noflow_shape, TWO_HOURS);
    struct eng_out o;
    eng_ack_saved(&E, T, &o); /* no eng_acknowledge first */
    handle(&o);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW));
}

/* ================================================================ several attempts in the log */

static struct eng_out restore_from(const struct logrec *recs, int n)
{
    struct eng_params p = test_params();
    struct eng_out o;
    eng_init(&E, &p, 100 * HOUR);
    T = 100 * HOUR;
    for (int i = 0; i < n; i++)
        eng_restore_event(&E, recs[i].kind, recs[i].value, recs[i].mono);
    eng_restore_done(&E, &o);
    safe(&o);
    return o;
}

#define INTENT(h) {(uint64_t)(h) * HOUR, ENG_EV_WATER_INTENT, 20000, true}
#define DIAG(h, d) {(uint64_t)(h) * HOUR + 3 * MIN, ENG_EV_DIAG, (d), true}
#define LOCK(h, r) {(uint64_t)(h) * HOUR + 3 * MIN, ENG_EV_LOCKOUT, (r), true}
#define ACK(h) {(uint64_t)(h) * HOUR + 30 * MIN, ENG_EV_ACK, 0, true}

static void test_restore_several_attempts(void)
{
    /* All closed: not locked; the run counts only since the last OK. */
    struct logrec all_closed[] = {INTENT(1), DIAG(1, ENG_DIAG_OK), INTENT(3), DIAG(3, ENG_DIAG_INCONCLUSIVE),
                                  INTENT(5), DIAG(5, ENG_DIAG_CANCELLED), INTENT(7), DIAG(7, ENG_DIAG_OK)};
    restore_from(all_closed, 8);
    CHECK(eng_phase(&E) != ENG_LOCKOUT && E.inconclusive_run == 0);

    /* The first closed, the last one not: unfinished. */
    struct logrec last_open[] = {INTENT(1), DIAG(1, ENG_DIAG_OK), INTENT(3)};
    struct eng_out o = restore_from(last_open, 3);
    CHECK(locked(ENG_LOCK_UNFINISHED_WATERING) && o.request == ENG_REQ_STOP);

    /* The first not closed, a later one closed: still unfinished (the first never finished). */
    struct logrec first_open[] = {INTENT(1), INTENT(3), DIAG(3, ENG_DIAG_OK)};
    restore_from(first_open, 3);
    CHECK(locked(ENG_LOCK_UNFINISHED_WATERING));

    /* Unfinished, then locked, acknowledged, and a good watering after: not locked. */
    struct logrec acked[] = {INTENT(1), LOCK(2, ENG_LOCK_UNFINISHED_WATERING), ACK(2), INTENT(4), DIAG(4, ENG_DIAG_OK)};
    restore_from(acked, 5);
    CHECK(eng_phase(&E) != ENG_LOCKOUT);

    /* A watering that ended in a lockout (no DIAG) is closed by the LOCKOUT. */
    struct logrec by_lock[] = {INTENT(1), LOCK(1, ENG_LOCK_ACTUATOR)};
    restore_from(by_lock, 2);
    CHECK(locked(ENG_LOCK_ACTUATOR));

    /* Two lockouts with no ACK between: the first reason is kept, as live. */
    struct logrec two_locks[] = {INTENT(1), LOCK(1, ENG_LOCK_ACTUATOR), LOCK(2, ENG_LOCK_UNEXPECTED_PUMP)};
    restore_from(two_locks, 3);
    CHECK(locked(ENG_LOCK_ACTUATOR));

    /* A fault before an ACK, a new unfinished one after it: the new reason. */
    struct logrec after_ack[] = {INTENT(1), DIAG(1, ENG_DIAG_PUMP_NO_FLOW), ACK(2), INTENT(4)};
    restore_from(after_ack, 4);
    CHECK(locked(ENG_LOCK_UNFINISHED_WATERING));
}

static void test_ack_when_not_locked_does_nothing(void)
{
    boot(0, false);
    steps(30);
    struct eng_out o;
    enum eng_phase ph = eng_phase(&E);
    eng_acknowledge(&E, T, &o);
    handle(&o);
    CHECK(o.n_events == 0 && eng_phase(&E) == ph && !E.ack_pending);
}

/* ================================================================ normal recovery */

static void test_restart_after_good_watering(void)
{
    boot(0, false);
    run_diagnoses(1, ok_shape, TWO_HOURS);
    uint64_t first = 0;
    for (int i = 0; i < nlog; i++)
        if (LOG[i].kind == ENG_EV_WATER_INTENT)
            first = LOG[i].mono;
    struct eng_out o = boot(T + 10 * SEC, true);
    CHECK(eng_phase(&E) != ENG_LOCKOUT && o.request != ENG_REQ_STOP && o.n_events == 0);
    CHECK(E.inconclusive_run == 0);
    shape = ok_shape;
    while (starts == 0 && T < first + 2 * HOUR)
        step();
    CHECK(starts == 1 && T >= first + HOUR); /* waters again, after the interval */
}

static void test_first_boot_with_empty_log(void)
{
    struct eng_out o = boot(0, false);
    CHECK(eng_phase(&E) == ENG_IDLE && o.request == ENG_REQ_NONE && o.n_events == 0);
}

int main(void)
{
    RUN(test_three_inconclusive_in_a_row_lock_out);
    RUN(test_ok_in_between_resets_the_run);
    RUN(test_inconclusive_run_survives_hub_restart);
    RUN(test_inconclusive_run_survives_reboot);
    RUN(test_restore_counts_and_cancels);
    RUN(test_crash_mid_dose);
    RUN(test_crash_before_start_confirmed);
    RUN(test_crash_while_verifying);
    RUN(test_reboot_mid_dose);
    RUN(test_second_restart_stays_locked_without_relogging);
    RUN(test_fault_lockout_survives_restart_and_reboot);
    RUN(test_lockout_survives_even_if_lockout_record_lost);
    RUN(test_unreadable_lock_reason_still_locks);
    RUN(test_ack_clears_lockout_but_not_limits);
    RUN(test_ack_survives_restart);
    RUN(test_new_fault_after_ack_is_restored);
    RUN(test_ack_resets_inconclusive_run);
    RUN(test_ack_does_nothing_until_saved);
    RUN(test_ack_saved_without_ack_does_nothing);
    RUN(test_restore_several_attempts);
    RUN(test_ack_when_not_locked_does_nothing);
    RUN(test_restart_after_good_watering);
    RUN(test_first_boot_with_empty_log);
    return unit_done("test_recovery");
}
