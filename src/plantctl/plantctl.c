/* plantctl: read-only view of the log. Never opens anything for writing (I7). */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/exitcodes.h"
#include "common/paths.h"
#include "store/store.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: plantctl [-d datadir] status\n"
            "       plantctl [-d datadir] tail [N]   (default N=20)\n");
}

static const char *event_name(unsigned kind)
{
    switch (kind) {
    case REC_EV_START: return "START";
    case REC_EV_RECOVERED: return "RECOVERED";
    case REC_EV_GAP: return "GAP";
    case REC_EV_SAMPLER_RESTART: return "SAMPLER_RESTART";
    default: return "EVENT?";
    }
}

static const char *sensor_name(uint8_t id)
{
    switch (id) {
    case SENSOR_SYSTEM: return "system";
    case SENSOR_SOIL: return "soil";
    case SENSOR_LEVEL: return "level";
    default: return "sensor?";
    }
}

/* Human-readable value column. */
static void fmt_value(const struct record *r, char *out, size_t n)
{
    if (r->flags & REC_F_EVENT) {
        unsigned kind = REC_EVENT_KIND(r->flags);
        switch (kind) {
        case REC_EV_RECOVERED: snprintf(out, n, "%s dropped=%dB", event_name(kind), r->value); break;
        case REC_EV_GAP: snprintf(out, n, "%s missed=%d", event_name(kind), r->value); break;
        case REC_EV_SAMPLER_RESTART: snprintf(out, n, "%s pid=%d", event_name(kind), r->value); break;
        default: snprintf(out, n, "%s", event_name(kind)); break;
        }
    } else if (r->sensor_id == SENSOR_LEVEL) {
        if (r->flags & REC_F_NO_ECHO)
            snprintf(out, n, "NO_ECHO");
        else
            snprintf(out, n, "%d.%d cm", r->value / 10, r->value % 10);
    } else {
        snprintf(out, n, "%d raw", r->value);
    }
}

static void fmt_wall(int64_t wall_s, char *out, size_t n)
{
    time_t t = (time_t)wall_s;
    struct tm tm;
    if (!localtime_r(&t, &tm) || strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm) == 0)
        snprintf(out, n, "%lld", (long long)wall_s);
}

static void print_record(const struct record *r)
{
    char wall[32], val[48];
    fmt_wall(r->wall_s, wall, sizeof wall);
    fmt_value(r, val, sizeof val);
    printf("%8u  %s  %12.3f  %-6s  %s\n", r->seq, wall, (double)r->mono_ns / 1e9,
           sensor_name(r->sensor_id), val);
}

/* Age from CLOCK_MONOTONIC; only meaningful if the record is from this boot. */
static void fmt_age(const struct record *r, char *out, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    if (r->mono_ns > now)
        snprintf(out, n, "age ? (earlier boot)");
    else
        snprintf(out, n, "age %.1f s", (double)(now - r->mono_ns) / 1e9);
}

static int cmd_tail(const struct store_reader *rd, long n)
{
    uint32_t start = (n >= (long)rd->count) ? 0 : rd->count - (uint32_t)n;
    printf("%8s  %-19s  %12s  %-6s  %s\n", "seq", "wall", "mono_s", "sensor", "value");
    for (uint32_t i = start; i < rd->count; i++) {
        struct record r;
        int rc = store_reader_get(rd, i, &r);
        if (rc != 0) {
            fprintf(stderr, "plantctl: record %u unreadable (rc=%d)\n", i, rc);
            return rc;
        }
        print_record(&r);
    }
    return 0;
}

