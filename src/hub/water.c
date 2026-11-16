/* Hub glue: hub + store <-> watering engine <-> pump control. See water.h for the interim
 * design (pump control inside the hub, SIMULATED relay). */
#include "hub/water.h"

#include <stdio.h>
#include <time.h>

#include "common/exitcodes.h"

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Engine event kind <-> log record kind (record.h). */
static unsigned rec_kind(enum eng_event_kind k)
{
    switch (k) {
    case ENG_EV_WATER_INTENT: return REC_EV_WATER_INTENT;
    case ENG_EV_DIAG: return REC_EV_DIAG;
    case ENG_EV_LOCKOUT: return REC_EV_LOCKOUT;
    case ENG_EV_ACK: return REC_EV_ACK;
    case ENG_EV_PHASE: return REC_EV_PHASE;
    case ENG_EV_BLOCKS: return REC_EV_BLOCKS;
    case ENG_EV_NONE: break;
    }
    return 0;
}

static enum eng_event_kind eng_kind(const struct record *r)
{
    if (!(r->flags & REC_F_EVENT) || r->sensor_id != SENSOR_SYSTEM)
        return ENG_EV_NONE;
    switch (REC_EVENT_KIND(r->flags)) {
    case REC_EV_WATER_INTENT: return ENG_EV_WATER_INTENT;
    case REC_EV_DIAG: return ENG_EV_DIAG;
    case REC_EV_LOCKOUT: return ENG_EV_LOCKOUT;
    case REC_EV_ACK: return ENG_EV_ACK;
    case REC_EV_PHASE: return ENG_EV_PHASE;
    case REC_EV_BLOCKS: return ENG_EV_BLOCKS;
    default: return ENG_EV_NONE; /* samples, START, GAP...: only the time is used */
    }
}

/* pump control's reason -> the engine's: anything abnormal is an actuator failure. */
static enum eng_stop eng_stop_of(enum pump_end why)
{
    switch (why) {
    case PUMP_END_DOSE_DONE: return ENG_STOP_DOSE_DONE;
    case PUMP_END_STOPPED: return ENG_STOP_REQUESTED;
    case PUMP_END_CAP: return ENG_STOP_CAP;
    default: return ENG_STOP_ACT_FAIL;
    }
}

static int from_pump(struct water *w, const struct pump_report *rep);

/* Every engine output: save its events first (durable ones with fdatasync), and only then pass
 * its request to pump control. If a save fails, return the error WITHOUT acting on the request:
 * the hub exits, the pump stays as it was told last (and pump control's own rules apply). */
static int from_engine(struct water *w, const struct eng_out *out)
{
    for (unsigned i = 0; i < out->n_events; i++) {
        struct record r = {
            .sensor_id = SENSOR_SYSTEM,
            .flags = REC_EVENT_FLAGS(rec_kind(out->ev[i].kind)),
            .mono_ns = mono_ns(),
            .wall_s = (int64_t)time(NULL),
            .value = out->ev[i].value,
        };
        int rc = store_append(w->s, &r, out->ev[i].durable ? STORE_DURABLE : 0);
        if (rc != 0) {
            fprintf(stderr, "water: could not save engine event kind %u: request not passed on\n",
                    rec_kind(out->ev[i].kind));
            return rc;
        }
        w->events_logged++;
    }
    if (out->dropped_info || out->dropped_critical)
        fprintf(stderr, "water: ERROR engine dropped events (info=%u critical=%u)\n", out->dropped_info,
                out->dropped_critical);

    struct pump_report rep;
    if (out->request == ENG_REQ_START) {
        pump_start(&w->pc, mono_ns(), out->dose_ms, &rep);
        return from_pump(w, &rep);
    }
    if (out->request == ENG_REQ_STOP) {
        pump_stop(&w->pc, mono_ns(), &rep);
        return from_pump(w, &rep);
    }
    return 0;
}

