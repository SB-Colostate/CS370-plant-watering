/* Private to src/analysis: helpers shared by engine.c, history.c and verify.c.
 * Not part of the engine's interface; nothing outside src/analysis includes this. */
#ifndef PLANT_ENGINE_INTERNAL_H
#define PLANT_ENGINE_INTERNAL_H

#include "analysis/engine.h"

#define MS 1000000ull /* ns per ms */

/* engine.c */
void engi_out_reset(struct eng_out *out);
void engi_set_phase(struct eng *e, enum eng_phase ph, struct eng_out *out);
void engi_enter_lockout(struct eng *e, enum eng_lock why, struct eng_out *out);

/* history.c */
uint64_t engi_elapsed_ns(uint64_t now, uint64_t then); /* never negative */
void engi_history_prune(struct eng *e);
void engi_history_add(struct eng *e, int64_t t_ns, uint32_t dose_ms);
bool engi_rate_limited(const struct eng *e);
bool engi_daily_capped(const struct eng *e); /* call after engi_history_prune */

/* verify.c */
void engi_track_verification(struct eng *e);
void engi_watch_verify(struct eng *e, struct eng_out *out);

#endif
