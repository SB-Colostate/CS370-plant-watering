/* Verification (owner: Sam): did the water arrive? Both sensors, before vs. after. See engine.h. */
#include "analysis/engine.h"

#include "analysis/engine_internal.h"

/* ---------------------------------------------------------------- verification */

#define DATA_BLOCKS \
    (ENG_BLK_SOIL_STALE | ENG_BLK_LEVEL_STALE | ENG_BLK_LEVEL_NO_ECHO | ENG_BLK_SOIL_RAIL)

enum band { BAND_NONE, BAND_CHANGED, BAND_UNCLEAR };

/* Within +-noise: no change. At least min: changed. Anything else, including a change the
 * wrong way (water level rose, soil got drier), is too unclear to call. */
static enum band change_band(int32_t change, int32_t noise, int32_t min)
{
    if (change >= min)
        return BAND_CHANGED;
    if (change >= -noise && change <= noise)
        return BAND_NONE;
    return BAND_UNCLEAR;
}

enum eng_diag eng_classify(const struct eng_params *p, int32_t level_drop_mm, int32_t soil_rise)
{
    enum band level = change_band(level_drop_mm, p->level_noise_mm, p->level_drop_min_mm);
    enum band soil = change_band(soil_rise, p->soil_noise, p->soil_rise_min);
    if (level == BAND_UNCLEAR || soil == BAND_UNCLEAR)
        return ENG_DIAG_INCONCLUSIVE;
    if (level == BAND_CHANGED && soil == BAND_CHANGED)
        return ENG_DIAG_OK;                 /* water left the tank and reached the soil */
    if (level == BAND_NONE && soil == BAND_NONE)
        return ENG_DIAG_PUMP_NO_FLOW;       /* nothing moved: pump, relay or line */
    if (level == BAND_CHANGED)
        return ENG_DIAG_LEAK_OR_PROBE_OUT;  /* water left, soil didn't notice */
    return ENG_DIAG_LEVEL_NOT_TRACKING;     /* soil wetter, tank unchanged: gauge not tracking */
}

/* While watering and verifying: remember the wettest smoothed soil, and whether either sensor
 * ever stopped being trustworthy. */
void engi_track_verification(struct eng *e)
{
    if (e->blocks & DATA_BLOCKS) {
        e->verify_data_lost = true;
        return;
    }
    int32_t v = soil_filter_value(&e->soil);
    if (v > e->soil_peak)
        e->soil_peak = v;
}

static void finish_verification(struct eng *e, enum eng_diag d, struct eng_out *out)
{
    e->last_diag = d;
    eng_out_push(out, ENG_EV_DIAG, (int32_t)d, true);
    switch (d) {
    case ENG_DIAG_PUMP_NO_FLOW: engi_enter_lockout(e, ENG_LOCK_PUMP_NO_FLOW, out); break;
    case ENG_DIAG_LEAK_OR_PROBE_OUT: engi_enter_lockout(e, ENG_LOCK_LEAK_OR_PROBE_OUT, out); break;
    case ENG_DIAG_LEVEL_NOT_TRACKING: engi_enter_lockout(e, ENG_LOCK_LEVEL_NOT_TRACKING, out); break;
    case ENG_DIAG_OK:
        e->inconclusive_run = 0;
        engi_set_phase(e, ENG_IDLE, out); /* the limits still count this watering */
        break;
    case ENG_DIAG_INCONCLUSIVE:
        /* Water may have gone in without us knowing. Not forever: that is how overwatering
         * would happen with nobody noticing. */
        if (++e->inconclusive_run >= e->p.max_inconclusive)
            engi_enter_lockout(e, ENG_LOCK_TOO_MANY_INCONCLUSIVE, out);
        else
            engi_set_phase(e, ENG_IDLE, out);
        break;
    case ENG_DIAG_NONE:
    case ENG_DIAG_CHANNELING:
    case ENG_DIAG_CANCELLED:
        engi_set_phase(e, ENG_IDLE, out);
        break;
    }
}

void engi_watch_verify(struct eng *e, struct eng_out *out)
{
    engi_track_verification(e);
    if (engi_elapsed_ns(e->now_ns, e->verify_start_ns) < (uint64_t)e->p.verify_window_ms * MS)
        return;
    /* Only a full dose with trustworthy data the whole time can be judged. */
    bool full_dose = e->stop_why == ENG_STOP_DOSE_DONE && e->ran_ms >= e->p.dose_ms;
    if (!full_dose || e->verify_data_lost) {
        finish_verification(e, ENG_DIAG_INCONCLUSIVE, out);
        return;
    }
    int32_t water_after = eng_water_height_mm(&e->p, level_filter_dist_mm(&e->level));
    e->verify_level_drop_mm = e->water_before_mm - water_after;
    e->verify_soil_rise = e->soil_peak - e->soil_before;
    finish_verification(e, eng_classify(&e->p, e->verify_level_drop_mm, e->verify_soil_rise), out);
}