static int from_pump(struct water *w, const struct pump_report *rep)
{
    struct eng_out out;
    if (rep->kind == PUMP_REP_STARTED) {
        eng_on_watering_started(&w->e, mono_ns(), &out);
        return from_engine(w, &out);
    }
    if (rep->kind == PUMP_REP_STOPPED) {
        fprintf(stderr, "water: pump stopped (%s) after %u ms\n", pump_end_name(rep->why), rep->ran_ms);
        eng_on_watering_stopped(&w->e, mono_ns(), rep->ran_ms, eng_stop_of(rep->why), &out);
        return from_engine(w, &out);
    }
    return 0;
}

int water_pump_off(struct water *w, const char *dir)
{
    if (pump_simfile_open(&w->relay, dir) < 0) {
        fprintf(stderr, "water: simulated relay path under %s unusable\n", dir);
        return EXIT_SYSERR;
    }
    struct pump_params pp;
    pump_params_default(&pp);
    struct pump_backend b = pump_simfile_backend(&w->relay);
    if (pump_init(&w->pc, &pp, &b, mono_ns()) != 0) {
        fprintf(stderr, "water: could not switch the pump OFF at startup: not running\n");
        return EXIT_SYSERR;
    }
    w->pump_ready = true;
    fprintf(stderr, "water: SIMULATED pump (%s), relay OFF at startup. Interim: pump control runs "
                    "inside the hub; not for a real pump\n", w->relay.state_path);
    return 0;
}

int water_open(struct water *w, struct store *s, const char *dir, bool test_timings)
{
    w->s = s;
    struct eng_params p;
    eng_params_default(&p);
    if (test_timings) {
        /* Short timings so a whole cycle runs in seconds. TEST ONLY. */
        p.dry_dwell_ms = 3000;
        p.warmup_ms = 3000;
        p.dose_ms = 3000;
        p.verify_window_ms = 6000;
        fprintf(stderr, "water: TEST TIMINGS (-t): dwell 3 s, warm-up 3 s, dose 3 s, verify 6 s\n");
    }
    if (eng_init(&w->e, &p, mono_ns()) != 0) {
        fprintf(stderr, "water: engine params invalid: %s\n", eng_params_check(&p));
        return EXIT_USAGE;
    }

    /* Replay the WHOLE log, oldest first (docs/engine-hub-integration.md §2). */
    struct store_reader rd;
    int rc = store_reader_open(&rd, dir);
    if (rc != 0)
        return rc;
    for (uint32_t i = 0; i < rd.count && rc == 0; i++) {
        struct record r;
        rc = store_reader_get(&rd, i, &r);
        if (rc == 0) {
            enum eng_event_kind k = eng_kind(&r);
            eng_restore_event(&w->e, k, k == ENG_EV_NONE ? 0 : r.value, r.mono_ns);
            w->replayed++;
        }
    }
    store_reader_close(&rd);
    if (rc != 0) {
        fprintf(stderr, "water: replay failed at a record (rc=%d)\n", rc);
        return rc;
    }

    struct eng_out out;
    eng_restore_done(&w->e, &out);
    w->engine_ready = true;
    fprintf(stderr, "water: replayed %lu records; engine %s (lock %s)\n", w->replayed,
            eng_phase_name(eng_phase(&w->e)), eng_lock_name(eng_lock_reason(&w->e)));
    return from_engine(w, &out);
}

int water_on_sample(struct water *w, const struct record *r)
{
    struct eng_out out;
    eng_on_sample(&w->e, r, &out);
    return from_engine(w, &out);
}

int water_tick(struct water *w)
{
    uint64_t now = mono_ns();
    struct eng_out out;
    eng_tick(&w->e, now, &out);
    int rc = from_engine(w, &out);
    if (rc != 0)
        return rc;
    struct pump_report rep;
    pump_hub_alive(&w->pc, now, &rep);
    if ((rc = from_pump(w, &rep)) != 0)
        return rc;
    pump_tick(&w->pc, now, &rep);
    return from_pump(w, &rep);
}

int water_close(struct water *w)
{
    if (!w->pump_ready)
        return 0;
    struct pump_report rep;
    pump_shutdown(&w->pc, mono_ns(), &rep);
    w->pump_ready = false;
    if (w->engine_ready && w->s)
        return from_pump(w, &rep); /* the engine logs how the watering ended, if one was running */
    return 0;
}
