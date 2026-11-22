/* Watering engine, commit 4: deciding the plant needs water vs. requesting a start, the dwell
 * timer and hysteresis, the rate limit and daily cap, and restoring them after a restart.
 * Synthetic fixtures: one soil + one level sample per second, then a tick.
 *
 * Test params: soil dry at <= 40, wet again at >= 80 (our probe reads LOW when dry);
 * dwell 60 s; warm-up 10 s; stale 5 s; dose 20 s; >= 1 h between starts; 60 s per 24 h
 * (3 doses). Level: distance 100 mm = plenty of water, 195 mm = nearly empty. */
#include <limits.h>

#include "analysis/engine.h"
#include "unit.h"

#define SEC 1000000000ull
#define MIN (60 * SEC)
#define HOUR (3600 * SEC)
#define NONE INT32_MIN
#define NOECHO (-1)
#define DRY 20
#define WET 300
#define FULL 100
#define LOW 195

static struct eng E;
static struct eng_out O;
static uint64_t T;
static int starts;
static uint64_t start_times[16];
static int32_t saved_intent; /* dose of the last WATER_INTENT, as the hub would have saved it */
static int phase_changes;
static int back_to_idle; /* DRY_DWELL -> IDLE: "dry" was withdrawn */
static enum eng_phase prev_phase;

/* The WATER_INTENT event in an output: its index, or -1. Position does not matter: the hub
 * saves every event of a call before it acts on the request. */
static int intent_index(const struct eng_out *o)
{
    for (unsigned i = 0; i < o->n_events; i++)
        if (o->ev[i].kind == ENG_EV_WATER_INTENT)
            return (int)i;
    return -1;
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
    p.daily_dose_ms = 60000;
    return p;
}

/* After every call. A START must pass the engine's own output check (dose under the cap, no
 * blocks, a durable intent for the same dose) AND come from a finished dry dwell. */
static void safe(void)
{
    const char *why = eng_check_output(&E, &O);
    CHECK(why == NULL);
    if (why)
        fprintf(stderr, "    unsafe output: %s\n", why);
    CHECK(O.dropped_info == 0 && E.internal_faults == 0);
    if (O.request == ENG_REQ_START) {
        CHECK(eng_blocks(&E) == 0);
        CHECK(eng_phase(&E) == ENG_START_REQUESTED);
        CHECK(E.now_ns - E.dry_since_ns >= (uint64_t)E.p.dry_dwell_ms * 1000000u);
        int k = intent_index(&O);
        CHECK(k >= 0 && O.ev[k].durable && O.ev[k].value == (int32_t)O.dose_ms);
        if (k >= 0)
            saved_intent = O.ev[k].value;
        if (starts < 16)
            start_times[starts] = E.now_ns;
        starts++;
    }
    if (eng_phase(&E) != prev_phase) {
        phase_changes++;
        if (prev_phase == ENG_DRY_DWELL && eng_phase(&E) == ENG_IDLE)
            back_to_idle++;
    }
    prev_phase = eng_phase(&E);
}

/* Output of eng_restore_done, checked like any other. */
static void safe_out(const struct eng_out *o)
{
    CHECK(eng_check_output(&E, o) == NULL && o->dropped_info == 0);
}

static void start_at(uint64_t t0, const struct eng_params *p)
{
    CHECK(eng_init(&E, p, t0) == 0);
    T = t0;
    starts = 0;
    phase_changes = 0;
    back_to_idle = 0;
    prev_phase = eng_phase(&E);
}

static void start(void)
{
    struct eng_params p = test_params();
    start_at(0, &p);
}

static void step(int32_t soil, int32_t dist)
{
    T += SEC;
    struct record r;
    if (soil != NONE) {
        r = (struct record){.sensor_id = SENSOR_SOIL, .mono_ns = T, .value = soil};
        eng_on_sample(&E, &r, &O);
        safe();
    }
    if (dist != NONE) {
        r = (struct record){.sensor_id = SENSOR_LEVEL, .mono_ns = T};
        if (dist == NOECHO)
            r.flags = REC_F_NO_ECHO;
        else
            r.value = dist;
        eng_on_sample(&E, &r, &O);
        safe();
    }
    eng_tick(&E, T, &O);
    safe();
}

static void steps(int n, int32_t soil, int32_t dist)
{
    for (int i = 0; i < n; i++)
        step(soil, dist);
}

