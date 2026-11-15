/* Sensor filters (owner: Sam). See filters.h. */
#include "analysis/filters.h"

#include <string.h>

/* ---------------------------------------------------------------- median window */

void medwin_init(struct medwin *w, unsigned size)
{
    memset(w, 0, sizeof *w);
    w->size = size;
}

void medwin_push(struct medwin *w, int32_t x)
{
    if (w->n == w->size) {
        memmove(&w->v[0], &w->v[1], (w->size - 1) * sizeof w->v[0]);
        w->n--;
    }
    w->v[w->n++] = x;
}

bool medwin_full(const struct medwin *w)
{
    return w->n == w->size;
}

int32_t medwin_median(const struct medwin *w)
{
    /* Insertion sort of a copy: at most 5 values. */
    int32_t s[MEDWIN_MAX];
    for (unsigned i = 0; i < w->n; i++) {
        int32_t x = w->v[i];
        unsigned j = i;
        while (j > 0 && s[j - 1] > x) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = x;
    }
    return s[w->n / 2];
}

/* True if a reading at t cannot be continued from the last one at `newest`. */
static bool discontinuity(uint64_t newest, uint64_t t, uint64_t max_gap_ns, bool have)
{
    return have && (t < newest || t - newest > max_gap_ns);
}

/* ---------------------------------------------------------------- soil */

void soil_filter_init(struct soil_filter *f, uint64_t max_gap_ns)
{
    memset(f, 0, sizeof *f);
    f->max_gap_ns = max_gap_ns;
    medwin_init(&f->win, SOIL_MEDIAN_N);
}

void soil_filter_add(struct soil_filter *f, int32_t raw, uint64_t t_ns)
{
    if (discontinuity(f->newest_ns, t_ns, f->max_gap_ns, f->win.n > 0)) {
        medwin_init(&f->win, SOIL_MEDIAN_N);
        f->ready = false;
        f->resets++;
    }
    medwin_push(&f->win, raw);
    f->newest_ns = t_ns;
    if (!medwin_full(&f->win))
        return;

    int32_t m = medwin_median(&f->win);
    if (!f->ready) {
        f->ewma_x16 = m * 16; /* seed from data, not from 0 (0 would look like dry soil) */
        f->ready = true;
    } else {
        f->ewma_x16 += (m * 16 - f->ewma_x16) / (1 << SOIL_EWMA_SHIFT);
    }
}

bool soil_filter_ready(const struct soil_filter *f)
{
    return f->ready;
}

int32_t soil_filter_value(const struct soil_filter *f)
{
    return (f->ewma_x16 + 8) / 16; /* round to nearest count */
}

/* ---------------------------------------------------------------- level */

void level_filter_init(struct level_filter *f, uint64_t max_gap_ns)
{
    memset(f, 0, sizeof *f);
    f->max_gap_ns = max_gap_ns;
    medwin_init(&f->win, LEVEL_MEDIAN_N);
}

void level_filter_add_echo(struct level_filter *f, int32_t dist_mm, uint64_t t_ns)
{
    if (discontinuity(f->newest_ns, t_ns, f->max_gap_ns, f->win.n > 0)) {
        medwin_init(&f->win, LEVEL_MEDIAN_N);
        f->resets++;
    }
    medwin_push(&f->win, dist_mm);
    f->newest_ns = t_ns;
    f->no_echo_run = 0;
}

void level_filter_add_no_echo(struct level_filter *f)
{
    /* Not a distance: count it, and leave the window and newest_ns alone so the age shows. */
    f->no_echo_run++;
}

bool level_filter_ready(const struct level_filter *f)
{
    return medwin_full(&f->win);
}

int32_t level_filter_dist_mm(const struct level_filter *f)
{
    return medwin_median(&f->win);
}