static int cmd_status(const struct store_reader *rd, const char *dir)
{
    int locked = store_locked(dir);
    printf("data dir:   %s\n", dir);
    printf("writer:     %s\n", locked == 1 ? "hub running (LOCK held)" : locked == 0 ? "none (no hub holds LOCK)" : "unknown");
    printf("records:    %u", rd->count);
    if (rd->count > 0)
        printf(" (last seq %u)", rd->count - 1);
    printf("\n");
    off_t valid = (off_t)rd->count * REC_SIZE;
    if (rd->file_size > valid)
        printf("torn tail:  %lld bytes past the valid prefix (cut on next hub start)\n",
               (long long)(rd->file_size - valid));

    /* Walk backwards until we've seen the latest of each kind. */
    struct record last_soil = {0}, last_level = {0}, last_start = {0}, last_recov = {0}, last_gap = {0};
    int have_soil = 0, have_level = 0, have_start = 0, have_recov = 0, have_gap = 0;
    for (uint32_t i = rd->count; i-- > 0;) {
        struct record r;
        int rc = store_reader_get(rd, i, &r);
        if (rc != 0) {
            fprintf(stderr, "plantctl: record %u unreadable (rc=%d)\n", i, rc);
            return rc;
        }
        if (r.flags & REC_F_EVENT) {
            unsigned k = REC_EVENT_KIND(r.flags);
            if (k == REC_EV_START && !have_start) last_start = r, have_start = 1;
            if (k == REC_EV_RECOVERED && !have_recov) last_recov = r, have_recov = 1;
            if (k == REC_EV_GAP && !have_gap) last_gap = r, have_gap = 1;
        } else if (r.sensor_id == SENSOR_SOIL && !have_soil) {
            last_soil = r, have_soil = 1;
        } else if (r.sensor_id == SENSOR_LEVEL && !have_level) {
            last_level = r, have_level = 1;
        }
        if (have_soil && have_level && have_start && have_recov && have_gap)
            break;
    }

    char val[48], age[40], wall[32];
    if (have_soil) {
        fmt_value(&last_soil, val, sizeof val);
        fmt_age(&last_soil, age, sizeof age);
        printf("soil:       %-14s seq %-8u %s\n", val, last_soil.seq, age);
    } else {
        printf("soil:       no samples\n");
    }
    if (have_level) {
        fmt_value(&last_level, val, sizeof val);
        fmt_age(&last_level, age, sizeof age);
        printf("level:      %-14s seq %-8u %s\n", val, last_level.seq, age);
    } else {
        printf("level:      no samples\n");
    }
    if (have_start) {
        fmt_wall(last_start.wall_s, wall, sizeof wall);
        printf("last START: seq %u at %s\n", last_start.seq, wall);
    }
    if (have_recov) {
        fmt_wall(last_recov.wall_s, wall, sizeof wall);
        printf("last RECOVERED: seq %u at %s, dropped %d bytes\n", last_recov.seq, wall, last_recov.value);
    } else {
        printf("last RECOVERED: none\n");
    }
    if (have_gap)
        printf("last GAP:   seq %u, missed %d\n", last_gap.seq, last_gap.value);
    return 0;
}

int main(int argc, char **argv)
{
    char dir[PATH_MAX];
    const char *dirarg = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "d:h")) != -1) {
        switch (opt) {
        case 'd': dirarg = optarg; break;
        default: usage(); return EXIT_USAGE;
        }
    }
    if (dirarg) {
        if (snprintf(dir, sizeof dir, "%s", dirarg) >= (int)sizeof dir) {
            fprintf(stderr, "plantctl: data dir path too long\n");
            return EXIT_USAGE;
        }
    } else if (default_data_dir(dir, sizeof dir) < 0) {
        fprintf(stderr, "plantctl: $HOME unset; pass -d datadir\n");
        return EXIT_USAGE;
    }
    if (optind >= argc) {
        usage();
        return EXIT_USAGE;
    }

    const char *cmd = argv[optind];
    long n = 20;
    if (strcmp(cmd, "tail") == 0 && optind + 1 < argc) {
        char *end;
        errno = 0;
        n = strtol(argv[optind + 1], &end, 10);
        if (errno || *end || n < 0) {
            fprintf(stderr, "plantctl: bad N '%s'\n", argv[optind + 1]);
            return EXIT_USAGE;
        }
    } else if (strcmp(cmd, "status") != 0 && strcmp(cmd, "tail") != 0) {
        usage();
        return EXIT_USAGE;
    }

    struct store_reader rd;
    int rc = store_reader_open(&rd, dir);
    if (rc != 0)
        return rc;
    rc = strcmp(cmd, "status") == 0 ? cmd_status(&rd, dir) : cmd_tail(&rd, n);
    store_reader_close(&rd);
    return rc;
}
