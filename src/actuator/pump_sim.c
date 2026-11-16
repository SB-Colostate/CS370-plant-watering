/* SIMULATED pump backend. See pump_sim.h. */
#include "actuator/pump_sim.h"

static int sim_set(void *ctx, bool on)
{
    struct pump_sim *s = ctx;
    s->commands++;
    if (on && s->fail_next_on > 0) {
        s->fail_next_on--;
        return -1;
    }
    if (!on && s->fail_next_off > 0) {
        s->fail_next_off--;
        return -1;
    }
    s->relay = on;
    if (on)
        s->ons++;
    else
        s->offs++;
    return 0;
}

struct pump_backend pump_sim_backend(struct pump_sim *sim)
{
    return (struct pump_backend){.set = sim_set, .ctx = sim};
}
