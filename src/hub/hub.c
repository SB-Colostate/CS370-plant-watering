/* hub: receives sampler datagrams and appends them to the log (mechanism D).
 * Recovery runs in store_open at startup; fsync follows the store's batch policy. */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "common/config.h"
#include "common/exitcodes.h"
#include "common/msg.h"
#include "common/paths.h"
#include "store/store.h"

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int msg_valid(const struct plant_msg *m)
{
    return m->magic == MSG_MAGIC && m->version == MSG_VERSION && m->type == MSG_SAMPLE &&
           m->pad0 == 0 && m->pad1 == 0 &&
           (m->sensor_id == SENSOR_SOIL || m->sensor_id == SENSOR_LEVEL) &&
           (m->flags & ~MSG_F_NO_ECHO) == 0;
}

static int append_event(struct store *s, unsigned kind, int32_t value)
{
    struct record r = {
        .sensor_id = SENSOR_SYSTEM,
        .flags = REC_EVENT_FLAGS(kind),
        .mono_ns = mono_ns(),
        .wall_s = (int64_t)time(NULL),
        .value = value,
    };
    return store_append(s, &r, 0);
}

static int bind_socket(const char *path)
{
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    int n = snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (n < 0 || (size_t)n >= sizeof a.sun_path) {
        fprintf(stderr, "hub: socket path too long for sun_path: %s\n", path);
        return -1;
    }
    /* Remove a stale socket from a previous run, but never some other kind of file. */
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "hub: %s exists and is not a socket\n", path);
            return -1;
        }
        unlink(path);
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("hub: socket");
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "hub: bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    char dir[PATH_MAX] = "", sock_path[PATH_MAX];
    int opt;
    while ((opt = getopt(argc, argv, "d:")) != -1) {
        switch (opt) {
        case 'd': snprintf(dir, sizeof dir, "%s", optarg); break;
        default: fprintf(stderr, "usage: hub [-d datadir]\n"); return EXIT_USAGE;
        }
    }
    if (!dir[0] && default_data_dir(dir, sizeof dir) < 0) {
        fprintf(stderr, "hub: $HOME unset; pass -d datadir\n");
        return EXIT_USAGE;
    }
    if (snprintf(sock_path, sizeof sock_path, "%s/%s", dir, CFG_SOCK_FILE) >= (int)sizeof sock_path)
        return EXIT_USAGE;

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct store s = {0};
    struct store_recovery rec;
    int rc = store_open(&s, dir, &rec);
    if (rc != 0)
        return rc;
    fprintf(stderr, "hub: log %s/%s: %u valid records, dropped %llu bytes%s; next seq %u\n", dir,
            CFG_LOG_FILE, rec.records, (unsigned long long)rec.dropped_bytes,
            rec.created ? " (new file)" : "", s.next_seq);

    int sock = bind_socket(sock_path);
    if (sock < 0) {
        store_close(&s);
        return EXIT_SYSERR;
    }

    uint32_t cur_pid = 0, expect_seq = 0;
    unsigned long received = 0, rejected = 0;
    rc = EXIT_OK;
    while (!stop && rc == EXIT_OK) {
        int due = store_sync_due_ms(&s);
        int timeout = (due < 0 || due > 1000) ? 1000 : due;
        struct pollfd pfd = {.fd = sock, .events = POLLIN};
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0 && errno != EINTR) {
            perror("hub: poll");
            rc = EXIT_SYSERR;
            break;
        }
        for (;;) {
            struct plant_msg m;
            ssize_t n = recv(sock, &m, sizeof m, MSG_DONTWAIT | MSG_TRUNC);
            if (n < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    perror("hub: recv");
                    rc = EXIT_SYSERR;
                }
                break;
            }
            if (n != (ssize_t)sizeof m || !msg_valid(&m)) {
                if (rejected++ % 60 == 0)
                    fprintf(stderr, "hub: rejected datagram (len %zd), rejected=%lu\n", n, rejected);
                continue;
            }
            received++;
            if (m.sampler_pid != cur_pid) {
                cur_pid = m.sampler_pid;
                expect_seq = m.sampler_seq;
                rc = append_event(&s, REC_EV_SAMPLER_RESTART, (int32_t)cur_pid);
            } else if (m.sampler_seq != expect_seq) {
                uint32_t missed = m.sampler_seq - expect_seq;
                fprintf(stderr, "hub: sampler %u gap: %u datagrams missed\n", cur_pid, missed);
                rc = append_event(&s, REC_EV_GAP, (int32_t)missed);
            }
            expect_seq = m.sampler_seq + 1;
            if (rc != EXIT_OK)
                break;
            struct record r = {
                .sensor_id = m.sensor_id,
                .flags = (uint8_t)(m.flags & MSG_F_NO_ECHO),
                .mono_ns = m.mono_ns,
                .wall_s = (int64_t)time(NULL),
                .value = m.value,
            };
            if ((rc = store_append(&s, &r, 0)) != EXIT_OK)
                break;
        }
        if (rc == EXIT_OK)
            rc = store_maybe_sync(&s);
    }

    int crc = store_close(&s);
    close(sock);
    unlink(sock_path);
    fprintf(stderr, "hub: exit %d (received=%lu rejected=%lu syncs=%llu max_sync_us=%llu)\n",
            rc ? rc : crc, received, rejected, (unsigned long long)s.syncs,
            (unsigned long long)s.max_sync_us);
    return rc ? rc : crc;
}
