/* Watering engine, commit 1: parameters, safe initial state, and the rule that the engine never
 * requests watering it has not decided on. All inputs are fixtures, not hardware readings. */
#include <string.h>

#include "analysis/engine.h"
#include "common/config.h"
#include "unit.h"

#define SEC 1000000000ull

/* Checks that hold after EVERY engine call, in this commit and all later ones. */
static void check_out(const struct eng *e, const struct eng_out *out)
{
    const char *why = eng_check_output(e, out);
    CHECK(why == NULL);
    if (why)
        fprintf(stderr, "    unsafe output: %s\n", why);
    CHECK(out->dropped_info == 0);
    CHECK(e->internal_faults == 0);
    /* Checked here directly, not only through the engine's own eng_check_output: */
    if (out->request == ENG_REQ_START) {
        CHECK(eng_blocks(e) == 0);                                      /* no blocking condition */
        CHECK(eng_phase(e) == ENG_START_REQUESTED);
        CHECK(e->now_ns - e->dry_since_ns >= (uint64_t)e->p.dry_dwell_ms * 1000000u); /* dwell done */
        CHECK(out->dose_ms > 0 && out->dose_ms < e->p.pump_cap_ms && out->dose_ms < CFG_PUMP_CAP_MS);
        int intents = 0;
        for (unsigned i = 0; i < out->n_events; i++)
            if (out->ev[i].kind == ENG_EV_WATER_INTENT && out->ev[i].durable &&
                out->ev[i].value == (int32_t)out->dose_ms)
                intents++;
        CHECK(intents == 1);                                            /* one durable intent */
    }
}

static int has_event(const struct eng_out *out, enum eng_event_kind k, int32_t v, bool durable)
{
    for (unsigned i = 0; i < out->n_events; i++)
        if (out->ev[i].kind == k && out->ev[i].value == v && out->ev[i].durable == durable)
            return 1;
    return 0;
}

static struct record soil(int32_t v, uint64_t t)
{
    return (struct record){.sensor_id = SENSOR_SOIL, .mono_ns = t, .value = v};
}

static struct record level(int32_t dist_mm, uint64_t t)
{
    return (struct record){.sensor_id = SENSOR_LEVEL, .mono_ns = t, .value = dist_mm};
}

static struct record level_no_echo(uint64_t t)
{
    return (struct record){.sensor_id = SENSOR_LEVEL, .flags = REC_F_NO_ECHO, .mono_ns = t};
}

/* White-box: puts the engine where a real START would leave it (commit 4 will get there through
 * inputs) and builds a valid START output. Tests break it one way at a time. */
static void valid_start(struct eng *e, struct eng_out *out)
{
    struct eng_params p;
    eng_params_default(&p);
    eng_init(e, &p, 0);
    e->phase = ENG_START_REQUESTED;
    e->blocks = 0;
    memset(out, 0, sizeof *out);
    out->request = ENG_REQ_START;
    out->dose_ms = p.dose_ms;
    eng_out_push(out, ENG_EV_PHASE, ENG_START_REQUESTED, false);
    eng_out_push(out, ENG_EV_WATER_INTENT, (int32_t)p.dose_ms, true);
}

/* ---------------------------------------------------------------- parameters */

static void test_default_params_valid(void)
{
    struct eng_params p;
    eng_params_default(&p);
    const char *why = eng_params_check(&p);
    CHECK(why == NULL);
    if (why)
        fprintf(stderr, "    default params rejected: %s\n", why);
    CHECK(p.pump_cap_ms == CFG_PUMP_CAP_MS);
    CHECK(CFG_PUMP_CAP_MS == 600000); /* the 10-minute cap is a team rule, not a tunable */
    CHECK(p.dose_ms < p.pump_cap_ms);
}

