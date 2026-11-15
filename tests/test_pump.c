/* Watering engine, commit 5: after a start is requested. The engine never touches the pump; a
 * small fake pump-control layer below plays that part and can misbehave on purpose.
 * Synthetic fixtures: one soil + one level sample per second, then a tick.
 *
 * Test params as in test_watering.c (dry <= 40, dwell 60 s, warm-up 10 s, stale 5 s), plus:
 * dose 20 s, start must be confirmed within 3 s, stop within 3 s, cap 600 s. */
#include <limits.h>
#include <string.h>

#include "analysis/engine.h"
#include "common/config.h"
#include "unit.h"

#define SEC 1000000000ull
#define NONE INT32_MIN
#define NOECHO (-1)
#define DRY 20
#define FULL 100
#define LOW 195

/* ---------------------------------------------------------------- fake pump control */

static struct {
    int confirm_after_s;  /* seconds from START until it switches on and confirms; -1: never */
    bool refuse;          /* answers START with "could not switch" */
    bool stuck_on;        /* does not end the dose by itself */
    bool ignore_stop;     /* does not obey STOP */
    bool on;
    uint64_t on_since;
    uint64_t pending_since; /* START received, not acted on yet */
    bool pending;
    uint32_t dose_ms;
    uint64_t longest_on_ns;
} P;

static struct eng E;
static uint64_t T;
static int starts;
static int lockouts_seen;

static void apply(const struct eng_out *o);
static void pump_tick(void);

/* After EVERY engine call. */
static void safe(const struct eng_out *o)
{
    const char *why = eng_check_output(&E, o);
    CHECK(why == NULL);
    if (why)
        fprintf(stderr, "    unsafe output: %s\n", why);
    CHECK(o->dropped_info == 0 && E.internal_faults == 0);
    if (eng_phase(&E) == ENG_LOCKOUT)
        CHECK(o->request == ENG_REQ_STOP);                     /* locked out: always STOP */
    enum eng_phase ph = eng_phase(&E);
    if (ph == ENG_IDLE || ph == ENG_DRY_DWELL || ph == ENG_VERIFYING)
        CHECK(!P.on);                                          /* never running "behind its back" */
}

static void report_started(void)
{
    struct eng_out o;
    eng_on_watering_started(&E, T, &o);
    safe(&o);
    apply(&o);
}

static void report_stopped(uint32_t ran_ms, enum eng_stop why)
{
    struct eng_out o;
    eng_on_watering_stopped(&E, T, ran_ms, why, &o);
    safe(&o);
    apply(&o);
}

static void pump_off(enum eng_stop why)
{
    uint64_t ran = T - P.on_since;
    if (ran > P.longest_on_ns)
        P.longest_on_ns = ran;
    P.on = false;
    report_stopped((uint32_t)(ran / 1000000u), why);
}

/* Pump control acting on an engine output. */
static void apply(const struct eng_out *o)
{
    if (o->request == ENG_REQ_START) {
        starts++;
        if (P.refuse) {
            report_stopped(0, ENG_STOP_ACT_FAIL);
            return;
        }
        P.pending = true;
        P.pending_since = T;
        P.dose_ms = o->dose_ms;
        if (P.confirm_after_s == 0)
            pump_tick(); /* fast pump control: switches on and confirms right away */
    } else if (o->request == ENG_REQ_STOP && !P.ignore_stop) {
        if (P.on) {
            pump_off(ENG_STOP_REQUESTED);
        } else if (P.pending) {
            P.pending = false;
            report_stopped(0, ENG_STOP_REQUESTED); /* cancelled before it switched on */
        }
    }
}

/* Pump control's own clock: confirm a pending start, end a dose. */
static void pump_tick(void)
{
    if (P.pending && P.confirm_after_s >= 0 && T - P.pending_since >= (uint64_t)P.confirm_after_s * SEC) {
        P.pending = false;
        P.on = true;
        P.on_since = T;
        report_started();
    }
    if (P.on && !P.stuck_on && T - P.on_since >= (uint64_t)P.dose_ms * 1000000u)
        pump_off(ENG_STOP_DOSE_DONE);
    if (P.on && T - P.on_since > P.longest_on_ns)
        P.longest_on_ns = T - P.on_since;
}

/* ---------------------------------------------------------------- harness */

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
    p.daily_dose_ms = 60000;
    p.start_confirm_ms = 3000;
    p.stop_grace_ms = 3000;
    p.pump_cap_ms = CFG_PUMP_CAP_MS;
    return p;
}

static void start_with(const struct eng_params *p)
{
    CHECK(eng_init(&E, p, 0) == 0);
    T = 0;
    starts = 0;
    lockouts_seen = 0;
    memset(&P, 0, sizeof P);
}

static void start(void)
{
    struct eng_params p = test_params();
    start_with(&p);
}

