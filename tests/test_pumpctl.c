/* Pump control with the SIMULATED backend: the 10-minute cap, turning off when the hub goes
 * silent, OFF at start and shutdown, and switch failures. No hardware involved. */
#include <string.h>

#include "actuator/pump.h"
#include "actuator/pump_sim.h"
#include "common/config.h"
#include "unit.h"

#define SEC 1000000000ull

static struct pump PC;
static struct pump_sim SIM;
static struct pump_report R;
static uint64_t T;

/* After EVERY call: if the simulated relay is on, pump control knows it, the run is under the
 * cap, and the hub was heard within the timeout. */
static void safe(void)
{
    if (SIM.relay) {
        CHECK(pump_relay_maybe_on(&PC));
        CHECK(pump_state(&PC) == PUMP_ON || pump_state(&PC) == PUMP_FAULT);
        if (pump_state(&PC) == PUMP_ON) {
            CHECK(T >= PC.on_since_ns && T - PC.on_since_ns < (uint64_t)PC.p.cap_ms * 1000000u);
            CHECK(T - PC.last_heard_ns <= (uint64_t)PC.p.hub_timeout_ms * 1000000u);
        }
    }
    if (pump_state(&PC) == PUMP_ON)
        CHECK(SIM.relay);
}

static void setup(void)
{
    memset(&SIM, 0, sizeof SIM);
    struct pump_params p;
    pump_params_default(&p);
    struct pump_backend b = pump_sim_backend(&SIM);
    T = 0;
    CHECK(pump_init(&PC, &p, &b, T) == 0);
    safe();
}

static void tick(void) { pump_tick(&PC, T, &R); safe(); }
static void alive(void) { pump_hub_alive(&PC, T, &R); safe(); }
static void start(uint32_t dose_ms) { pump_start(&PC, T, dose_ms, &R); safe(); }
static void stop(void) { pump_stop(&PC, T, &R); safe(); }

/* One second: the hub says it's alive (if `hub`), then pump control's own tick. Returns the
 * first STOPPED report seen, if any, in `out`. */
static bool second(bool hub, struct pump_report *out)
{
    T += SEC;
    if (hub) {
        alive();
        if (R.kind == PUMP_REP_STOPPED) {
            *out = R;
            return true;
        }
    }
    tick();
    if (R.kind == PUMP_REP_STOPPED) {
        *out = R;
        return true;
    }
    return false;
}

/* n seconds with the hub talking every second; no run may end meanwhile. */
static void talk(int n)
{
    struct pump_report r;
    for (int i = 0; i < n; i++)
        CHECK(!second(true, &r));
}

static int run_until_stopped(int limit_s, bool hub, struct pump_report *out)
{
    for (int i = 1; i <= limit_s; i++)
        if (second(hub, out))
            return i;
    return -1;
}

/* ================================================================ OFF at start and shutdown */

static void test_init_switches_off_first(void)
{
    memset(&SIM, 0, sizeof SIM);
    SIM.relay = true; /* left on by a process that crashed */
    struct pump_params p;
    pump_params_default(&p);
    struct pump_backend b = pump_sim_backend(&SIM);
    CHECK(pump_init(&PC, &p, &b, 0) == 0);
    CHECK(!SIM.relay && SIM.commands == 1 && SIM.offs == 1); /* the very first command is OFF */
    CHECK(pump_state(&PC) == PUMP_OFF && !pump_relay_maybe_on(&PC));
}

static void test_init_off_failure_is_fault(void)
{
    memset(&SIM, 0, sizeof SIM);
    SIM.relay = true;
    SIM.fail_next_off = 1;
    struct pump_params p;
    pump_params_default(&p);
    struct pump_backend b = pump_sim_backend(&SIM);
    CHECK(pump_init(&PC, &p, &b, 0) == -1);
    CHECK(pump_state(&PC) == PUMP_FAULT && pump_relay_maybe_on(&PC));
    T = 0;
    tick(); /* retries OFF, succeeds */
    CHECK(!SIM.relay && !pump_relay_maybe_on(&PC) && pump_state(&PC) == PUMP_FAULT);
    start(20000);
    CHECK(R.kind == PUMP_REP_STOPPED && R.why == PUMP_END_REFUSED && !SIM.relay); /* latched */
}