static void test_bad_params_rejected(void)
{
    struct eng_params good, p;
    eng_params_default(&good);

/* Each bad value must be rejected BY THE RULE FOR THAT FIELD (the message starts with the
 * field name). Otherwise a later rule can hide a missing one: an inverted empty/full distance
 * also breaks the reserve rule, so "some rule rejected it" would pass with the distance check gone. */
#define BAD(field, stmt)                                                           \
    do {                                                                           \
        p = good;                                                                  \
        stmt;                                                                      \
        const char *why = eng_params_check(&p);                                    \
        int ok = why != NULL && strncmp(why, field, strlen(field)) == 0;           \
        CHECK(ok);                                                                 \
        if (!ok)                                                                   \
            fprintf(stderr, "    %s: want rejection by %s, got: %s\n", #stmt, field, \
                    why ? why : "accepted");                                       \
    } while (0)

    BAD("soil_dry_enter", p.soil_dry_enter = -1);
    BAD("soil_dry_exit", p.soil_dry_exit = p.soil_dry_enter);          /* no hysteresis */
    BAD("soil_rail_high", p.soil_rail_high = p.soil_dry_exit);
    BAD("soil_rail_high", p.soil_rail_high = 1024);
    BAD("dry_dwell_ms", p.dry_dwell_ms = 0);
    BAD("level_full_dist_mm", p.level_full_dist_mm = 0);
    /* The distance mix-up: "empty" set nearer the sensor than "full". */
    BAD("level_empty_dist_mm", (p.level_empty_dist_mm = 40, p.level_full_dist_mm = 200));
    BAD("level_empty_dist_mm", p.level_empty_dist_mm = p.level_full_dist_mm);
    BAD("level_reserve_mm", p.level_reserve_mm = 0);
    BAD("level_reserve_mm", p.level_reserve_mm = p.level_empty_dist_mm - p.level_full_dist_mm);
    BAD("level_hyst_mm", p.level_hyst_mm = -1);
    BAD("level_hyst_mm", p.level_hyst_mm = p.level_empty_dist_mm - p.level_full_dist_mm - p.level_reserve_mm);
    BAD("level_confirm", p.level_confirm = 0);
    BAD("no_echo_run", p.no_echo_run = 0);
    BAD("soil_rail_run", p.soil_rail_run = 0);
    BAD("pump_cap_ms", p.pump_cap_ms = 0);
    BAD("pump_cap_ms", p.pump_cap_ms = CFG_PUMP_CAP_MS + 1);         /* can't exceed the team's 10 min */
    BAD("dose_ms", p.dose_ms = 0);
    BAD("dose_ms", p.dose_ms = p.pump_cap_ms);                   /* a dose must end before the cap */
    BAD("min_interval_ms", p.min_interval_ms = p.dose_ms - 1);
    BAD("min_interval_ms", p.min_interval_ms = ENG_DAY_MS / ENG_HISTORY - 1);
    BAD("daily_dose_ms", p.daily_dose_ms = p.dose_ms - 1);
    BAD("stale_ms", p.stale_ms = 0);
    BAD("start_confirm_ms", p.start_confirm_ms = 0);
    BAD("stop_grace_ms", p.stop_grace_ms = 0);
    BAD("stop_grace_ms", p.stop_grace_ms = (p.pump_cap_ms - p.dose_ms) / 2); /* stop + give-up must fit before the cap */
#undef BAD

    /* Lowering the cap for a test is allowed, as long as the dose still fits under it. */
    p = good;
    p.pump_cap_ms = 30000;
    p.dose_ms = 5000;
    CHECK(eng_params_check(&p) == NULL);
}

/* ---------------------------------------------------------------- reservoir direction */

static void test_water_height_direction(void)
{
    struct eng_params p;
    eng_params_default(&p);
    p.level_empty_dist_mm = 300;
    p.level_full_dist_mm = 50;

    CHECK(eng_water_height_mm(&p, 300) == 0);   /* at the inlet: no usable water */
    CHECK(eng_water_height_mm(&p, 50) == 250);  /* full */
    CHECK(eng_water_height_mm(&p, 290) == 10);
    CHECK(eng_water_height_mm(&p, 310) < 0);    /* below the inlet */
    /* Larger distance must always mean less water. */
    for (int32_t d = 0; d < 400; d++)
        CHECK(eng_water_height_mm(&p, d + 1) < eng_water_height_mm(&p, d));
}