static void step(int32_t soil, int32_t dist)
{
    T += SEC;
    pump_tick();
    struct record r;
    struct eng_out o;
    if (soil != NONE) {
        r = (struct record){.sensor_id = SENSOR_SOIL, .mono_ns = T, .value = soil};
        eng_on_sample(&E, &r, &o);
        safe(&o);
        apply(&o);
    }
    if (dist != NONE) {
        r = (struct record){.sensor_id = SENSOR_LEVEL, .mono_ns = T};
        if (dist == NOECHO)
            r.flags = REC_F_NO_ECHO;
        else
            r.value = dist;
        eng_on_sample(&E, &r, &o);
        safe(&o);
        apply(&o);
    }
    eng_tick(&E, T, &o);
    safe(&o);
    apply(&o);
}

static void steps(int n, int32_t soil, int32_t dist)
{
    for (int i = 0; i < n; i++)
        step(soil, dist);
}

/* Healthy and dry until the engine asks for water. */
static void until_requested(void)
{
    for (int i = 0; i < 200 && starts == 0; i++)
        step(DRY, FULL);
    CHECK(starts == 1);
}

static uint64_t on_s(void) { return P.longest_on_ns / SEC; }

/* ================================================================ normal sequence */

static void test_normal_sequence(void)
{
    start();
    P.confirm_after_s = 0;
    until_requested();
    CHECK(eng_phase(&E) == ENG_WATERING); /* confirmed in the same second */
    steps(30, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_VERIFYING);
    CHECK(on_s() == 20);                  /* exactly one dose */
    CHECK(E.ran_ms == 20000 && E.stop_why == ENG_STOP_DOSE_DONE);
    CHECK(E.unexpected_reports == 0);
}

static void test_request_is_not_running(void)
{
    start();
    P.confirm_after_s = 2;
    until_requested();
    CHECK(eng_phase(&E) == ENG_START_REQUESTED && !P.on); /* asked, not yet running */
    steps(2, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_WATERING && P.on);         /* confirmed within the 3 s window */
    steps(30, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_VERIFYING);
}

/* ================================================================ start never confirmed */

static void test_start_never_confirmed(void)
{
    start();
    P.confirm_after_s = -1;
    until_requested();
    steps(3, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_START_REQUESTED);
    step(DRY, FULL); /* > 3 s */
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
    steps(100, DRY, FULL); /* safe() checks STOP on every call */
    CHECK(on_s() == 0 && starts == 1);
}

static void test_start_confirmed_too_late(void)
{
    start();
    P.confirm_after_s = 6; /* after the engine already gave up */
    until_requested();
    steps(10, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
    CHECK(E.unexpected_reports >= 1); /* the late "started" was not accepted */
    CHECK(!P.on && on_s() <= 1);      /* and was stopped at once */
}

static void test_start_refused(void)
{
    start();
    P.refuse = true;
    until_requested();
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
}

static void test_sensor_fails_before_start_is_confirmed(void)
{
    struct eng_params p = test_params();
    p.start_confirm_ms = 15000; /* slow pump control, so the sensor can fail in the window */
    start_with(&p);
    P.confirm_after_s = 12;
    until_requested();
    steps(8, DRY, NONE); /* level unplugged: stale after 5 s -> cancel */
    CHECK(eng_phase(&E) != ENG_LOCKOUT);
    CHECK(eng_phase(&E) == ENG_IDLE || eng_phase(&E) == ENG_DRY_DWELL);
    CHECK(on_s() == 0 && !P.on);
    steps(100, DRY, FULL);
    CHECK(starts == 1); /* the cancelled start still counts: next one waits for the interval */
}

/* ================================================================ stopping */

static void check_stops_mid_dose(int32_t bad_soil, int32_t bad_dist, uint64_t max_on_s)
{
    start();
    P.confirm_after_s = 0;
    until_requested();
    steps(5, DRY, FULL);
    steps(15, bad_soil, bad_dist);
    CHECK(eng_phase(&E) == ENG_VERIFYING); /* stopped early, cleanly, not locked out */
    CHECK(E.stop_why == ENG_STOP_REQUESTED);
    CHECK(on_s() < 20 && on_s() <= max_on_s);
}

static void test_stops_when_level_unplugged(void) { check_stops_mid_dose(DRY, NONE, 5 + 6); }
static void test_stops_when_soil_unplugged(void) { check_stops_mid_dose(NONE, FULL, 5 + 6); }
static void test_stops_on_missing_echoes(void) { check_stops_mid_dose(DRY, NOECHO, 5 + 3); }
static void test_stops_when_reservoir_runs_low(void) { check_stops_mid_dose(DRY, LOW, 5 + 5); }
static void test_stops_when_soil_pins_at_rail(void) { check_stops_mid_dose(1023, FULL, 5 + 5); }

static void test_pump_overruns_the_dose(void)
{
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true; /* pump control forgets to end the dose, but obeys STOP */
    until_requested();
    steps(40, DRY, FULL);
    CHECK(on_s() <= 20 + 3 + 1);       /* the engine asked for STOP at dose + grace */
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
}

static void test_pump_ignores_stop(void)
{
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true;
    P.ignore_stop = true; /* stuck relay */
    until_requested();
    steps(30, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
    /* The engine cannot switch the pump off itself; it keeps asking (safe() checks STOP on
     * every call) while pump control's own cap and the supervisor are the hard stop. */
    steps(700, DRY, FULL);
    CHECK(starts == 1);
}

static void test_pump_reports_its_cap(void)
{
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true;
    P.ignore_stop = true;
    until_requested();
    report_stopped(600000, ENG_STOP_CAP); /* pump control's own 10-minute cut fired */
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_PUMP_TIMEOUT);

    /* Its cap can fire short of 600 s by our count (its clock, or a lowered test cap):
     * the reason alone must be enough. */
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true;
    P.ignore_stop = true;
    until_requested();
    report_stopped(15000, ENG_STOP_CAP);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_PUMP_TIMEOUT);
}

