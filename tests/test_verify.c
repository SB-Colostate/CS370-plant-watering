/* Watering engine, commit 6: did the water arrive? Both sensors, before vs. after.
 * Synthetic fixtures (one soil + one level sample per second); each scenario gives the soil and
 * the distance a shape over time, counted from the second the pump starts.
 *
 * Test params: dose 20 s, verification window 120 s after the stop. Water height noise +-5 mm,
 * a drop needs >= 10 mm. Soil noise +-10, a rise needs >= 30. Distance 100 mm = 100 mm of water
 * (empty_dist 200); a LARGER distance means LESS water, so 100 -> 120 is a 20 mm drop. */
#include <limits.h>
#include <string.h>

#include "analysis/engine.h"
#include "unit.h"

#define SEC 1000000000ull
#define NONE INT32_MIN
#define NOECHO (-1)
#define DRY 20
#define FULL 100

/* ---------------------------------------------------------------- fake pump control */

static struct eng E;
static uint64_t T;
static bool pump_on;
static uint64_t pump_on_since;
static uint32_t pump_dose_ms;
static uint32_t short_dose_ms; /* if set: says "dose done" after only this long */
static int diag_events, lockout_events;
static int32_t diag_value;
static bool diag_durable;

static void apply(const struct eng_out *o);

static void safe(const struct eng_out *o)
{
    const char *why = eng_check_output(&E, o);
    CHECK(why == NULL);
    if (why)
        fprintf(stderr, "    unsafe output: %s\n", why);
    CHECK(o->dropped_info == 0 && E.internal_faults == 0);
    enum eng_phase ph = eng_phase(&E);
    if (ph == ENG_IDLE || ph == ENG_DRY_DWELL || ph == ENG_VERIFYING)
        CHECK(!pump_on);
    for (unsigned i = 0; i < o->n_events; i++) {
        if (o->ev[i].kind == ENG_EV_DIAG) {
            diag_events++;
            diag_value = o->ev[i].value;
            diag_durable = o->ev[i].durable;
        }
        if (o->ev[i].kind == ENG_EV_LOCKOUT)
            lockout_events++;
    }
}

static void apply(const struct eng_out *o)
{
    struct eng_out r;
    if (o->request == ENG_REQ_START) {
        pump_on = true; /* fast pump control: on and confirmed at once */
        pump_on_since = T;
        pump_dose_ms = short_dose_ms ? short_dose_ms : o->dose_ms;
        eng_on_watering_started(&E, T, &r);
        safe(&r);
        apply(&r);
    } else if (o->request == ENG_REQ_STOP && pump_on) {
        pump_on = false;
        eng_on_watering_stopped(&E, T, (uint32_t)((T - pump_on_since) / 1000000u), ENG_STOP_REQUESTED, &r);
        safe(&r);
        apply(&r);
    }
}

static void pump_tick(void)
{
    if (pump_on && T - pump_on_since >= (uint64_t)pump_dose_ms * 1000000u) {
        struct eng_out r;
        pump_on = false;
        eng_on_watering_stopped(&E, T, pump_dose_ms, ENG_STOP_DOSE_DONE, &r);
        safe(&r);
        apply(&r);
    }
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
    p.verify_window_ms = 120000;
    p.level_noise_mm = 5;
    p.level_drop_min_mm = 10;
    p.soil_noise = 10;
    p.soil_rise_min = 30;
    return p;
}

static void feed(int32_t soil, int32_t dist)
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

/* What the sensors read t seconds after the pump started. */
typedef void (*shape_fn)(int t, int32_t *soil, int32_t *dist);

/* Dry and healthy until the start, then the shape, until a diagnosis (or a time limit).
 * Returns the diagnosis. */
static enum eng_diag run(shape_fn shape)
{
    struct eng_params p = test_params();
    CHECK(eng_init(&E, &p, 0) == 0);
    T = 0;
    pump_on = false;
    short_dose_ms = 0;
    diag_events = lockout_events = 0;
    int t = -1;
    for (int i = 0; i < 1000 && diag_events == 0; i++) {
        if (t < 0 && eng_phase(&E) == ENG_WATERING)
            t = 0;
        int32_t soil = DRY, dist = FULL;
        if (t >= 0)
            shape(t++, &soil, &dist);
        feed(soil, dist);
    }
    CHECK(t >= 0);             /* watering happened */
    CHECK(diag_events == 1);   /* exactly one diagnosis */
    CHECK(diag_durable);       /* saved, so it survives a power cut */
    CHECK(diag_value == (int32_t)eng_last_diag(&E));
    return eng_last_diag(&E);
}

/* Reservoir: distance rises 100 -> 100 + drop over the 20 s dose. */
static int32_t dist_drop(int t, int drop) { return FULL + (t < 20 ? drop * t / 20 : drop); }
/* Soil: starts responding 10 s after the pump starts. */
static int32_t soil_to(int t, int32_t after) { return t < 10 ? DRY : after; }