/* ---------------------------------------------------------------- initial state */

static void test_starts_idle_and_blocked(void)
{
    struct eng e;
    struct eng_params p;
    eng_params_default(&p);
    CHECK(eng_init(&e, &p, 5 * SEC) == 0);
    CHECK(eng_phase(&e) == ENG_IDLE);
    CHECK(eng_lock_reason(&e) == ENG_LOCK_NONE);
    /* Nothing measured yet, so nothing is known to be safe. */
    CHECK(eng_blocks(&e) ==
          (ENG_BLK_SOIL_STALE | ENG_BLK_LEVEL_STALE | ENG_BLK_RESERVOIR_LOW | ENG_BLK_WARMUP));
    CHECK(eng_reservoir(&e) == ENG_RES_UNKNOWN);
}

static void test_init_with_bad_params_is_inert(void)
{
    struct eng e;
    struct eng_params p;
    struct eng_out out;
    eng_params_default(&p);
    p.dose_ms = p.pump_cap_ms; /* invalid */
    CHECK(eng_init(&e, &p, 0) == -1);
    CHECK(eng_phase(&e) == ENG_LOCKOUT);
    CHECK(eng_blocks(&e) == ENG_BLK_ALL);

    struct record r = soil(0, SEC);
    eng_on_sample(&e, &r, &out);
    check_out(&e, &out);
    CHECK(out.request != ENG_REQ_START);
    eng_tick(&e, 2 * SEC, &out);
    check_out(&e, &out);
    CHECK(out.request != ENG_REQ_START);
}

/* ---------------------------------------------------------------- inputs */

static void test_rejects_malformed_samples(void)
{
    struct eng e;
    struct eng_params p;
    struct eng_out out;
    eng_params_default(&p);
    eng_init(&e, &p, 0);

    struct record bad[] = {
        {.sensor_id = SENSOR_SYSTEM, .flags = REC_EVENT_FLAGS(REC_EV_START), .mono_ns = SEC},
        {.sensor_id = SENSOR_SOIL, .flags = REC_EVENT_FLAGS(REC_EV_GAP), .mono_ns = SEC},
        {.sensor_id = 7, .mono_ns = SEC, .value = 10},                     /* unknown sensor */
        {.sensor_id = SENSOR_SOIL, .mono_ns = SEC, .value = 1024},         /* above ADC range */
        {.sensor_id = SENSOR_SOIL, .mono_ns = SEC, .value = -1},
        {.sensor_id = SENSOR_SOIL, .flags = REC_F_NO_ECHO, .mono_ns = SEC},/* soil can't be no-echo */
        {.sensor_id = SENSOR_LEVEL, .mono_ns = SEC, .value = -5},          /* negative distance */
        {.sensor_id = SENSOR_LEVEL, .flags = 0x04, .mono_ns = SEC, .value = 100}, /* unknown flag */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        eng_on_sample(&e, &bad[i], &out);
        check_out(&e, &out);
        CHECK(out.n_events == 0 && out.request == ENG_REQ_NONE);
    }
    CHECK(e.rejected == sizeof bad / sizeof bad[0]);
    CHECK(e.samples == 0 && e.soil.win.n == 0 && e.level.win.n == 0 && e.level.no_echo_run == 0);

    struct record ok[] = {soil(0, 2 * SEC), soil(1023, 3 * SEC), level(165, 3 * SEC),
                          level_no_echo(4 * SEC)};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        eng_on_sample(&e, &ok[i], &out);
        check_out(&e, &out);
    }
    CHECK(e.samples == 4);
    CHECK(e.soil.win.n == 2 && e.soil.newest_ns == 3 * SEC);
    CHECK(e.level.win.n == 1 && e.level.no_echo_run == 1);
    CHECK(e.level.newest_ns == 3 * SEC); /* the no-echo at 4 s did not refresh it */
    CHECK(e.now_ns == 4 * SEC);
}

