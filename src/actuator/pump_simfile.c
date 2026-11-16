/* SIMULATED relay kept in a file. See pump_simfile.h. */
#include "actuator/pump_simfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int pump_simfile_open(struct pump_simfile *f, const char *dir)
{
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -1;
    int a = snprintf(f->state_path, sizeof f->state_path, "%s/pump.sim", dir);
    int b = snprintf(f->log_path, sizeof f->log_path, "%s/pump.sim.log", dir);
    return (a < 0 || (size_t)a >= sizeof f->state_path || b < 0 || (size_t)b >= sizeof f->log_path) ? -1 : 0;
}

static int write_all(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static int sim_set(void *ctx, bool on)
{
    struct pump_simfile *f = ctx;
    int fd = open(f->state_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    int rc = write_all(fd, on ? "1\n" : "0\n", 2);
    close(fd);
    if (rc < 0)
        return -1;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[80];
    int n = snprintf(line, sizeof line, "%llu %ld %s\n",
                     (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec,
                     (long)getpid(), on ? "ON" : "OFF");
    fd = open(f->log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    rc = write_all(fd, line, (size_t)n);
    close(fd);
    return rc;
}

struct pump_backend pump_simfile_backend(struct pump_simfile *f)
{
    return (struct pump_backend){.set = sim_set, .ctx = f};
}
