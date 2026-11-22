/* SIMULATED relay kept in a file (owner: Sam), for running the real hub on a PC. The state lives
 * in <dir>/pump.sim ("1" on, "0" off) and every command is appended to <dir>/pump.sim.log as
 * "<CLOCK_MONOTONIC ns> <pid> ON|OFF". Like a real relay, the file keeps its last state when the
 * process that set it dies: after a crash only the next pump_init() turns it off.
 * Not hardware; anything produced with it is simulated. */
#ifndef PLANT_PUMP_SIMFILE_H
#define PLANT_PUMP_SIMFILE_H

#include <limits.h>

#include "actuator/pump.h"

struct pump_simfile {
    char state_path[PATH_MAX];
    char log_path[PATH_MAX];
};

/* Paths under dir (created if missing, one level). Returns 0, or -1 if a path is too long or
 * dir can't be created. */
int pump_simfile_open(struct pump_simfile *f, const char *dir);
struct pump_backend pump_simfile_backend(struct pump_simfile *f);

#endif
