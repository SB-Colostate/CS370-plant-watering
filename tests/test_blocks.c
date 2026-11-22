/* Watering engine, commit 3: sensor blocks from filtered readings. Synthetic fixtures.
 * Uno timing is modelled as one soil + one level sample per second, then a tick.
 *
 * Test params: empty_dist 200 mm, full_dist 40 mm, reserve 20 mm, hysteresis 10 mm, so
 *   distance 100 -> 100 mm of water (OK), 175 -> 25 mm (hysteresis band), 195 -> 5 mm (LOW).
 * Larger distance = less water. stale 5 s, warm-up 10 s, 3 readings to confirm. */
#include <limits.h>

#include "analysis/engine.h"
#include "unit.h"

#define SEC 1000000000ull
#define NONE INT32_MIN /* sensor sent nothing this second (unplugged) */
#define NOECHO (-1)    /* ultrasonic sent a no-echo reading */
#define SENSOR_BLOCKS                                                                     \
    (ENG_BLK_SOIL_STALE | ENG_BLK_LEVEL_STALE | ENG_BLK_LEVEL_NO_ECHO | ENG_BLK_SOIL_RAIL | \
     ENG_BLK_RESERVOIR_LOW | ENG_BLK_WARMUP)

static struct eng E;
static struct eng_out O;
static uint64_t T;
static long unblocked_steps; /* how often blocks were all clear (keeps the invariant non-vacuous) */
static uint32_t prev_blocks;  /* blocks after the previous call */

static struct eng_params test_params(void)
{
    struct eng_params p;
    eng_params_default(&p);
    p.level_empty_dist_mm = 200;
    p.level_full_dist_mm = 40;
    p.level_reserve_mm = 20;
    p.level_hyst_mm = 10;
    p.level_confirm = 3;
    p.no_echo_run = 3;
    p.soil_rail_run = 5;
    p.stale_ms = 5000;
    p.warmup_ms = 10000;
    return p;
}

/* After EVERY engine call: the output is safe, and no sensor block is clear unless its sensor
 * really has fresh, confirmed data. "Default to no watering" in one check. */
static void safe(void)
{
    CHECK(eng_check_output(&E, &O) == NULL);
    CHECK(O.dropped_info == 0 && E.internal_faults == 0);
    CHECK(O.request != ENG_REQ_START);
    /* Every change of the blocks is logged, with the new value, by the call that made it. */
    if (eng_blocks(&E) != prev_blocks) {
        int logged = 0;
        for (unsigned i = 0; i < O.n_events; i++)
            if (O.ev[i].kind == ENG_EV_BLOCKS && O.ev[i].value == (int32_t)eng_blocks(&E))
                logged = 1;
        CHECK(logged);
    }
    prev_blocks = eng_blocks(&E);
    if ((eng_blocks(&E) & SENSOR_BLOCKS) == 0) {
        uint64_t stale = (uint64_t)E.p.stale_ms * 1000000u;
        CHECK(soil_filter_ready(&E.soil) && E.now_ns - E.soil.newest_ns <= stale);
        CHECK(level_filter_ready(&E.level) && E.now_ns - E.level.newest_ns <= stale);
        CHECK(eng_reservoir(&E) == ENG_RES_OK);
        CHECK(!E.soil_rail && E.level.no_echo_run < E.p.no_echo_run);
        unblocked_steps++;
    }
}

static void start(void)
{
    struct eng_params p = test_params();
    CHECK(eng_init(&E, &p, 0) == 0);
    T = 0;
    prev_blocks = eng_blocks(&E);
}

/* One second of Uno output. soil/dist may be NONE; dist may be NOECHO. */
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

static bool blocked(uint32_t bit) { return (eng_blocks(&E) & bit) != 0; }

/* Healthy for long enough that every sensor block has cleared. */
static void healthy_start(void)
{
    start();
    steps(15, 300, 100);
    CHECK(eng_blocks(&E) == 0);
}

/* ---------------------------------------------------------------- not enough readings yet */

static void test_blocked_until_enough_good_readings(void)
{
    start();
    for (int i = 1; i <= 4; i++) {
        step(300, 100);
        CHECK(blocked(ENG_BLK_SOIL_STALE));      /* soil median needs 5 readings */
        CHECK(blocked(ENG_BLK_RESERVOIR_LOW));   /* reservoir needs 3 confirmed filtered readings */
        if (i <= 2)
            CHECK(blocked(ENG_BLK_LEVEL_STALE)); /* level median needs 3 echoes */
    }
    step(300, 100);
    CHECK(!blocked(ENG_BLK_SOIL_STALE | ENG_BLK_LEVEL_STALE | ENG_BLK_RESERVOIR_LOW));
    CHECK(blocked(ENG_BLK_WARMUP)); /* ...and the data must stay good for the warm-up */
    steps(9, 300, 100);
    CHECK(blocked(ENG_BLK_WARMUP));
    step(300, 100);
    CHECK(eng_blocks(&E) == 0); /* safe() checked that the change was logged */
}