static void ok_shape(int t, int32_t *s, int32_t *d) { *s = soil_to(t, 150); *d = dist_drop(t, 20); }
static void noflow_shape(int t, int32_t *s, int32_t *d) { (void)t; *s = DRY; *d = FULL; }
static void leak_shape(int t, int32_t *s, int32_t *d) { *s = DRY; *d = dist_drop(t, 20); }
static void notrack_shape(int t, int32_t *s, int32_t *d) { *s = soil_to(t, 150); *d = FULL; }

static bool locked(enum eng_lock why) { return eng_phase(&E) == ENG_LOCKOUT && eng_lock_reason(&E) == why; }

/* ================================================================ the four clear outcomes */

static void test_normal_watering_is_ok(void)
{
    CHECK(run(ok_shape) == ENG_DIAG_OK);
    CHECK(eng_phase(&E) == ENG_IDLE && lockout_events == 0);
    CHECK(E.verify_level_drop_mm >= 19 && E.verify_soil_rise >= 100); /* before vs. after */
}

static void test_no_flow(void)
{
    CHECK(run(noflow_shape) == ENG_DIAG_PUMP_NO_FLOW);
    CHECK(locked(ENG_LOCK_PUMP_NO_FLOW) && lockout_events == 1);
}

static void test_leak_or_probe_out(void)
{
    CHECK(run(leak_shape) == ENG_DIAG_LEAK_OR_PROBE_OUT);
    CHECK(locked(ENG_LOCK_LEAK_OR_PROBE_OUT));
}

static void test_level_not_tracking(void)
{
    CHECK(run(notrack_shape) == ENG_DIAG_LEVEL_NOT_TRACKING);
    CHECK(locked(ENG_LOCK_LEVEL_NOT_TRACKING));
}

/* ================================================================ readings that don't agree */

static void unclear_level(int t, int32_t *s, int32_t *d) { *s = soil_to(t, 150); *d = dist_drop(t, 7); }
static void unclear_soil(int t, int32_t *s, int32_t *d) { *s = soil_to(t, DRY + 20); *d = dist_drop(t, 20); }
static void level_rose(int t, int32_t *s, int32_t *d) { *s = soil_to(t, 150); *d = dist_drop(t, -20); }
static void soil_drier(int t, int32_t *s, int32_t *d) { *s = soil_to(t, 0); *d = dist_drop(t, 20); }

static void check_inconclusive(shape_fn f)
{
    enum eng_diag d = run(f);
    CHECK(d == ENG_DIAG_INCONCLUSIVE);
    /* Not a fault and not a success: no lockout. (The phase may already be DRY_DWELL again if
     * the soil still reads dry; the rate limit holds the next start back.) */
    CHECK(eng_phase(&E) != ENG_LOCKOUT && lockout_events == 0);
}

static void test_small_level_drop_is_unclear(void) { check_inconclusive(unclear_level); }
static void test_small_soil_rise_is_unclear(void) { check_inconclusive(unclear_soil); }
static void test_level_rose_is_unclear(void) { check_inconclusive(level_rose); }
/* The soil "after" is the wettest smoothed value since the request, so a fall can only show as
 * "no rise". With the tank dropping, soil falling to 0 is what a pulled-out probe looks like:
 * a lockout, which is the safe answer. */
static void test_soil_got_drier_while_tank_dropped(void)
{
    CHECK(run(soil_drier) == ENG_DIAG_LEAK_OR_PROBE_OUT);
    CHECK(locked(ENG_LOCK_LEAK_OR_PROBE_OUT));
}

/* ================================================================ lost sensors */

static void soil_lost(int t, int32_t *s, int32_t *d) { ok_shape(t, s, d); if (t >= 40 && t < 50) *s = NONE; }
static void level_lost(int t, int32_t *s, int32_t *d) { ok_shape(t, s, d); if (t >= 40 && t < 50) *d = NONE; }
static void echo_lost(int t, int32_t *s, int32_t *d) { ok_shape(t, s, d); if (t >= 40 && t < 44) *d = NOECHO; }

/* Each of these LOOKS like a perfect watering once the sensor is back. It must not be OK. */
static void test_soil_lost_during_verification(void) { check_inconclusive(soil_lost); }
static void test_level_lost_during_verification(void) { check_inconclusive(level_lost); }
static void test_echoes_lost_during_verification(void) { check_inconclusive(echo_lost); }

/* ================================================================ interrupted watering */

static void level_lost_mid_dose(int t, int32_t *s, int32_t *d) { ok_shape(t, s, d); if (t >= 5 && t < 12) *d = NONE; }
static void low_mid_dose(int t, int32_t *s, int32_t *d) { *s = soil_to(t, 150); *d = t < 5 ? FULL : 195; }

static void test_interrupted_by_sensor_loss(void)
{
    check_inconclusive(level_lost_mid_dose);
    CHECK(E.stop_why == ENG_STOP_REQUESTED && E.ran_ms < 20000);
}

static void test_interrupted_by_low_reservoir(void)
{
    check_inconclusive(low_mid_dose);
    CHECK(E.stop_why == ENG_STOP_REQUESTED && E.ran_ms < 20000);
}

