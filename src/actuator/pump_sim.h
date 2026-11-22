/* SIMULATED pump backend (owner: Sam): stands in for the relay until the pump and GPIO wiring
 * can be tested. Records what it was told and can be made to fail. Nothing it does is a
 * hardware measurement; anything produced with it must be labelled simulated. */
#ifndef PLANT_PUMP_SIM_H
#define PLANT_PUMP_SIM_H

#include <stdbool.h>
#include <stdint.h>

#include "actuator/pump.h"

struct pump_sim {
    bool relay;            /* what the simulated relay is doing */
    int fail_next_on;      /* this many ON commands fail (relay unchanged) */
    int fail_next_off;     /* this many OFF commands fail (relay stays on: stuck) */
    uint64_t commands, ons, offs;
};

/* A backend that drives `sim`. */
struct pump_backend pump_sim_backend(struct pump_sim *sim);

#endif
