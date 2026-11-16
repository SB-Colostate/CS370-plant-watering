/* Sensor filters. Fixtures marked M3 are copied from docs/evidence/m3-demo-2026-10-29.txt
 * (real Uno readings); everything else is synthetic. */
#include "analysis/filters.h"
#include "unit.h"

#define SEC 1000000000ull
#define GAP (5 * SEC) /* max gap used by these tests */

/* ---------------------------------------------------------------- median window */

static void test_median(void)
{
    struct medwin w;
    medwin_init(&w, 3);
    medwin_push(&w, 9);
    CHECK(!medwin_full(&w) && medwin_median(&w) == 9);
    medwin_push(&w, 1);
    medwin_push(&w, 5);
    CHECK(medwin_full(&w) && medwin_median(&w) == 5);
    medwin_push(&w, 7); /* window is now 1 5 7 */
    CHECK(medwin_median(&w) == 5 && w.v[0] == 1 && w.v[2] == 7);

    medwin_init(&w, 5);
    int32_t xs[] = {4, 4, 100, 4, 0};
    for (int i = 0; i < 5; i++)
        medwin_push(&w, xs[i]);
    CHECK(medwin_median(&w) == 4); /* duplicates */
}

static void test_uninitialized_window_is_safe(void)
{
    struct medwin w = {0}; /* size 0: e.g. an engine whose init failed */
    medwin_push(&w, 5);
    CHECK(w.n == 0 && !medwin_full(&w));
    CHECK(medwin_median(&w) == 0);
}

/* ---------------------------------------------------------------- soil */

static uint64_t t_soil;
static void soil_add(struct soil_filter *f, int32_t v)
{
    t_soil += SEC;
    soil_filter_add(f, v, t_soil);
}

static void soil_steady(struct soil_filter *f, int32_t v, int n)
{
    for (int i = 0; i < n; i++)
        soil_add(f, v);
}

static void test_soil_ready_and_seeded(void)
{
    struct soil_filter f;
    soil_filter_init(&f, GAP);
    t_soil = 0;
    int32_t first[] = {300, 310, 290, 305, 295};
    for (int i = 0; i < 4; i++) {
        soil_add(&f, first[i]);
        CHECK(!soil_filter_ready(&f));
    }
    soil_add(&f, first[4]);
    CHECK(soil_filter_ready(&f));
    /* Seeded from the median, not ramped up from 0 (which would look like dry soil). */
    CHECK(soil_filter_value(&f) == 300);
}

static void test_soil_constant_is_exact(void)
{
    struct soil_filter f;
    soil_filter_init(&f, GAP);
    t_soil = 0;
    soil_steady(&f, 37, 200);
    CHECK(soil_filter_value(&f) == 37);
}

static void test_soil_single_and_double_spikes_removed(void)
{
    int32_t base[] = {30, 300};
    int32_t spike[] = {1023, 0};
    for (int k = 0; k < 2; k++) {
        for (int burst = 1; burst <= 2; burst++) {
            struct soil_filter f;
            soil_filter_init(&f, GAP);
            t_soil = 0;
            soil_steady(&f, base[k], 20);
            for (int i = 0; i < burst; i++)
                soil_add(&f, spike[k]);
            for (int i = 0; i < 10; i++) {
                soil_add(&f, base[k]);
                CHECK(soil_filter_value(&f) == base[k]);
            }
        }
    }
}

static void test_soil_follows_real_step_without_overshoot(void)
{
    struct soil_filter f;
    soil_filter_init(&f, GAP);
    t_soil = 0;
    soil_steady(&f, 30, 20);
    int32_t prev = soil_filter_value(&f);
    int reached = -1;
    for (int i = 0; i < 60; i++) {
        soil_add(&f, 300);
        int32_t v = soil_filter_value(&f);
        CHECK(v >= prev && v <= 300); /* moves one way, never past the target */
        prev = v;
        if (reached < 0 && v >= 295)
            reached = i;
    }
    CHECK(reached >= 3 && reached <= 40); /* median delay ~3 samples, then EWMA */
}

static void test_soil_gap_and_backwards_time_reset(void)
{
    struct soil_filter f;
    soil_filter_init(&f, GAP);
    t_soil = 0;
    soil_steady(&f, 100, 10);

    t_soil += GAP + SEC; /* outage longer than max gap */
    soil_add(&f, 500);
    CHECK(!soil_filter_ready(&f) && f.resets == 1);
    soil_steady(&f, 500, 4);
    CHECK(soil_filter_ready(&f) && soil_filter_value(&f) == 500); /* nothing from before */

    soil_filter_add(&f, 500, t_soil - SEC); /* time ran backwards */
    CHECK(!soil_filter_ready(&f) && f.resets == 2);
}