/* Random inputs: every output must be safe (check_out), and every START must have finished the
 * dwell with no blocks and a durable intent. Stretches of good dry data alternate with random
 * garbage so that starts really happen (checked at the end, so the START checks are not vacuous).
 * Deterministic pseudo-random inputs, so a failure is reproducible. */
static void test_random_inputs_give_safe_outputs(void)
{
    struct eng e;
    struct eng_params p;
    struct eng_out out;
    eng_params_default(&p);
    p.dry_dwell_ms = 60000;
    p.warmup_ms = 10000;
    eng_init(&e, &p, 0);

    uint32_t x = 12345;
    uint64_t t = 0;
    int starts = 0, regime_left = 0;
    bool good = false;
    for (int i = 0; i < 200000; i++) {
        x = x * 1103515245u + 12345u;
        if (regime_left-- <= 0) {
            good = (x >> 20) % 3 != 0;               /* 2/3 of stretches are good data */
            regime_left = 30 + (int)((x >> 8) % 600);
        }
        t += SEC / 2 + (x >> 8) % SEC;               /* 0.5 .. 1.5 s between inputs */
        struct record r;
        unsigned k = (x >> 4) % 3;
        if (k == 2) {
            eng_tick(&e, t, &out);
        } else {
            if (good)
                r = k == 0 ? soil((int32_t)((x >> 12) % 30), t) : level(60 + (int32_t)((x >> 12) % 60), t);
            else if (k == 0)
                r = soil((int32_t)((x >> 12) % 1100) - 20, t); /* includes out-of-range values */
            else
                r = (x >> 14) % 4 == 0 ? level_no_echo(t) : level((int32_t)((x >> 12) % 400), t);
            eng_on_sample(&e, &r, &out);
        }
        check_out(&e, &out);
        if (out.request == ENG_REQ_START) {
            starts++;
            e.phase = ENG_IDLE; /* white-box: pump control is not simulated here (see test_pump.c) */
        }
    }
    CHECK(starts >= 5);
    printf("    (%d start requests checked)\n", starts);
}

/* ---------------------------------------------------------------- pump-control reports */

static void test_unexpected_pump_start_locks_out(void)
{
    struct eng e;
    struct eng_params p;
    struct eng_out out;
    eng_params_default(&p);
    eng_init(&e, &p, 0);

    eng_on_watering_started(&e, 10 * SEC, &out);
    check_out(&e, &out);
    CHECK(out.request == ENG_REQ_STOP);
    CHECK(eng_phase(&e) == ENG_LOCKOUT);
    CHECK(eng_lock_reason(&e) == ENG_LOCK_UNEXPECTED_PUMP);
    CHECK(has_event(&out, ENG_EV_LOCKOUT, ENG_LOCK_UNEXPECTED_PUMP, true));
    CHECK(has_event(&out, ENG_EV_PHASE, ENG_LOCKOUT, false));

    /* Again: still asks for STOP, keeps the first reason, logs nothing new. */
    eng_on_watering_started(&e, 11 * SEC, &out);
    check_out(&e, &out);
    CHECK(out.request == ENG_REQ_STOP && out.n_events == 0);
    CHECK(eng_lock_reason(&e) == ENG_LOCK_UNEXPECTED_PUMP);
    CHECK(e.unexpected_reports == 2);
}