/* Runs until a start is requested or `limit` seconds pass. Returns seconds taken, or -1. */
static int run_until_start(int limit, int32_t soil, int32_t dist)
{
    int before = starts;
    for (int i = 1; i <= limit; i++) {
        step(soil, dist);
        if (starts > before)
            return i;
    }
    return -1;
}

/* White-box stand-in: the watering (test_pump.c) and its verification (commit 6) are over. */
static void finish_watering(void)
{
    E.phase = ENG_IDLE;
    prev_phase = ENG_IDLE;
}

/* ================================================================ it DOES request water */

static void test_requests_start_when_dry_and_safe(void)
{
    start();
    int t = run_until_start(200, DRY, FULL);
    /* Soil is trustworthy at 5 s (5-reading median), so the 60 s dwell ends at ~65 s. */
    CHECK(t >= 64 && t <= 67);
    CHECK(starts == 1 && eng_phase(&E) == ENG_START_REQUESTED);
    CHECK(O.request == ENG_REQ_START || O.request == ENG_REQ_NONE);
    /* The request carries the configured dose and a durable intent (checked in safe()). */
    steps(100, DRY, FULL);
    CHECK(starts == 1); /* one request, not one per second */
}

static void test_needs_water_is_separate_from_starting(void)
{
    start();
    steps(120, DRY, LOW); /* dry plant, nearly empty tank */
    CHECK(eng_needs_water(&E));                              /* the decision... */
    CHECK(starts == 0 && (eng_blocks(&E) & ENG_BLK_RESERVOIR_LOW)); /* ...but no request */

    /* Refilled: the start comes as soon as the reservoir is confirmed; no new dwell. */
    int t = run_until_start(20, DRY, FULL);
    CHECK(t >= 1 && t <= 6);
}

/* ================================================================ dwell and hysteresis */

static void test_short_dry_readings_are_not_dry(void)
{
    start();
    for (int i = 0; i < 600; i++)
        step(i % 10 == 0 || i % 10 == 1 ? 0 : WET, FULL); /* two 0 readings every 10 s */
    CHECK(eng_phase(&E) == ENG_IDLE && starts == 0);
}

static void test_no_flapping_inside_the_hysteresis_band(void)
{
    start();
    steps(20, WET, FULL);
    steps(300, 60, FULL);  /* in the band (40..80), coming from wet: still not dry */
    CHECK(eng_phase(&E) == ENG_IDLE && phase_changes == 0);

    for (int i = 0; i < 300; i++)
        step(i % 2 ? 35 : 45, FULL); /* hovering right at the dry threshold */
    CHECK(back_to_idle == 0);        /* once dry, it does not flip back and forth */

    start();
    steps(20, DRY, FULL);
    CHECK(eng_phase(&E) == ENG_DRY_DWELL);
    int t = run_until_start(200, 60, FULL); /* in the band, coming from dry: still dry */
    CHECK(t > 0 && phase_changes == 2);     /* IDLE -> DRY_DWELL -> START_REQUESTED only */
}

static void test_wet_again_restarts_the_dwell(void)
{
    start();
    steps(40, DRY, FULL);
    steps(20, WET, FULL);
    CHECK(eng_phase(&E) == ENG_IDLE && starts == 0);
    uint64_t dry_again = T;
    CHECK(run_until_start(200, DRY, FULL) > 0);
    CHECK(start_times[0] - dry_again >= 60 * SEC); /* a full new dwell */
}

static void test_soil_unplugged_restarts_the_dwell(void)
{
    start();
    steps(50, DRY, FULL);
    steps(10, NONE, FULL);
    CHECK(eng_phase(&E) == ENG_IDLE && starts == 0); /* can't trust "dry" without readings */
    uint64_t back = T;
    CHECK(run_until_start(200, DRY, FULL) > 0);
    CHECK(start_times[0] - back >= 65 * SEC); /* 5 readings to trust it, then a full dwell */
}

/* ================================================================ it REFUSES when unsafe */

/* Dwell finishes while a fault is present: no start while it lasts, a start after it clears. */
static void check_refuses_then_starts(int32_t bad_soil, int32_t bad_dist, uint32_t expect_block)
{
    start();
    steps(30, DRY, FULL);
    for (int i = 0; i < 120; i++)
        step(bad_soil, bad_dist);
    CHECK(starts == 0);
    CHECK(eng_blocks(&E) & expect_block);
    CHECK(run_until_start(120, DRY, FULL) > 0);
}