static void test_warmup_restarts_if_a_sensor_drops_out(void)
{
    start();
    steps(8, 300, 100);
    steps(7, 300, NONE); /* level gone before warm-up finished */
    steps(15, 300, 100);
    CHECK(eng_blocks(&E) == 0);
    /* Warm-up only happens once: a later outage does not bring it back. */
    steps(7, NONE, 100);
    steps(5, 300, 100);
    CHECK(!blocked(ENG_BLK_WARMUP));
}

/* ---------------------------------------------------------------- disconnect and return */

static void test_soil_disconnect_and_return(void)
{
    healthy_start();
    steps(5, NONE, 100);
    CHECK(!blocked(ENG_BLK_SOIL_STALE)); /* last reading exactly 5 s old: still fresh */
    step(NONE, 100);
    CHECK(blocked(ENG_BLK_SOIL_STALE));  /* 6 s: stale */
    steps(10, NONE, 100);
    CHECK(blocked(ENG_BLK_SOIL_STALE));

    /* Back: the gap reset the filter, so it takes 5 new readings, none from before. */
    for (int i = 0; i < 4; i++) {
        step(300, 100);
        CHECK(blocked(ENG_BLK_SOIL_STALE));
    }
    step(300, 100);
    CHECK(eng_blocks(&E) == 0);
    CHECK(E.soil.resets == 1);
}

static void test_level_disconnect_requires_reconfirming_reservoir(void)
{
    healthy_start();
    steps(6, 300, NONE);
    CHECK(blocked(ENG_BLK_LEVEL_STALE));
    steps(30, 300, NONE); /* unplugged for a while; the tank could have been emptied meanwhile */

    step(300, 100);
    CHECK(eng_reservoir(&E) == ENG_RES_UNKNOWN); /* an old "OK" is not trusted */
    for (int i = 0; i < 3; i++) {
        step(300, 100);
        CHECK(eng_blocks(&E) != 0);
    }
    step(300, 100);
    CHECK(eng_reservoir(&E) == ENG_RES_OK && eng_blocks(&E) == 0);
}

static void test_both_sensors_silent_ticks_only(void)
{
    healthy_start();
    steps(10, NONE, NONE); /* nothing arrives at all; only the hub's ticks */
    CHECK(blocked(ENG_BLK_SOIL_STALE) && blocked(ENG_BLK_LEVEL_STALE));
}

/* ---------------------------------------------------------------- missing echoes */

static void test_short_no_echo_blip_is_tolerated(void)
{
    healthy_start();
    steps(2, 300, NOECHO);
    CHECK(eng_blocks(&E) == 0);
    step(300, 100);
    CHECK(eng_blocks(&E) == 0 && E.level.no_echo_run == 0);
}

static void test_no_echo_run_blocks_then_goes_stale(void)
{
    healthy_start();
    steps(2, 300, NOECHO);
    step(300, NOECHO);
    CHECK(blocked(ENG_BLK_LEVEL_NO_ECHO));
    CHECK(!blocked(ENG_BLK_LEVEL_STALE));
    steps(3, 300, NOECHO);
    CHECK(blocked(ENG_BLK_LEVEL_STALE)); /* no real echo for 6 s */

    step(300, 100); /* echoes return after the gap */
    CHECK(!blocked(ENG_BLK_LEVEL_NO_ECHO));
    CHECK(blocked(ENG_BLK_LEVEL_STALE | ENG_BLK_RESERVOIR_LOW)); /* filter restarted */
    steps(4, 300, 100);
    CHECK(eng_blocks(&E) == 0);
}

/* ---------------------------------------------------------------- invalid soil readings */

static void test_invalid_soil_readings_count_as_no_readings(void)
{
    healthy_start();
    uint64_t rejected = E.rejected;
    for (int i = 0; i < 6; i++)
        step(i % 2 ? 1024 : -1, 100); /* out of ADC range: rejected */
    CHECK(E.rejected == rejected + 6);
    CHECK(blocked(ENG_BLK_SOIL_STALE));
}

static void test_soil_pinned_at_rail(void)
{
    healthy_start();
    steps(4, 1023, 100);
    CHECK(!blocked(ENG_BLK_SOIL_RAIL)); /* not yet */
    step(1023, 100);
    CHECK(blocked(ENG_BLK_SOIL_RAIL));
    steps(4, 300, 100);
    CHECK(blocked(ENG_BLK_SOIL_RAIL));  /* one good stretch is not enough to clear */
    step(300, 100);
    CHECK(!blocked(ENG_BLK_SOIL_RAIL));
}

/* ---------------------------------------------------------------- reservoir */