static void test_report_of_running_too_long(void)
{
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true;
    P.ignore_stop = true;
    until_requested();
    report_stopped(CFG_PUMP_CAP_MS, ENG_STOP_DOSE_DONE); /* "done", but it ran 10 minutes */
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_PUMP_TIMEOUT);
}

static void test_engine_cap_backstop(void)
{
    /* White-box: valid params make the engine give up long before the cap, so pretend the pump
     * has been running for 10 minutes and check the backstop still fires. */
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true;
    P.ignore_stop = true;
    until_requested();
    E.stop_requested = false;
    T += (uint64_t)CFG_PUMP_CAP_MS * 1000000u; /* 10 minutes pass with no engine call at all */
    step(DRY, FULL);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_PUMP_TIMEOUT);
}

static void test_stop_failure_reported(void)
{
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true;
    P.ignore_stop = true;
    until_requested();
    report_stopped(5000, ENG_STOP_ACT_FAIL);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
}

static void test_stop_claims_a_request_we_never_made(void)
{
    start();
    P.confirm_after_s = 0;
    P.stuck_on = true;
    P.ignore_stop = true;
    until_requested();
    report_stopped(5000, ENG_STOP_REQUESTED);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
}

/* ================================================================ unexpected pump activity */

static void test_second_started_while_watering(void)
{
    start();
    P.confirm_after_s = 0;
    until_requested();
    report_started();
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_UNEXPECTED_PUMP);
    CHECK(!P.on); /* STOP obeyed */
}

static void test_started_while_verifying(void)
{
    start();
    P.confirm_after_s = 0;
    until_requested();
    steps(25, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_VERIFYING);
    report_started();
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_UNEXPECTED_PUMP);
}

static void test_ran_while_verifying(void)
{
    start();
    P.confirm_after_s = 0;
    until_requested();
    steps(25, DRY, FULL);
    report_stopped(3000, ENG_STOP_DOSE_DONE);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_UNEXPECTED_PUMP);
}

static void test_pump_switches_on_by_itself(void)
{
    start();
    steps(20, 300, FULL); /* wet soil, idle */
    P.on = true;          /* relay welded / someone pressed it */
    P.on_since = T;
    report_started();
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_UNEXPECTED_PUMP);
    CHECK(!P.on && starts == 0);
    steps(50, DRY, FULL);
    CHECK(starts == 0); /* stays locked out, keeps asking for STOP */
}

int main(void)
{
    RUN(test_normal_sequence);
    RUN(test_request_is_not_running);
    RUN(test_start_never_confirmed);
    RUN(test_start_confirmed_too_late);
    RUN(test_start_refused);
    RUN(test_sensor_fails_before_start_is_confirmed);
    RUN(test_stops_when_level_unplugged);
    RUN(test_stops_when_soil_unplugged);
    RUN(test_stops_on_missing_echoes);
    RUN(test_stops_when_reservoir_runs_low);
    RUN(test_stops_when_soil_pins_at_rail);
    RUN(test_pump_overruns_the_dose);
    RUN(test_pump_ignores_stop);
    RUN(test_pump_reports_its_cap);
    RUN(test_report_of_running_too_long);
    RUN(test_engine_cap_backstop);
    RUN(test_stop_failure_reported);
    RUN(test_stop_claims_a_request_we_never_made);
    RUN(test_second_started_while_watering);
    RUN(test_started_while_verifying);
    RUN(test_ran_while_verifying);
    RUN(test_pump_switches_on_by_itself);
    return unit_done("test_pump");
}