static void test_unexpected_stop_report(void)
{
    struct eng e;
    struct eng_params p;
    struct eng_out out;
    eng_params_default(&p);

    /* "Stopped, ran 0 ms" while idle: harmless confirmation, no lockout. */
    eng_init(&e, &p, 0);
    eng_on_watering_stopped(&e, SEC, 0, ENG_STOP_REQUESTED, &out);
    check_out(&e, &out);
    CHECK(eng_phase(&e) == ENG_IDLE && out.request == ENG_REQ_NONE);
    CHECK(e.unexpected_reports == 1);

    /* "Stopped, ran 4 s" while idle: water moved without a request. */
    eng_on_watering_stopped(&e, 2 * SEC, 4000, ENG_STOP_DOSE_DONE, &out);
    check_out(&e, &out);
    CHECK(out.request == ENG_REQ_STOP);
    CHECK(eng_phase(&e) == ENG_LOCKOUT);
    CHECK(eng_lock_reason(&e) == ENG_LOCK_UNEXPECTED_PUMP);
}

/* ---------------------------------------------------------------- output buffer */

static void test_out_push_reserves_slots_for_critical_events(void)
{
    struct eng_out out;
    memset(&out, 0, sizeof out);

    /* Flood with informational events: they stop at the non-reserved limit. */
    for (int i = 0; i < 20; i++)
        eng_out_push(&out, ENG_EV_PHASE, i, false);
    CHECK(out.n_events == ENG_MAX_EVENTS - ENG_RESERVED_EVENTS);
    CHECK(out.dropped_info == 20 - (ENG_MAX_EVENTS - ENG_RESERVED_EVENTS));
    CHECK(out.dropped_critical == 0);

    /* One of each critical kind still fits. */
    CHECK(eng_out_push(&out, ENG_EV_WATER_INTENT, 5000, true));
    CHECK(eng_out_push(&out, ENG_EV_DIAG, ENG_DIAG_OK, false));
    CHECK(eng_out_push(&out, ENG_EV_LOCKOUT, ENG_LOCK_PUMP_TIMEOUT, true));
    CHECK(out.n_events == ENG_MAX_EVENTS && out.dropped_critical == 0);
    CHECK(out.ev[ENG_MAX_EVENTS - 1].kind == ENG_EV_LOCKOUT);

    /* A fourth critical event cannot fit: counted, never silent. */
    CHECK(!eng_out_push(&out, ENG_EV_LOCKOUT, ENG_LOCK_ACTUATOR, true));
    CHECK(out.dropped_critical == 1);
}

/* ---------------------------------------------------------------- output check */

static void test_check_output_accepts_a_valid_start(void)
{
    struct eng e;
    struct eng_out out;
    valid_start(&e, &out);
    CHECK(eng_check_output(&e, &out) == NULL);
}

