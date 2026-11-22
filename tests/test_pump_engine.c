/* The watering engine and pump control wired together the way the hub would wire them, with
 * the SIMULATED pump backend and a tiny simulated plant: while the simulated relay is on, the
 * tank level drops 1 mm/s and the soil gets wet 10 s later. Everything here is synthetic.
 * The glue below is a stand-in for hub code that does not exist yet (and is Jim's to review). */
#include <string.h>

#include "actuator/pump.h"
#include "actuator/pump_sim.h"
#include "analysis/engine.h"
#include "unit.h"

#define SEC 1000000000ull
#define DRY 20

static struct eng E;
static struct pump PC;
static struct pump_sim SIM;
static uint64_t T;
static bool hub_up;
static int starts_requested;

/* the plant */
static int32_t water_mm;      /* above the inlet; distance = 200 - water */
static int relay_on_s;        /* seconds the relay has been on in the current watering */
static int wet_for_s;         /* seconds of wet soil left */

/* the log the hub would keep */
static struct { uint64_t t; enum eng_event_kind k; int32_t v; } LOG[8192];
static int nlog;

static struct eng_params eng_test_params(void)
{
    struct eng_params p;
    eng_params_default(&p);
    p.soil_dry_enter = 40;
    p.soil_dry_exit = 80;
    p.dry_dwell_ms = 60000;
    p.warmup_ms = 10000;
    p.level_empty_dist_mm = 200;
    p.level_full_dist_mm = 40;
    p.level_reserve_mm = 20;
    p.level_hyst_mm = 10;
    p.dose_ms = 20000;
    p.verify_window_ms = 120000;
    return p;
}

/* pump control's "why" -> the engine's: everything abnormal is an actuator failure. */
static enum eng_stop to_eng(enum pump_end why)
{
    switch (why) {
    case PUMP_END_DOSE_DONE: return ENG_STOP_DOSE_DONE;
    case PUMP_END_STOPPED: return ENG_STOP_REQUESTED;
    case PUMP_END_CAP: return ENG_STOP_CAP;
    default: return ENG_STOP_ACT_FAIL;
    }
}

static void from_pump(const struct pump_report *r);

static void from_engine(const struct eng_out *o)
{
    CHECK(eng_check_output(&E, o) == NULL && o->dropped_info == 0 && o->dropped_critical == 0);
    for (unsigned i = 0; i < o->n_events; i++) /* saved first, then the request goes on */
        if (nlog < 8192)
            LOG[nlog].t = T, LOG[nlog].k = o->ev[i].kind, LOG[nlog].v = o->ev[i].value, nlog++;
    struct pump_report r;
    if (o->request == ENG_REQ_START) {
        starts_requested++;
        pump_start(&PC, T, o->dose_ms, &r);
        from_pump(&r);
    } else if (o->request == ENG_REQ_STOP) {
        pump_stop(&PC, T, &r);
        from_pump(&r);
    }
}

static void from_pump(const struct pump_report *r)
{
    if (!hub_up)
        return; /* nobody is listening */
    struct eng_out o;
    if (r->kind == PUMP_REP_STARTED) {
        eng_on_watering_started(&E, T, &o);
        from_engine(&o);
    } else if (r->kind == PUMP_REP_STOPPED) {
        eng_on_watering_stopped(&E, T, r->ran_ms, to_eng(r->why), &o);
        from_engine(&o);
    }
}

static void hub_start(uint64_t t0, bool replay)
{
    struct eng_params p = eng_test_params();
    CHECK(eng_init(&E, &p, t0) == 0);
    for (int i = 0; replay && i < nlog; i++)
        eng_restore_event(&E, LOG[i].k, LOG[i].v, LOG[i].t);
    hub_up = true;
    struct eng_out o;
    eng_restore_done(&E, &o);
    from_engine(&o);
}

static void boot(void)
{
    memset(&SIM, 0, sizeof SIM);
    struct pump_params pp;
    pump_params_default(&pp);
    struct pump_backend b = pump_sim_backend(&SIM);
    T = 0;
    CHECK(pump_init(&PC, &pp, &b, T) == 0);
    nlog = 0;
    starts_requested = 0;
    water_mm = 100;
    relay_on_s = 0;
    wet_for_s = 0;
    hub_start(0, false);
}

