/* Test helper: a fake Uno for the REAL sampler. Every 200 ms it writes "SOIL,<n>,DIST,<cm>" into a
 * FIFO, and it simulates the plant from the SIMULATED relay file the hub writes (pump.sim):
 * while the relay reads "1" the tank drains 5 mm/s and, after 1 s, the soil is wet; the soil
 * dries again dry_after_s seconds after the relay went off. All values are synthetic.
 * usage: fakeuno <fifo> <relay-file> [dry_after_s]   (stops on SIGTERM) */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static int relay_on(const char *path)
{
    char c = '0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (read(fd, &c, 1) != 1)
        c = '0';
    close(fd);
    return c == '1';
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: fakeuno <fifo> <relay-file> [dry_after_s]\n");
        return 1;
    }
    int dry_after_ticks = (argc > 3 ? atoi(argv[3]) : 8) * 5;
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int fd = open(argv[1], O_WRONLY);
    if (fd < 0) {
        perror("fakeuno: open fifo");
        return 1;
    }
    int water_mm = 100;   /* 100 mm above the inlet: distance 200 - 100 = 100 mm */
    int on_ticks = 0, off_ticks = 0, wet = 0, was_on = 0;
    struct timespec tick = {.tv_sec = 0, .tv_nsec = 200000000};
    while (!stop) {
        int on = relay_on(argv[2]);
        if (on != was_on)
            fprintf(stderr, "fakeuno: relay %s (water %d mm)\n", on ? "ON" : "OFF", water_mm);
        was_on = on;
        if (on) {
            if (water_mm > 0)
                water_mm--;            /* 1 mm per 200 ms */
            if (++on_ticks >= 5)
                wet = 1;
            off_ticks = 0;
        } else if (wet && ++off_ticks >= dry_after_ticks) {
            wet = 0;
            on_ticks = 0;
            fprintf(stderr, "fakeuno: soil dry again\n");
        }
        int dist_mm = 200 - water_mm;
        char line[64];
        int n = snprintf(line, sizeof line, "SOIL,%d,DIST,%d.%d\r\n", wet ? 300 : 10, dist_mm / 10, dist_mm % 10);
        if (write(fd, line, (size_t)n) != n && !stop) {
            perror("fakeuno: write");
            return 1;
        }
        nanosleep(&tick, NULL);
    }
    close(fd);
    return 0;
}
