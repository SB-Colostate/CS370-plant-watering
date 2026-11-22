/* Pump control (owner: Sam). See pump.h for the rules. Pure: switching goes through pc->b. */
#include "actuator/pump.h"

#include <string.h>

#include "common/config.h"

#define MS 1000000ull /* ns per ms */

void pump_params_default(struct pump_params *p)
{
    p->cap_ms = CFG_PUMP_CAP_MS;
    p->hub_timeout_ms = CFG_PUMP_HUB_TIMEOUT_MS;
}

const char *pump_params_check(const struct pump_params *p)
{
    if (p->cap_ms == 0 || p->cap_ms > CFG_PUMP_CAP_MS)
        return "cap_ms must be in (0, CFG_PUMP_CAP_MS]";
    if (p->hub_timeout_ms == 0)
        return "hub_timeout_ms is 0";
    return NULL;
}

static void no_report(struct pump_report *rep)
{
    memset(rep, 0, sizeof *rep);
}

/* Time since `then`. Time going backwards counts as "forever", so every limit fires. */
static uint64_t age_ns(uint64_t now, uint64_t then)
{
    return now >= then ? now - then : UINT64_MAX;
}

/* Returns true if the relay is now known to be off. A failure means FAULT. */
static bool switch_off(struct pump *pc)
{
    if (pc->b.set(pc->b.ctx, false) == 0) {
        pc->relay_maybe_on = false;
        return true;
    }
    pc->switch_failures++;
    pc->relay_maybe_on = true;
    pc->state = PUMP_FAULT;
    return false;
}

/* Ends the current run and reports it. */
static void end_run(struct pump *pc, uint64_t now, enum pump_end why, struct pump_report *rep)
{
    uint64_t ran = age_ns(now, pc->on_since_ns);
    uint32_t ran_ms = ran == UINT64_MAX ? pc->p.cap_ms /* unknown: assume the longest */
                      : ran / MS > UINT32_MAX ? UINT32_MAX : (uint32_t)(ran / MS);
    bool off = switch_off(pc);
    if (off)
        pc->state = PUMP_OFF;
    rep->kind = PUMP_REP_STOPPED;
    rep->ran_ms = ran_ms;
    rep->why = off ? why : PUMP_END_SWITCH_FAILED;
}

/* Called first by every entry point. */
static void check_limits(struct pump *pc, uint64_t now, struct pump_report *rep)
{
    if (pc->state == PUMP_FAULT) {
        if (pc->relay_maybe_on && pc->b.set(pc->b.ctx, false) == 0)
            pc->relay_maybe_on = false; /* off now, but stays FAULT until pump_init() */
        return;
    }
    if (pc->state != PUMP_ON)
        return;
    uint64_t on = age_ns(now, pc->on_since_ns);
    if (on >= (uint64_t)pc->p.cap_ms * MS)
        end_run(pc, now, PUMP_END_CAP, rep);       /* the hard limit, whatever the dose */
    else if (on >= (uint64_t)pc->dose_ms * MS)
        end_run(pc, now, PUMP_END_DOSE_DONE, rep);
    else if (age_ns(now, pc->last_heard_ns) >= (uint64_t)pc->p.hub_timeout_ms * MS)
        end_run(pc, now, PUMP_END_HUB_SILENT, rep); /* the hub is gone or hung */
}

int pump_init(struct pump *pc, const struct pump_params *p, const struct pump_backend *b, uint64_t now_ns)
{
    memset(pc, 0, sizeof *pc);
    pc->p = *p;
    pc->b = *b;
    pc->params_ok = pump_params_check(p) == NULL;
    pc->last_heard_ns = now_ns;
    pc->relay_maybe_on = true; /* a crashed process may have left it on */
    pc->state = PUMP_OFF;
    if (!switch_off(pc))
        return -1;
    return pc->params_ok ? 0 : -1;
}

void pump_start(struct pump *pc, uint64_t now_ns, uint32_t dose_ms, struct pump_report *rep)
{
    no_report(rep);
    check_limits(pc, now_ns, rep);
    pc->last_heard_ns = now_ns;
    if (rep->kind != PUMP_REP_NONE || pc->state == PUMP_ON) {
        pc->ignored_starts++; /* never restarts or extends a run */
        return;
    }
    if (!pc->params_ok || pc->shut_down || pc->state == PUMP_FAULT || dose_ms == 0 ||
        dose_ms >= pc->p.cap_ms) {
        pc->refused++;
        rep->kind = PUMP_REP_STOPPED;
        rep->why = PUMP_END_REFUSED;
        return;
    }
    pc->relay_maybe_on = true;
    if (pc->b.set(pc->b.ctx, true) != 0) {
        pc->switch_failures++;
        if (switch_off(pc)) /* make sure; if this fails too, FAULT */
            pc->state = PUMP_OFF;
        rep->kind = PUMP_REP_STOPPED;
        rep->why = PUMP_END_SWITCH_FAILED;
        return;
    }
    pc->state = PUMP_ON;
    pc->on_since_ns = now_ns;
    pc->dose_ms = dose_ms;
    pc->runs++;
    rep->kind = PUMP_REP_STARTED;
}

void pump_stop(struct pump *pc, uint64_t now_ns, struct pump_report *rep)
{
    no_report(rep);
    check_limits(pc, now_ns, rep);
    pc->last_heard_ns = now_ns;
    if (pc->state == PUMP_ON)
        end_run(pc, now_ns, PUMP_END_STOPPED, rep);
}

void pump_hub_alive(struct pump *pc, uint64_t now_ns, struct pump_report *rep)
{
    no_report(rep);
    check_limits(pc, now_ns, rep); /* with the old time: a late "alive" can't rescue a run */
    pc->last_heard_ns = now_ns;
}

void pump_tick(struct pump *pc, uint64_t now_ns, struct pump_report *rep)
{
    no_report(rep);
    check_limits(pc, now_ns, rep);
}

void pump_shutdown(struct pump *pc, uint64_t now_ns, struct pump_report *rep)
{
    no_report(rep);
    check_limits(pc, now_ns, rep);
    if (pc->state == PUMP_ON)
        end_run(pc, now_ns, PUMP_END_SHUTDOWN, rep);
    else if (pc->state == PUMP_OFF)
        (void)switch_off(pc); /* off again anyway, on the way out */
    pc->shut_down = true;
}

enum pump_state pump_state(const struct pump *pc) { return pc->state; }
bool pump_relay_maybe_on(const struct pump *pc) { return pc->relay_maybe_on; }

const char *pump_end_name(enum pump_end why)
{
    switch (why) {
    case PUMP_END_NONE: return "NONE";
    case PUMP_END_DOSE_DONE: return "DOSE_DONE";
    case PUMP_END_STOPPED: return "STOPPED";
    case PUMP_END_CAP: return "CAP";
    case PUMP_END_HUB_SILENT: return "HUB_SILENT";
    case PUMP_END_SHUTDOWN: return "SHUTDOWN";
    case PUMP_END_SWITCH_FAILED: return "SWITCH_FAILED";
    case PUMP_END_REFUSED: return "REFUSED";
    }
    return "?";
}