static void test_refuses_while_level_unplugged(void) { check_refuses_then_starts(DRY, NONE, ENG_BLK_LEVEL_STALE); }
static void test_refuses_while_no_echo(void) { check_refuses_then_starts(DRY, NOECHO, ENG_BLK_LEVEL_NO_ECHO); }
static void test_refuses_while_reservoir_low(void) { check_refuses_then_starts(DRY, LOW, ENG_BLK_RESERVOIR_LOW); }

static void test_refuses_during_warmup(void)
{
    struct eng_params p = test_params();
    p.dry_dwell_ms = 1000; /* dwell shorter than the warm-up */
    start_at(0, &p);
    int t = run_until_start(60, DRY, FULL);
    CHECK(t >= 15); /* 5 s to trust the soil, then 10 s of warm-up */
}

static void test_never_starts_on_wet_or_rail_soil(void)
{
    start();
    steps(600, WET, FULL);
    steps(600, 1023, FULL); /* pinned at the rail */
    CHECK(starts == 0);
}

/* ================================================================ rate limit and daily cap */

static void test_rate_limit_between_starts(void)
{
    start();
    CHECK(run_until_start(200, DRY, FULL) > 0);
    finish_watering();
    steps(120, DRY, FULL);
    CHECK(starts == 1 && eng_needs_water(&E));
    CHECK(eng_blocks(&E) == ENG_BLK_RATE_LIMIT); /* the only thing in the way */
    CHECK(run_until_start(3600, DRY, FULL) > 0);
    CHECK(start_times[1] - start_times[0] >= HOUR);
    CHECK(start_times[1] - start_times[0] <= HOUR + 2 * SEC);
}

static void test_daily_cap(void)
{
    start();
    for (int i = 0; i < 30 * 3600 && starts < 5; i++) {
        int before = starts;
        step(DRY, FULL);
        if (starts > before)
            finish_watering();
    }
    CHECK(starts >= 4);
    /* 3 doses of 20 s fit in 60 s/day: the 3rd at ~2 h, the 4th must wait for the 1st to age out. */
    CHECK(start_times[2] - start_times[0] < 3 * HOUR);
    CHECK(start_times[3] - start_times[0] >= 24 * HOUR);
    CHECK(start_times[3] - start_times[0] <= 24 * HOUR + 2 * SEC);
}

/* ================================================================ restarts can't bypass limits */

static void test_restart_cannot_bypass_rate_limit(void)
{
    start();
    CHECK(run_until_start(200, DRY, FULL) > 0);
    uint64_t t1 = start_times[0];
    int32_t dose = saved_intent; /* the intent the hub saved */
    CHECK(dose == 20000);

    /* Without restoring, a new engine 10 min later would water within ~65 s... */
    struct eng_params p = test_params();
    start_at(t1 + 10 * MIN, &p);
    CHECK(run_until_start(200, DRY, FULL) > 0);

    /* ...so the hub replays the saved intent into the new engine first. */
    start_at(t1 + 10 * MIN, &p);
    eng_restore_event(&E, ENG_EV_WATER_INTENT, dose, t1);
    eng_restore_event(&E, ENG_EV_DIAG, ENG_DIAG_OK, t1 + 2 * MIN); /* closed, as in a real log */
    { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }
    CHECK(E.restored == 1);
    int t = run_until_start(3600, DRY, FULL);
    CHECK(t > 0);
    CHECK(start_times[0] >= t1 + HOUR && start_times[0] <= t1 + HOUR + 2 * SEC);
}

static void test_restart_cannot_bypass_daily_cap(void)
{
    struct eng_params p = test_params();
    start_at(10 * HOUR, &p);
    for (int h = 1; h <= 5; h += 2) {
        eng_restore_event(&E, ENG_EV_WATER_INTENT, 20000, (uint64_t)h * HOUR);
        eng_restore_event(&E, ENG_EV_DIAG, ENG_DIAG_OK, (uint64_t)h * HOUR + 2 * MIN);
    }
    { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }
    steps(60, DRY, FULL);
    CHECK(eng_blocks(&E) & ENG_BLK_DAILY_CAP);
    CHECK(run_until_start(16 * 3600, DRY, FULL) > 0);
    /* The 1 h start ages out at 25 h; then 2 doses + 1 new = 60 s fits. */
    CHECK(start_times[0] >= 25 * HOUR && start_times[0] <= 25 * HOUR + 2 * SEC);
}