/* M3: probe touched by hand then released (raw counts, 1 per ~1.06 s). */
static void test_soil_m3_touch_is_followed(void)
{
    static const int32_t m3[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 83, 97, 83, 72, 83,
                                 73, 83, 82, 81, 90, 83, 91, 94, 97, 96, 94, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0};
    struct soil_filter f;
    soil_filter_init(&f, GAP);
    t_soil = 0;
    int32_t peak = 0, last = 0;
    for (size_t i = 0; i < sizeof m3 / sizeof m3[0]; i++) {
        soil_add(&f, m3[i]);
        if (soil_filter_ready(&f)) {
            last = soil_filter_value(&f);
            if (last > peak)
                peak = last;
        }
    }
    CHECK(peak > 60);        /* a real, sustained change gets through */
    CHECK(peak <= 97);       /* never above the largest reading */
    CHECK(last < peak / 2);  /* and it settles back after release */
}

/* ---------------------------------------------------------------- level */

static uint64_t t_level;
static void level_add(struct level_filter *f, int32_t mm)
{
    t_level += SEC;
    level_filter_add_echo(f, mm, t_level);
}

/* M3: HC-SR04 distances in mm, seq 85..169 (odd seq), hand moved over the sensor in the middle.
 * 2126 (212.6 cm, seq 115) and 2453 (245.3 cm, seq 155) are the stray echoes. */
static void test_level_m3_stray_echoes_removed(void)
{
    static const int32_t m3[] = {168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168,
                                 168, 168, 2126, 168, 52,  39,  17,  52,  22,  23,  122, 115, 26,
                                 26,  91,  87,  27,  46,  80,  98,  162, 65,  2453, 171, 162, 162,
                                 162, 162, 162, 162};
    struct level_filter f;
    level_filter_init(&f, GAP);
    t_level = 0;
    int32_t max_out = 0;
    for (size_t i = 0; i < sizeof m3 / sizeof m3[0]; i++) {
        level_add(&f, m3[i]);
        if (level_filter_ready(&f) && level_filter_dist_mm(&f) > max_out)
            max_out = level_filter_dist_mm(&f);
    }
    CHECK(max_out <= 171); /* largest real distance near the spikes */
    CHECK(level_filter_dist_mm(&f) == 162);
}

static void test_level_two_outliers_in_a_row_get_through(void)
{
    /* Known limit of a 3-sample median, recorded on purpose. The engine must not act on one
     * filtered value alone (commit 3 adds persistence rules). */
    struct level_filter f;
    level_filter_init(&f, GAP);
    t_level = 0;
    for (int i = 0; i < 5; i++)
        level_add(&f, 160);
    level_add(&f, 2400);
    level_add(&f, 2400);
    CHECK(level_filter_dist_mm(&f) == 2400);
}

static void test_level_no_echo_is_not_a_distance(void)
{
    struct level_filter f;
    level_filter_init(&f, GAP);
    t_level = 0;

    /* No-echoes never count toward "ready". */
    level_filter_add_no_echo(&f);
    level_add(&f, 150);
    level_filter_add_no_echo(&f);
    level_add(&f, 150);
    CHECK(!level_filter_ready(&f));
    level_add(&f, 150);
    CHECK(level_filter_ready(&f) && level_filter_dist_mm(&f) == 150);
    CHECK(f.no_echo_run == 0);

    /* A run of no-echoes: the distance is not changed by them, the run is counted, and the time
     * of the newest real echo stays put so the engine can see the age growing. */
    uint64_t newest = f.newest_ns;
    for (int i = 0; i < 4; i++)
        level_filter_add_no_echo(&f);
    CHECK(f.no_echo_run == 4);
    CHECK(f.newest_ns == newest);
    CHECK(level_filter_dist_mm(&f) == 150);
    for (int i = 0; i < 3; i++)
        CHECK(f.win.v[i] == 150); /* nothing like 0 or -1 entered the window */

    level_add(&f, 152);
    CHECK(f.no_echo_run == 0 && f.newest_ns > newest);
}

static void test_level_long_outage_resets(void)
{
    struct level_filter f;
    level_filter_init(&f, GAP);
    t_level = 0;
    for (int i = 0; i < 5; i++)
        level_add(&f, 100);
    for (int i = 0; i < 10; i++)
        level_filter_add_no_echo(&f); /* echoes stop... */
    t_level += 10 * SEC;              /* ...for longer than the max gap */
    level_add(&f, 180);
    CHECK(!level_filter_ready(&f) && f.resets == 1);
    level_add(&f, 180);
    level_add(&f, 180);
    CHECK(level_filter_ready(&f) && level_filter_dist_mm(&f) == 180); /* nothing from before */

    level_filter_add_echo(&f, 180, t_level - SEC); /* time ran backwards */
    CHECK(!level_filter_ready(&f) && f.resets == 2);
}

int main(void)
{
    RUN(test_median);
    RUN(test_uninitialized_window_is_safe);
    RUN(test_soil_ready_and_seeded);
    RUN(test_soil_constant_is_exact);
    RUN(test_soil_single_and_double_spikes_removed);
    RUN(test_soil_follows_real_step_without_overshoot);
    RUN(test_soil_gap_and_backwards_time_reset);
    RUN(test_soil_m3_touch_is_followed);
    RUN(test_level_m3_stray_echoes_removed);
    RUN(test_level_two_outliers_in_a_row_get_through);
    RUN(test_level_no_echo_is_not_a_distance);
    RUN(test_level_long_outage_resets);
    return unit_done("test_filters");
}