static void test_check_output_rejects_unsafe_starts(void)
{
    struct eng e;
    struct eng_out out;

#define UNSAFE(stmt)                                                    \
    do {                                                                \
        valid_start(&e, &out);                                          \
        stmt;                                                           \
        CHECK(eng_check_output(&e, &out) != NULL);                      \
        if (eng_check_output(&e, &out) == NULL)                         \
            fprintf(stderr, "    accepted unsafe output: %s\n", #stmt); \
    } while (0)

    /* Dose limits. */
    UNSAFE((out.dose_ms = 0, out.ev[1].value = 0));
    UNSAFE((out.dose_ms = e.p.pump_cap_ms, out.ev[1].value = (int32_t)e.p.pump_cap_ms));
    UNSAFE((out.dose_ms = e.p.pump_cap_ms + 1, out.ev[1].value = (int32_t)e.p.pump_cap_ms + 1));
    /* Even if the engine's own cap were corrupted above 10 min, the team cap still applies. */
    UNSAFE((e.p.pump_cap_ms = CFG_PUMP_CAP_MS + 60000, out.dose_ms = CFG_PUMP_CAP_MS,
            out.ev[1].value = CFG_PUMP_CAP_MS));
    /* Request vs. state. */
    UNSAFE(e.phase = ENG_IDLE);
    UNSAFE(e.phase = ENG_WATERING);   /* already running: a second START is not allowed */
    UNSAFE(e.phase = ENG_LOCKOUT);
    UNSAFE(e.blocks = ENG_BLK_RESERVOIR_LOW);
    /* The intent record that must be on disk before the pump starts. */
    UNSAFE(out.ev[1].durable = false);
    UNSAFE(out.ev[1].value = (int32_t)out.dose_ms + 1);
    UNSAFE((out.n_events = 1));       /* intent missing */
    UNSAFE(eng_out_push(&out, ENG_EV_WATER_INTENT, (int32_t)out.dose_ms, true)); /* two intents */
    /* Lost or duplicated critical events. */
    UNSAFE(out.dropped_critical = 1);
    UNSAFE((eng_out_push(&out, ENG_EV_LOCKOUT, 1, true), eng_out_push(&out, ENG_EV_LOCKOUT, 2, true)));
    /* A dose with no START. */
    UNSAFE(out.request = ENG_REQ_NONE);
    UNSAFE(out.request = ENG_REQ_STOP);
#undef UNSAFE
}

static void test_enforce_replaces_unsafe_start_with_stop(void)
{
    struct eng e;
    struct eng_out out;
    valid_start(&e, &out);
    out.dose_ms = e.p.pump_cap_ms; /* would reach the 10-minute cap */
    out.ev[1].value = (int32_t)out.dose_ms;

    eng_enforce_output(&e, &out);
    CHECK(out.request == ENG_REQ_STOP && out.dose_ms == 0);
    CHECK(eng_phase(&e) == ENG_LOCKOUT);
    CHECK(eng_lock_reason(&e) == ENG_LOCK_INTERNAL);
    CHECK(e.internal_faults == 1);
    CHECK(has_event(&out, ENG_EV_LOCKOUT, ENG_LOCK_INTERNAL, true));
    CHECK(eng_check_output(&e, &out) == NULL); /* what the hub receives is now safe */
}

static void test_enforce_when_critical_event_was_lost(void)
{
    struct eng e;
    struct eng_out out;
    valid_start(&e, &out);
    out.dropped_critical = 1;

    eng_enforce_output(&e, &out);
    CHECK(out.request == ENG_REQ_STOP && out.dose_ms == 0);
    CHECK(eng_phase(&e) == ENG_LOCKOUT && eng_lock_reason(&e) == ENG_LOCK_INTERNAL);
    /* Still flagged, so the hub knows to log the loss itself. */
    CHECK(out.dropped_critical >= 1);
}

static void test_enforce_keeps_earlier_lock_reason(void)
{
    struct eng e;
    struct eng_out out;
    valid_start(&e, &out);
    e.phase = ENG_LOCKOUT;
    e.lock_reason = ENG_LOCK_PUMP_TIMEOUT;

    eng_enforce_output(&e, &out); /* START from LOCKOUT: unsafe */
    CHECK(out.request == ENG_REQ_STOP);
    CHECK(eng_lock_reason(&e) == ENG_LOCK_PUMP_TIMEOUT);
    CHECK(e.internal_faults == 1);
}

/* ---------------------------------------------------------------- requested vs. confirmed */

/* "Started" is only an expected report right after our own START. White-box: phases that
 * inputs cannot reach yet in commit 1 are set directly. */
static void test_started_report_expected_only_after_request(void)
{
    struct eng_params p;
    eng_params_default(&p);
    enum eng_phase unexpected[] = {ENG_IDLE, ENG_DRY_DWELL, ENG_WATERING, ENG_VERIFYING, ENG_LOCKOUT};
    for (size_t i = 0; i < sizeof unexpected / sizeof unexpected[0]; i++) {
        struct eng e;
        struct eng_out out;
        eng_init(&e, &p, 0);
        e.phase = unexpected[i];
        eng_on_watering_started(&e, SEC, &out);
        CHECK(out.request == ENG_REQ_STOP);
        CHECK(eng_phase(&e) == ENG_LOCKOUT);
        CHECK(e.unexpected_reports == 1);
        if (unexpected[i] != ENG_LOCKOUT)
            CHECK(eng_lock_reason(&e) == ENG_LOCK_UNEXPECTED_PUMP);
    }

    struct eng e;
    struct eng_out out;
    eng_init(&e, &p, 0);
    e.phase = ENG_START_REQUESTED;
    eng_on_watering_started(&e, SEC, &out);
    check_out(&e, &out);
    CHECK(e.unexpected_reports == 0);
    CHECK(eng_phase(&e) != ENG_LOCKOUT && out.request != ENG_REQ_STOP);
}

static void test_stopped_report_expected_while_requested_or_watering(void)
{
    /* Expected in both phases (not counted as unexpected), but what it means differs.
     * White-box: phases set directly; test_pump.c drives the same through real reports. */
    struct eng_params p;
    eng_params_default(&p);
    struct eng e;
    struct eng_out out;

    /* WATERING: a normal end of the dose -> VERIFYING. */
    eng_init(&e, &p, 0);
    e.phase = ENG_WATERING;
    eng_on_watering_stopped(&e, SEC, 4000, ENG_STOP_DOSE_DONE, &out);
    check_out(&e, &out);
    CHECK(e.unexpected_reports == 0 && eng_phase(&e) == ENG_VERIFYING);

    /* START_REQUESTED: the pump ran but never confirmed the start -> ACTUATOR lockout. */
    eng_init(&e, &p, 0);
    e.phase = ENG_START_REQUESTED;
    eng_on_watering_stopped(&e, SEC, 4000, ENG_STOP_DOSE_DONE, &out);
    check_out(&e, &out);
    CHECK(e.unexpected_reports == 0 && eng_phase(&e) == ENG_LOCKOUT);
    CHECK(eng_lock_reason(&e) == ENG_LOCK_ACTUATOR && out.request == ENG_REQ_STOP);
}

/* ---------------------------------------------------------------- names */

static void test_names_cover_enums(void)
{
    for (int ph = ENG_IDLE; ph <= ENG_LOCKOUT; ph++)
        CHECK(strcmp(eng_phase_name((enum eng_phase)ph), "?") != 0);
    for (uint32_t b = 1; b <= ENG_BLK_ALL; b <<= 1) {
        CHECK(strcmp(eng_block_name(b), "?") != 0);
        for (uint32_t c = b << 1; c <= ENG_BLK_ALL; c <<= 1)
            CHECK(strcmp(eng_block_name(b), eng_block_name(c)) != 0);
    }
    CHECK(strcmp(eng_block_name(ENG_BLK_ALL), "?") == 0); /* names are for single bits */
    for (int d = ENG_DIAG_NONE; d <= ENG_DIAG_CHANNELING; d++)
        CHECK(strcmp(eng_diag_name((enum eng_diag)d), "?") != 0);
    for (int l = ENG_LOCK_NONE; l <= ENG_LOCK_INTERNAL; l++)
        CHECK(strcmp(eng_lock_name((enum eng_lock)l), "?") != 0);
    for (int r = ENG_REQ_NONE; r <= ENG_REQ_STOP; r++)
        CHECK(strcmp(eng_request_name((enum eng_request)r), "?") != 0);
}

int main(void)
{
    RUN(test_default_params_valid);
    RUN(test_bad_params_rejected);
    RUN(test_water_height_direction);
    RUN(test_starts_idle_and_blocked);
    RUN(test_init_with_bad_params_is_inert);
    RUN(test_rejects_malformed_samples);
    RUN(test_random_inputs_give_safe_outputs);
    RUN(test_unexpected_pump_start_locks_out);
    RUN(test_unexpected_stop_report);
    RUN(test_out_push_reserves_slots_for_critical_events);
    RUN(test_check_output_accepts_a_valid_start);
    RUN(test_check_output_rejects_unsafe_starts);
    RUN(test_enforce_replaces_unsafe_start_with_stop);
    RUN(test_enforce_when_critical_event_was_lost);
    RUN(test_enforce_keeps_earlier_lock_reason);
    RUN(test_started_report_expected_only_after_request);
    RUN(test_stopped_report_expected_while_requested_or_watering);
    RUN(test_names_cover_enums);
    return unit_done("test_engine");
}