static void test_restored_start_from_earlier_boot_counts_as_now(void)
{
    /* After a reboot the monotonic clock starts over: a saved time can be "in the future".
     * With nothing else in the log, all we know is that it was before this boot: it counts
     * as just now (one interval), not as 30 days from now. */
    struct eng_params p = test_params();
    start_at(1 * HOUR, &p);
    eng_restore_event(&E, ENG_EV_WATER_INTENT, 20000, 30 * 24 * HOUR);
    eng_restore_event(&E, ENG_EV_DIAG, ENG_DIAG_OK, 30 * 24 * HOUR);
    { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }
    int t = run_until_start(2 * 3600, DRY, FULL);
    CHECK(t > 0);
    CHECK(start_times[0] >= 2 * HOUR && start_times[0] <= 2 * HOUR + 2 * SEC);
}

static void test_unreadable_restored_dose_assumes_the_worst(void)
{
    struct eng_params p = test_params();
    start_at(0, &p);
    eng_restore_event(&E, ENG_EV_WATER_INTENT, -5, 0); /* corrupt value */
    eng_restore_event(&E, ENG_EV_DIAG, ENG_DIAG_OK, 0);
    { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }
    steps(2 * 3600, DRY, FULL);
    CHECK(starts == 0 && (eng_blocks(&E) & ENG_BLK_DAILY_CAP)); /* counted as a full 10 min */
}

/* Many hub restarts AND Pi reboots, checked in REAL time. A "log" keeps every saved WATER_INTENT
 * with the monotonic time of the boot it was written in. On every restart the hub replays the
 * WHOLE log into a fresh engine. After a reboot the monotonic clock starts over (at 30 s here),
 * so old times can look newer than "now". No two starts may be closer than the interval and no
 * 24 h window may exceed the daily cap, whatever the restart pattern. */
static void test_many_restarts_and_reboots_never_exceed_limits(void)
{
    struct eng_params p = test_params();
    struct { uint64_t mono; enum eng_event_kind kind; int32_t value; } log[512];
    int nlog = 0;
    uint64_t real_at[256];
    int32_t dose_at[256];
    int n = 0, restarts = 0, reboots = 0;
    const uint64_t BOOT_MONO = 30 * SEC; /* monotonic time when the hub starts after a boot */
    uint64_t real = 0, boot_real = 0;    /* real time now, and when the current boot began */
    uint32_t x = 7;

    while (real < 6 * 24 * HOUR && nlog < 500) {
        start_at(real - boot_real + BOOT_MONO, &p);
        for (int i = 0; i < nlog; i++)
            eng_restore_event(&E, log[i].kind, log[i].value, log[i].mono);
        { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }

        x = x * 1103515245u + 12345u;
        uint64_t run_s = 600 + (x >> 8) % (8 * 3600); /* this run lasts 10 min .. 8 h */
        for (uint64_t s = 0; s < run_s; s++) {
            real += SEC;
            int before = starts;
            step(DRY, FULL);
            if (starts > before) {
                log[nlog].mono = E.now_ns;
                log[nlog].kind = ENG_EV_WATER_INTENT;
                log[nlog].value = saved_intent;
                nlog++;
                log[nlog].mono = E.now_ns; /* its verification result (stand-in, see finish_watering) */
                log[nlog].kind = ENG_EV_DIAG;
                log[nlog].value = ENG_DIAG_OK;
                nlog++;
                real_at[n] = real;
                dose_at[n] = saved_intent;
                n++;
                finish_watering();
            }
        }
        /* The real log has samples up to the moment of the crash: keep the last one. */
        log[nlog].mono = E.now_ns;
        log[nlog].kind = ENG_EV_NONE;
        log[nlog].value = 0;
        nlog++;
        x = x * 1103515245u + 12345u;
        if ((x >> 16) % 2) {
            boot_real = real; /* reboot: the monotonic clock starts over */
            reboots++;
        } else {
            restarts++;       /* hub restart: same boot, same clock */
        }
    }

    for (int i = 0; i < n; i++) {
        if (i > 0)
            CHECK(real_at[i] - real_at[i - 1] >= HOUR);
        int64_t used = 0;
        for (int j = 0; j <= i; j++)
            if (real_at[i] - real_at[j] < 24 * HOUR)
                used += dose_at[j];
        CHECK(used <= (int64_t)p.daily_dose_ms);
    }
    CHECK(reboots >= 5 && restarts >= 5);
    CHECK(n >= 12); /* safe AND most waterings still happen (18 is the most possible; restarts
                     * cost some, since each one redoes the warm-up and the dry dwell) */
    printf("    (%d starts over 6 days, %d hub restarts, %d reboots; at most 18 possible)\n", n,
           restarts, reboots);
}

