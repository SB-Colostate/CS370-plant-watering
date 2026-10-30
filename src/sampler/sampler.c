/* sampler: reads Uno lines from the serial port and sends one datagram per reading to the hub.
 * Exits EXIT_DEVICE on EOF/EIO (unplug) or when no valid frame arrives in time. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "common/config.h"
#include "common/exitcodes.h"
#include "common/msg.h"
#include "common/paths.h"
#include "sampler/parse.h"
#include "store/record.h"

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Raw 9600 8N1. Skipped for non-ttys so tests can feed a FIFO. */
static int setup_tty(int fd)
{
    if (!isatty(fd))
        return 0;
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;
    cfmakeraw(&t);
    cfsetispeed(&t, B9600);
    cfsetospeed(&t, B9600);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)HUPCL; /* don't drop DTR on close: avoids an extra Uno reset */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0)
        return -1;
    return tcflush(fd, TCIFLUSH);
}

struct sampler {
    int sock;
    struct sockaddr_un hub;
    int print_only;
    uint32_t seq;
    uint32_t pid;
    unsigned long sent, send_drops, bad_frames;
};

static void emit(struct sampler *sp, uint8_t sensor, uint8_t flags, int32_t value, uint64_t t)
{
    struct plant_msg m = {
        .magic = MSG_MAGIC, .version = MSG_VERSION, .type = MSG_SAMPLE,
        .sampler_pid = sp->pid, .sampler_seq = sp->seq++,
        .sensor_id = sensor, .flags = flags, .mono_ns = t, .value = value,
    };
    if (sendto(sp->sock, &m, sizeof m, MSG_DONTWAIT, (struct sockaddr *)&sp->hub, sizeof sp->hub) ==
        (ssize_t)sizeof m) {
        sp->sent++;
        return;
    }
    /* Hub down or restarting: count it and keep sampling. The hub sees the seq gap. */
    if (sp->send_drops++ % 60 == 0)
        fprintf(stderr, "sampler: send to hub failed (%s), drops=%lu\n", strerror(errno), sp->send_drops);
}

static void handle_line(struct sampler *sp, const char *line, size_t len, uint64_t t)
{
    struct reading r;
    if (parse_line(line, len, &r) < 0) {
        sp->bad_frames++;
        fprintf(stderr, "sampler: bad frame #%lu: \"%.*s\"\n", sp->bad_frames, (int)len, line);
        return;
    }
    if (sp->print_only) {
        if (r.no_echo)
            printf("soil_raw=%d level=NO_ECHO\n", r.soil);
        else
            printf("soil_raw=%d level=%d.%d cm\n", r.soil, r.dist_mm / 10, r.dist_mm % 10);
        fflush(stdout);
        return;
    }
    emit(sp, SENSOR_SOIL, 0, r.soil, t);
    emit(sp, SENSOR_LEVEL, r.no_echo ? MSG_F_NO_ECHO : 0, r.no_echo ? 0 : r.dist_mm, t);
}

static void usage(void)
{
    fprintf(stderr, "usage: sampler [-D device] [-d datadir] [-p]   (-p: print readings, don't send)\n");
}

int main(int argc, char **argv)
{
    const char *dev = CFG_SERIAL_DEV;
    char dir[PATH_MAX] = "";
    struct sampler sp = {.sock = -1, .pid = (uint32_t)getpid()};
    int opt;
    while ((opt = getopt(argc, argv, "D:d:p")) != -1) {
        switch (opt) {
        case 'D': dev = optarg; break;
        case 'd': snprintf(dir, sizeof dir, "%s", optarg); break;
        case 'p': sp.print_only = 1; break;
        default: usage(); return EXIT_USAGE;
        }
    }
    if (!dir[0] && default_data_dir(dir, sizeof dir) < 0) {
        fprintf(stderr, "sampler: $HOME unset; pass -d datadir\n");
        return EXIT_USAGE;
    }

    struct sigaction sa = {.sa_handler = on_signal}; /* no SA_RESTART: poll returns EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (!sp.print_only) {
        sp.hub.sun_family = AF_UNIX;
        int n = snprintf(sp.hub.sun_path, sizeof sp.hub.sun_path, "%s/%s", dir, CFG_SOCK_FILE);
        if (n < 0 || (size_t)n >= sizeof sp.hub.sun_path) {
            fprintf(stderr, "sampler: socket path too long for sun_path\n");
            return EXIT_USAGE;
        }
        sp.sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (sp.sock < 0) {
            perror("sampler: socket");
            return EXIT_SYSERR;
        }
    }

    int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "sampler: open %s: %s\n", dev, strerror(errno));
        return EXIT_DEVICE;
    }
    if (setup_tty(fd) < 0) {
        fprintf(stderr, "sampler: termios %s: %s\n", dev, strerror(errno));
        return EXIT_DEVICE;
    }
    fprintf(stderr, "sampler: pid %u reading %s\n", sp.pid, dev);

    char line[CFG_LINE_MAX];
    size_t len = 0;
    int discarding = 1; /* first line after open may be cut by the Uno reset */
    uint64_t last_valid = mono_ns();
    uint64_t timeout_ns = (uint64_t)CFG_FIRST_FRAME_TIMEOUT_MS * 1000000u;
    int rc = EXIT_OK;

    while (!stop) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0 && errno != EINTR) {
            perror("sampler: poll");
            rc = EXIT_SYSERR;
            break;
        }
        if (pr > 0) {
            char buf[256];
            ssize_t n = read(fd, buf, sizeof buf);
            if (n == 0) {
                fprintf(stderr, "sampler: EOF on %s (unplugged?)\n", dev);
                rc = EXIT_DEVICE;
                break;
            }
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                fprintf(stderr, "sampler: read %s: %s\n", dev, strerror(errno));
                rc = EXIT_DEVICE;
                break;
            }
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c != '\n') {
                    if (len < sizeof line)
                        line[len++] = c;
                    else
                        discarding = 1; /* overlong: drop through the next newline */
                    continue;
                }
                uint64_t t = mono_ns();
                if (len > 0 && line[len - 1] == '\r')
                    len--;
                if (discarding) {
                    discarding = 0;
                } else {
                    unsigned long bad_before = sp.bad_frames;
                    handle_line(&sp, line, len, t);
                    if (sp.bad_frames == bad_before) {
                        last_valid = t;
                        timeout_ns = (uint64_t)CFG_FRAME_TIMEOUT_MS * 1000000u;
                    }
                }
                len = 0;
            }
        }
        if (mono_ns() - last_valid > timeout_ns) {
            fprintf(stderr, "sampler: no valid frame for %llu ms\n",
                    (unsigned long long)(timeout_ns / 1000000u));
            rc = EXIT_DEVICE;
            break;
        }
    }

    fprintf(stderr, "sampler: exit %d (sent=%lu send_drops=%lu bad_frames=%lu)\n", rc, sp.sent,
            sp.send_drops, sp.bad_frames);
    close(fd);
    if (sp.sock >= 0)
        close(sp.sock);
    return rc;
}