static void test_shutdown(void)
{
    setup();
    start(20000);
    talk(5);
    pump_shutdown(&PC, T, &R);
    safe();
    CHECK(!SIM.relay && R.kind == PUMP_REP_STOPPED && R.why == PUMP_END_SHUTDOWN && R.ran_ms == 5000);
    pump_shutdown(&PC, T, &R); /* twice is fine */
    CHECK(!SIM.relay && R.kind == PUMP_REP_NONE);
    start(20000);
    CHECK(R.why == PUMP_END_REFUSED && !SIM.relay); /* no starts on the way out */

    setup();
    uint64_t offs = SIM.offs;
    pump_shutdown(&PC, T, &R); /* while OFF: still sends OFF once more */
    CHECK(!SIM.relay && SIM.offs == offs + 1);
}

/* ================================================================ normal runs */

static void test_runs_its_dose(void)
{
    setup();
    start(20000);
    CHECK(R.kind == PUMP_REP_STARTED && SIM.relay && pump_state(&PC) == PUMP_ON);
    struct pump_report done;
    CHECK(run_until_stopped(60, true, &done) == 20);
    CHECK(done.why == PUMP_END_DOSE_DONE && done.ran_ms == 20000 && !SIM.relay);
}

static void test_stop(void)
{
    setup();
    start(20000);
    talk(7);
    stop();
    CHECK(R.kind == PUMP_REP_STOPPED && R.why == PUMP_END_STOPPED && R.ran_ms == 7000 && !SIM.relay);
    stop(); /* while OFF: nothing */
    CHECK(R.kind == PUMP_REP_NONE);
}

static void test_start_while_on_does_not_extend(void)
{
    setup();
    start(20000);
    talk(15);
    start(20000); /* ignored */
    CHECK(R.kind == PUMP_REP_NONE && PC.ignored_starts == 1);
    struct pump_report done;
    CHECK(run_until_stopped(30, true, &done) == 5); /* still ends 20 s after the FIRST start */
}

/* ================================================================ the 10-minute cap */

static void test_dose_at_or_over_cap_refused(void)
{
    setup();
    start(CFG_PUMP_CAP_MS);
    CHECK(R.kind == PUMP_REP_STOPPED && R.why == PUMP_END_REFUSED && !SIM.relay && SIM.ons == 0);
    start(0);
    CHECK(R.why == PUMP_END_REFUSED && SIM.ons == 0);
}

static void test_cap_holds_even_if_the_dose_timer_breaks(void)
{
    /* White-box: corrupt the dose after the start, so only the cap can end the run. */
    setup();
    start(20000);
    PC.dose_ms = UINT32_MAX;
    struct pump_report done;
    CHECK(run_until_stopped(700, true, &done) == 600);
    CHECK(done.why == PUMP_END_CAP && done.ran_ms == 600000 && !SIM.relay);
}

static void test_time_backwards_counts_as_too_long(void)
{
    setup();
    T = 100 * SEC;
    start(20000);
    T = 50 * SEC; /* clock bug */
    tick();
    CHECK(R.kind == PUMP_REP_STOPPED && R.why == PUMP_END_CAP && !SIM.relay);
}

/* ================================================================ hub silence */

static void test_off_when_hub_goes_silent(void)
{
    setup();
    start(20000);
    struct pump_report done;
    int s = run_until_stopped(60, false, &done); /* the hub never speaks again */
    CHECK(s == 5 && done.why == PUMP_END_HUB_SILENT && done.ran_ms == 5000 && !SIM.relay);
}

static void test_late_alive_does_not_rescue_a_run(void)
{
    setup();
    start(20000);
    for (int i = 0; i < 4; i++) {
        T += SEC;
        tick();
    }
    T += 2 * SEC; /* 6 s of silence, then the hub "wakes up" */
    alive();
    CHECK(R.kind == PUMP_REP_STOPPED && R.why == PUMP_END_HUB_SILENT && !SIM.relay);
}

static void test_silence_while_off_is_harmless(void)
{
    setup();
    for (int i = 0; i < 60; i++) {
        T += SEC;
        tick();
        CHECK(R.kind == PUMP_REP_NONE);
    }
    start(20000); /* a START is itself word from the hub */
    CHECK(R.kind == PUMP_REP_STARTED && SIM.relay);
}

/* ================================================================ switch failures */