static void test_earlier_boot_that_kept_running_ages_correctly(void)
{
    /* Same reboot, but the log shows the old boot kept running after the start: 25 h of later
     * records (e.g. samples) prove the start is more than a day old. It no longer counts. */
    struct eng_params p = test_params();
    start_at(1 * HOUR, &p);
    eng_restore_event(&E, ENG_EV_WATER_INTENT, 20000, 30 * 24 * HOUR);
    eng_restore_event(&E, ENG_EV_DIAG, ENG_DIAG_OK, 30 * 24 * HOUR + 2 * MIN);
    eng_restore_event(&E, ENG_EV_NONE, 0, 30 * 24 * HOUR + 25 * HOUR);
    { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }
    CHECK(E.n_hist == 0);
    CHECK(run_until_start(200, DRY, FULL) > 0);

    /* Only 30 min of later records: at least 30 min old, so 30 min of the interval remain. */
    start_at(1 * HOUR, &p);
    eng_restore_event(&E, ENG_EV_WATER_INTENT, 20000, 30 * 24 * HOUR);
    eng_restore_event(&E, ENG_EV_DIAG, ENG_DIAG_OK, 30 * 24 * HOUR + 2 * MIN);
    eng_restore_event(&E, ENG_EV_NONE, 0, 30 * 24 * HOUR + 30 * MIN);
    { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }
    CHECK(run_until_start(3600, DRY, FULL) > 0);
    CHECK(start_times[0] >= 1 * HOUR + 30 * MIN && start_times[0] <= 1 * HOUR + 30 * MIN + 2 * SEC);
}

static void test_restore_ignores_other_events(void)
{
    start();
    eng_restore_event(&E, ENG_EV_PHASE, ENG_WATERING, 0);
    eng_restore_event(&E, ENG_EV_DIAG, ENG_DIAG_OK, 0);
    eng_restore_event(&E, ENG_EV_NONE, 0, 0);
    { struct eng_out ro; eng_restore_done(&E, &ro); safe_out(&ro); }
    CHECK(E.restored == 0 && E.n_hist == 0);
}

static void test_restore_after_live_input_is_ignored(void)
{
    start();
    step(DRY, FULL); /* live input: restore is now closed (done implicitly) */
    eng_restore_event(&E, ENG_EV_WATER_INTENT, 20000, T);
    CHECK(E.restored == 0 && E.n_hist == 0);
}

int main(void)
{
    RUN(test_requests_start_when_dry_and_safe);
    RUN(test_needs_water_is_separate_from_starting);
    RUN(test_short_dry_readings_are_not_dry);
    RUN(test_no_flapping_inside_the_hysteresis_band);
    RUN(test_wet_again_restarts_the_dwell);
    RUN(test_soil_unplugged_restarts_the_dwell);
    RUN(test_refuses_while_level_unplugged);
    RUN(test_refuses_while_no_echo);
    RUN(test_refuses_while_reservoir_low);
    RUN(test_refuses_during_warmup);
    RUN(test_never_starts_on_wet_or_rail_soil);
    RUN(test_rate_limit_between_starts);
    RUN(test_daily_cap);
    RUN(test_restart_cannot_bypass_rate_limit);
    RUN(test_restart_cannot_bypass_daily_cap);
    RUN(test_restored_start_from_earlier_boot_counts_as_now);
    RUN(test_unreadable_restored_dose_assumes_the_worst);
    RUN(test_many_restarts_and_reboots_never_exceed_limits);
    RUN(test_earlier_boot_that_kept_running_ages_correctly);
    RUN(test_restore_ignores_other_events);
    RUN(test_restore_after_live_input_is_ignored);
    return unit_done("test_watering");
}