static void second(void)
{
    T += SEC;
    /* the plant */
    if (SIM.relay) {
        water_mm--;
        relay_on_s++;
        if (relay_on_s >= 10)
            wet_for_s = 1800; /* wet for half an hour, then dry again */
    } else if (wet_for_s > 0) {
        wet_for_s--;
        if (wet_for_s == 0)
            relay_on_s = 0;
    }
    struct pump_report r;
    struct eng_out o;
    if (hub_up) {
        struct record s = {.sensor_id = SENSOR_SOIL, .mono_ns = T, .value = wet_for_s > 0 ? 150 : DRY};
        eng_on_sample(&E, &s, &o);
        from_engine(&o);
        struct record l = {.sensor_id = SENSOR_LEVEL, .mono_ns = T, .value = 200 - water_mm};
        eng_on_sample(&E, &l, &o);
        from_engine(&o);
        eng_tick(&E, T, &o);
        from_engine(&o);
        pump_hub_alive(&PC, T, &r);
        from_pump(&r);
    }
    pump_tick(&PC, T, &r); /* pump control's own process keeps ticking */
    from_pump(&r);

    /* With the hub up and handled, the relay is only ever on while the engine is watering, or
     * when it is welded on (pump control FAULT), and then the engine must be locked out. */
    if (hub_up && SIM.relay)
        CHECK(eng_phase(&E) == ENG_WATERING ||
              (pump_state(&PC) == PUMP_FAULT && eng_phase(&E) == ENG_LOCKOUT));
}

static void seconds(int n)
{
    for (int i = 0; i < n; i++)
        second();
}

static void until_phase(enum eng_phase ph, int limit)
{
    for (int i = 0; i < limit && eng_phase(&E) != ph; i++)
        second();
    CHECK(eng_phase(&E) == ph);
}

/* ================================================================ */

static void test_normal_watering_end_to_end(void)
{
    boot();
    until_phase(ENG_WATERING, 200);
    CHECK(SIM.relay);
    until_phase(ENG_VERIFYING, 60);
    CHECK(!SIM.relay && water_mm == 80); /* the dose ran exactly 20 s */
    until_phase(ENG_IDLE, 200);
    CHECK(eng_last_diag(&E) == ENG_DIAG_OK && starts_requested == 1);
}

static void test_hub_hangs_mid_dose(void)
{
    boot();
    until_phase(ENG_WATERING, 200);
    seconds(3);
    hub_up = false; /* the hub hangs: no samples, no ticks, no "alive" */
    int off_after = -1;
    for (int i = 1; i <= 60; i++) {
        second();
        if (!SIM.relay && off_after < 0)
            off_after = i;
    }
    CHECK(off_after > 0 && off_after <= 5); /* pump control switched off by itself */
    CHECK(water_mm >= 100 - 3 - 5);

    hub_start(T, true); /* the supervisor restarts the hub; it replays the log */
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_UNFINISHED_WATERING);
    int before = starts_requested;
    seconds(2 * 3600);
    CHECK(starts_requested == before && !SIM.relay);
}

static void test_pump_control_restarts_mid_dose(void)
{
    boot();
    until_phase(ENG_WATERING, 200);
    seconds(5);
    struct pump_params pp;
    pump_params_default(&pp);
    struct pump_backend b = pump_sim_backend(&SIM);
    pump_init(&PC, &pp, &b, T); /* pump control crashed and was restarted: OFF first */
    CHECK(!SIM.relay);
    seconds(60);
    /* It never reported the end of the run, so the engine gives up and locks out. */
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR && !SIM.relay);
}

static void test_stuck_relay(void)
{
    boot();
    until_phase(ENG_WATERING, 200);
    SIM.fail_next_off = 10; /* welded relay for 10 OFF attempts */
    seconds(30);
    CHECK(eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == ENG_LOCK_ACTUATOR);
    CHECK(pump_state(&PC) == PUMP_FAULT);
    seconds(30); /* both keep trying OFF; the relay lets go when the fault clears */
    CHECK(!SIM.relay);
    int before = starts_requested;
    seconds(2 * 3600);
    CHECK(starts_requested == before && !SIM.relay);
}

static void test_shutdown_while_watering(void)
{
    boot();
    until_phase(ENG_WATERING, 200);
    struct pump_report r;
    pump_shutdown(&PC, T, &r); /* orderly stop of the whole system */
    from_pump(&r);
    CHECK(!SIM.relay && eng_phase(&E) == ENG_LOCKOUT); /* the engine treats it as an actuator end */
}

int main(void)
{
    RUN(test_normal_watering_end_to_end);
    RUN(test_hub_hangs_mid_dose);
    RUN(test_pump_control_restarts_mid_dose);
    RUN(test_stuck_relay);
    RUN(test_shutdown_while_watering);
    return unit_done("test_pump_engine");
}