static void test_on_failure(void)
{
    setup();
    SIM.fail_next_on = 1;
    start(20000);
    CHECK(R.kind == PUMP_REP_STOPPED && R.why == PUMP_END_SWITCH_FAILED && R.ran_ms == 0);
    CHECK(!SIM.relay && pump_state(&PC) == PUMP_OFF && !pump_relay_maybe_on(&PC));
    start(20000); /* a failed ON is not latched */
    CHECK(R.kind == PUMP_REP_STARTED && SIM.relay);
}

static void test_stuck_relay(void)
{
    setup();
    start(20000);
    SIM.fail_next_off = 3; /* the OFF at the dose end and two retries fail */
    struct pump_report done;
    CHECK(run_until_stopped(30, true, &done) == 20);
    CHECK(done.why == PUMP_END_SWITCH_FAILED && pump_state(&PC) == PUMP_FAULT && SIM.relay);
    T += SEC;
    tick();
    CHECK(SIM.relay);  /* retry 1 failed */
    T += SEC;
    tick();
    CHECK(SIM.relay);  /* retry 2 failed */
    T += SEC;
    tick();
    CHECK(!SIM.relay && !pump_relay_maybe_on(&PC)); /* retry 3 worked */
    CHECK(pump_state(&PC) == PUMP_FAULT);
    start(20000);
    CHECK(R.why == PUMP_END_REFUSED && !SIM.relay); /* latched until a restart */
}

static void test_bad_params(void)
{
    struct pump_params p;
    pump_params_default(&p);
    CHECK(pump_params_check(&p) == NULL && p.cap_ms == CFG_PUMP_CAP_MS);
    p.cap_ms = CFG_PUMP_CAP_MS + 1;
    CHECK(pump_params_check(&p) != NULL);
    pump_params_default(&p);
    p.hub_timeout_ms = 0;
    CHECK(pump_params_check(&p) != NULL);
    memset(&SIM, 0, sizeof SIM);
    struct pump_backend b = pump_sim_backend(&SIM);
    p.cap_ms = 0;
    CHECK(pump_init(&PC, &p, &b, 0) == -1 && !SIM.relay); /* still switched OFF */
    T = 0;
    start(1000);
    CHECK(R.why == PUMP_END_REFUSED && !SIM.relay);
}

/* ================================================================ random abuse */

static void test_random_calls_keep_the_limits(void)
{
    /* Random starts, stops, silences, time jumps and switch failures. safe() runs after every
     * call: the relay is never on past the cap or past the hub timeout. */
    setup();
    uint32_t x = 4242;
    uint64_t on_ns = 0;
    for (int i = 0; i < 200000; i++) {
        x = x * 1103515245u + 12345u;
        T += (x >> 8) % (3 * SEC);
        unsigned r = (x >> 4) % 100;
        if (r < 2)
            SIM.fail_next_off = 1 + (int)((x >> 12) % 3);
        else if (r < 4)
            SIM.fail_next_on = 1;
        if (r < 15)
            start((uint32_t)((x >> 10) % 700000));
        else if (r < 20)
            stop();
        else if (r < 60)
            alive();
        else
            tick();
        if (SIM.relay)
            on_ns += 1;
        if (r == 99) { /* process restart (e.g. by the supervisor after a FAULT) */
            struct pump_params p;
            pump_params_default(&p);
            struct pump_backend b = pump_sim_backend(&SIM);
            pump_init(&PC, &p, &b, T);
            safe();
        }
    }
    CHECK(PC.runs + SIM.ons > 100 && on_ns > 0); /* the pump really ran, often */
}

int main(void)
{
    RUN(test_init_switches_off_first);
    RUN(test_init_off_failure_is_fault);
    RUN(test_shutdown);
    RUN(test_runs_its_dose);
    RUN(test_stop);
    RUN(test_start_while_on_does_not_extend);
    RUN(test_dose_at_or_over_cap_refused);
    RUN(test_cap_holds_even_if_the_dose_timer_breaks);
    RUN(test_time_backwards_counts_as_too_long);
    RUN(test_off_when_hub_goes_silent);
    RUN(test_late_alive_does_not_rescue_a_run);
    RUN(test_silence_while_off_is_harmless);
    RUN(test_on_failure);
    RUN(test_stuck_relay);
    RUN(test_bad_params);
    RUN(test_random_calls_keep_the_limits);
    return unit_done("test_pumpctl");
}
