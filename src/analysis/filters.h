/* Sensor filters for the watering engine (owner: Sam). Pure: no system calls, no clock reads.
 *
 * Soil:  median of the last SOIL_MEDIAN_N raw readings, then an EWMA with weight 1/2^SOIL_EWMA_SHIFT.
 *        The median removes up to two bad readings in a row completely; the EWMA smooths noise.
 * Level: median of the last 3 VALID ultrasonic distances. A no-echo reading is never a distance:
 *        it is counted in no_echo_run and does not enter the window.
 *
 * Neither filter hides a silent sensor: newest_ns is the time of the newest REAL reading, and only
 * real readings advance it. The engine decides staleness from it. A gap longer than max_gap_ns
 * between real readings (or time going backwards) resets the filter, so readings from before an
 * outage are never mixed with readings after it.
 */
#ifndef PLANT_FILTERS_H
#define PLANT_FILTERS_H

#include <stdbool.h>
#include <stdint.h>

/* Algorithm constants (not hardware calibration). */
#define SOIL_MEDIAN_N   5 /* removes up to 2 consecutive bad readings */
#define SOIL_EWMA_SHIFT 3 /* weight 1/8: about 8 samples (~8 s at 1 Hz) to follow a change */
#define LEVEL_MEDIAN_N  3 /* removes 1 stray echo; 2 in a row get through (see tests) */
#define MEDWIN_MAX      5

/* The last `size` values, oldest first. */
struct medwin {
    int32_t v[MEDWIN_MAX];
    unsigned n, size;
};

void medwin_init(struct medwin *w, unsigned size); /* 1 <= size <= MEDWIN_MAX, odd */
void medwin_push(struct medwin *w, int32_t x);     /* drops the oldest when full */
bool medwin_full(const struct medwin *w);
int32_t medwin_median(const struct medwin *w);     /* median of the n values held; n > 0 */

struct soil_filter {
    uint64_t max_gap_ns;
    struct medwin win;
    int32_t ewma_x16;  /* smoothed value in 1/16 counts, so small steps survive rounding */
    bool ready;        /* window has been full since the last reset */
    uint64_t newest_ns;
    uint64_t resets;
};

void soil_filter_init(struct soil_filter *f, uint64_t max_gap_ns);
void soil_filter_add(struct soil_filter *f, int32_t raw, uint64_t t_ns);
bool soil_filter_ready(const struct soil_filter *f);
int32_t soil_filter_value(const struct soil_filter *f); /* smoothed counts; only when ready */

struct level_filter {
    uint64_t max_gap_ns;
    struct medwin win;
    uint64_t newest_ns;   /* newest VALID echo; no-echo readings do not move it */
    uint32_t no_echo_run; /* no-echo readings since the last valid echo */
    uint64_t resets;
};

void level_filter_init(struct level_filter *f, uint64_t max_gap_ns);
void level_filter_add_echo(struct level_filter *f, int32_t dist_mm, uint64_t t_ns);
void level_filter_add_no_echo(struct level_filter *f);
bool level_filter_ready(const struct level_filter *f);
/* Median DISTANCE in mm: larger means less water. Only when ready. */
int32_t level_filter_dist_mm(const struct level_filter *f);

#endif