static void test_one_or_two_low_readings_are_ignored(void)
{
    healthy_start();
    step(300, 195); /* one reading says nearly empty */
    steps(5, 300, 100);
    CHECK(eng_reservoir(&E) == ENG_RES_OK && eng_blocks(&E) == 0);

    steps(2, 300, 195); /* two in a row pass the median of 3, but not the 3-reading confirm */
    steps(5, 300, 100);
    CHECK(eng_reservoir(&E) == ENG_RES_OK && eng_blocks(&E) == 0);
}

static void test_empty_reservoir_detected_and_held(void)
{
    healthy_start();
    int found = -1;
    for (int i = 1; i <= 10 && found < 0; i++) {
        step(300, 195);
        if (eng_reservoir(&E) == ENG_RES_LOW)
            found = i;
    }
    CHECK(found >= 3 && found <= 5); /* caught within a few seconds */
    CHECK(blocked(ENG_BLK_RESERVOIR_LOW));

    steps(30, 300, NONE); /* sensor unplugged while empty: stays LOW, not forgotten */
    CHECK(eng_reservoir(&E) == ENG_RES_LOW);
    steps(10, 300, 195);
    CHECK(eng_reservoir(&E) == ENG_RES_LOW);

    /* Refilled: clears only after the filter and the confirm both agree. */
    step(300, 100);
    step(300, 100);
    CHECK(eng_reservoir(&E) == ENG_RES_LOW);
    steps(3, 300, 100);
    CHECK(eng_reservoir(&E) == ENG_RES_OK && !blocked(ENG_BLK_RESERVOIR_LOW));
}

static void test_hysteresis_band_keeps_current_state(void)
{
    healthy_start();
    steps(20, 300, 175); /* 25 mm: above reserve, below reserve + hyst */
    CHECK(eng_reservoir(&E) == ENG_RES_OK);

    steps(10, 300, 195);
    CHECK(eng_reservoir(&E) == ENG_RES_LOW);
    steps(20, 300, 175); /* back into the band: not enough to call it refilled */
    CHECK(eng_reservoir(&E) == ENG_RES_LOW);
}

static void test_larger_distance_means_less_water(void)
{
    healthy_start();
    steps(10, 300, 60);  /* 140 mm of water */
    CHECK(eng_reservoir(&E) == ENG_RES_OK);
    steps(10, 300, 190); /* 10 mm of water */
    CHECK(eng_reservoir(&E) == ENG_RES_LOW);
    steps(10, 300, 250); /* beyond the inlet: still LOW */
    CHECK(eng_reservoir(&E) == ENG_RES_LOW);
}

static void test_closer_than_full_confirms_nothing(void)
{
    /* 30 mm is nearer than a full tank (40 mm): a hand or the dead zone, not "very full". */
    start();
    steps(30, 300, 30);
    CHECK(eng_reservoir(&E) == ENG_RES_UNKNOWN && blocked(ENG_BLK_RESERVOIR_LOW));
}

/* ---------------------------------------------------------------- everything at once */

static void test_random_faults_never_unblock_wrongly(void)
{
    /* Deterministic random mix of good data, unplugs, no-echoes, rail and low readings.
     * safe() runs after every call. */
    start();
    unblocked_steps = 0;
    uint32_t x = 2026;
    for (int i = 0; i < 20000; i++) {
        x = x * 1103515245u + 12345u;
        unsigned r = (x >> 16) % 100;
        int32_t soil = r < 5 ? NONE : r < 7 ? 1023 : r < 8 ? 1024 : 300;
        unsigned q = (x >> 8) % 100;
        int32_t dist = q < 5 ? NONE : q < 9 ? NOECHO : q < 13 ? 195 : q < 15 ? 30 : 100;
        int burst = (x >> 24) % 50 == 0 ? 8 : 1; /* occasional multi-second outage/fault */
        steps(burst, soil, dist);
    }
    CHECK(unblocked_steps > 1000); /* the healthy stretches really did unblock */
}

int main(void)
{
    RUN(test_blocked_until_enough_good_readings);
    RUN(test_warmup_restarts_if_a_sensor_drops_out);
    RUN(test_soil_disconnect_and_return);
    RUN(test_level_disconnect_requires_reconfirming_reservoir);
    RUN(test_both_sensors_silent_ticks_only);
    RUN(test_short_no_echo_blip_is_tolerated);
    RUN(test_no_echo_run_blocks_then_goes_stale);
    RUN(test_invalid_soil_readings_count_as_no_readings);
    RUN(test_soil_pinned_at_rail);
    RUN(test_one_or_two_low_readings_are_ignored);
    RUN(test_empty_reservoir_detected_and_held);
    RUN(test_hysteresis_band_keeps_current_state);
    RUN(test_larger_distance_means_less_water);
    RUN(test_closer_than_full_confirms_nothing);
    RUN(test_random_faults_never_unblock_wrongly);
    return unit_done("test_blocks");
}