static void test_short_dose_reported_as_done(void)
{
    /* Pump control says "dose done" after 10 s of a 20 s dose: not a full dose, can't judge. */
    struct eng_params p = test_params();
    CHECK(eng_init(&E, &p, 0) == 0);
    T = 0;
    pump_on = false;
    diag_events = lockout_events = 0;
    short_dose_ms = 10000;
    int t = -1;
    for (int i = 0; i < 1000 && diag_events == 0; i++) {
        if (t < 0 && eng_phase(&E) == ENG_WATERING)
            t = 0;
        int32_t soil = DRY, dist = FULL;
        if (t >= 0)
            ok_shape(t++, &soil, &dist);
        feed(soil, dist);
    }
    CHECK(eng_last_diag(&E) == ENG_DIAG_INCONCLUSIVE && eng_phase(&E) == ENG_IDLE);
}

/* ================================================================ noise */

/* Small level jitter on every reading, one stray echo like M3's 212.6 cm, and single and
 * double soil spikes. Before vs. after on filtered values must ignore all of it. */
static const int32_t jitter[] = {0, 2, -2, 1, -1, 0, 2, -1};

static void noisy(int t, int32_t *s, int32_t *d)
{
    *d += jitter[(unsigned)t % 8];
    if (t == 50)
        *d = 2126;
    if (t == 60 || t == 80 || t == 81)
        *s = 1023;
    if (t == 70)
        *s = 0;
}
static void noisy_ok(int t, int32_t *s, int32_t *d) { ok_shape(t, s, d); noisy(t, s, d); }
static void noisy_noflow(int t, int32_t *s, int32_t *d) { noflow_shape(t, s, d); noisy(t, s, d); }

static void test_noise_does_not_fake_success(void)
{
    CHECK(run(noisy_noflow) == ENG_DIAG_PUMP_NO_FLOW); /* spikes are not "soil got wetter" */
}

static void test_noise_does_not_hide_success(void)
{
    CHECK(run(noisy_ok) == ENG_DIAG_OK);
}

/* ================================================================ the table itself */

static void test_classify_boundaries(void)
{
    struct eng_params p = test_params();
    /* level: |drop| <= 5 no change; >= 10 dropped; between, or rose by > 5: unclear */
    CHECK(eng_classify(&p, 10, 30) == ENG_DIAG_OK);
    CHECK(eng_classify(&p, 9, 30) == ENG_DIAG_INCONCLUSIVE);
    CHECK(eng_classify(&p, 6, 30) == ENG_DIAG_INCONCLUSIVE);
    CHECK(eng_classify(&p, 5, 30) == ENG_DIAG_LEVEL_NOT_TRACKING);
    CHECK(eng_classify(&p, -5, 30) == ENG_DIAG_LEVEL_NOT_TRACKING);
    CHECK(eng_classify(&p, -6, 30) == ENG_DIAG_INCONCLUSIVE);
    /* soil: |rise| <= 10 no change; >= 30 rose; between, or fell by > 10: unclear */
    CHECK(eng_classify(&p, 10, 29) == ENG_DIAG_INCONCLUSIVE);
    CHECK(eng_classify(&p, 10, 11) == ENG_DIAG_INCONCLUSIVE);
    CHECK(eng_classify(&p, 10, 10) == ENG_DIAG_LEAK_OR_PROBE_OUT);
    CHECK(eng_classify(&p, 10, -11) == ENG_DIAG_INCONCLUSIVE);
    CHECK(eng_classify(&p, 0, 0) == ENG_DIAG_PUMP_NO_FLOW);
    CHECK(eng_classify(&p, 0, 200) == ENG_DIAG_LEVEL_NOT_TRACKING);
}

static void test_ok_watering_still_counts_against_limits(void)
{
    CHECK(run(ok_shape) == ENG_DIAG_OK);
    for (int i = 0; i < 600; i++)
        feed(DRY, FULL); /* soil dry again right away */
    CHECK(eng_phase(&E) == ENG_DRY_DWELL && (eng_blocks(&E) & ENG_BLK_RATE_LIMIT));
}

int main(void)
{
    RUN(test_normal_watering_is_ok);
    RUN(test_no_flow);
    RUN(test_leak_or_probe_out);
    RUN(test_level_not_tracking);
    RUN(test_small_level_drop_is_unclear);
    RUN(test_small_soil_rise_is_unclear);
    RUN(test_level_rose_is_unclear);
    RUN(test_soil_got_drier_while_tank_dropped);
    RUN(test_soil_lost_during_verification);
    RUN(test_level_lost_during_verification);
    RUN(test_echoes_lost_during_verification);
    RUN(test_interrupted_by_sensor_loss);
    RUN(test_interrupted_by_low_reservoir);
    RUN(test_short_dose_reported_as_done);
    RUN(test_noise_does_not_fake_success);
    RUN(test_noise_does_not_hide_success);
    RUN(test_classify_boundaries);
    RUN(test_ok_watering_still_counts_against_limits);
    return unit_done("test_verify");
}
