#include "store/store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/config.h"
#include "common/exitcodes.h"

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ms(const struct store *s)
{
    return s->now_ms ? s->now_ms() : mono_ms();
}

static int join(char *out, size_t n, const char *dir, const char *name)
{
    int len = snprintf(out, n, "%s/%s", dir, name);
    return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

/* Counts the valid prefix: magic, CRC, and seq == index. Stops at the first bad record. */
static int scan_valid(int fd, uint32_t *count, off_t *file_size)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;
    *file_size = st.st_size;

    uint8_t buf[REC_SIZE * 128];
    uint32_t n = 0;
    off_t off = 0;
    for (;;) {
        ssize_t got = pread(fd, buf, sizeof buf, off);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        size_t whole = (size_t)got / REC_SIZE;
        for (size_t i = 0; i < whole; i++) {
            struct record r;
            if (record_decode(buf + i * REC_SIZE, &r) != REC_OK || r.seq != n)
                goto done;
            n++;
        }
        if ((size_t)got < sizeof buf)
            break; /* EOF; any partial trailing record is invalid */
        off += got;
    }
done:
    *count = n;
    return 0;
}

static int fsync_dir(const char *dir)
{
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return -1;
    int rc = fsync(dfd);
    close(dfd);
    return rc;
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
    return store_append(s, &r, STORE_DURABLE);
}

int store_open(struct store *s, const char *dir, struct store_recovery *rec)
{
    char path[PATH_MAX];
    s->fd = -1;
    s->lock_fd = -1;
    memset(rec, 0, sizeof *rec);

    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "store: mkdir %s: %s\n", dir, strerror(errno));
        return EXIT_SYSERR;
    }

    if (join(path, sizeof path, dir, CFG_LOCK_FILE) < 0)
        return EXIT_USAGE;
    s->lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (s->lock_fd < 0) {
        fprintf(stderr, "store: open %s: %s\n", path, strerror(errno));
        return EXIT_SYSERR;
    }
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        int busy = (errno == EWOULDBLOCK);
        fprintf(stderr, "store: lock %s: %s\n", path, busy ? "held by another writer" : strerror(errno));
        close(s->lock_fd);
        s->lock_fd = -1;
        return busy ? EXIT_USAGE : EXIT_SYSERR;
    }

    if (join(path, sizeof path, dir, CFG_LOG_FILE) < 0)
        goto fail_usage;
    s->fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
    if (s->fd >= 0) {
        rec->created = 1;
        if (fsync_dir(dir) < 0) {
            fprintf(stderr, "store: fsync dir %s: %s\n", dir, strerror(errno));
            goto fail_sys;
        }
    } else if (errno == EEXIST) {
        s->fd = open(path, O_RDWR | O_APPEND | O_CLOEXEC);
    }
    if (s->fd < 0) {
        fprintf(stderr, "store: open %s: %s\n", path, strerror(errno));
        goto fail_sys;
    }

    uint32_t count;
    off_t file_size;
    if (scan_valid(s->fd, &count, &file_size) < 0) {
        fprintf(stderr, "store: scan %s: %s\n", path, strerror(errno));
        goto fail_sys;
    }
    off_t valid_end = (off_t)count * REC_SIZE;
    rec->records = count;
    rec->dropped_bytes = (uint64_t)(file_size - valid_end);
    if (valid_end < file_size) {
        if (ftruncate(s->fd, valid_end) < 0 || fdatasync(s->fd) < 0) {
            fprintf(stderr, "store: truncate %s: %s\n", path, strerror(errno));
            goto fail_sys;
        }
    }
    s->next_seq = count;
    s->size = valid_end;
    s->unsynced = 0;

    if (rec->dropped_bytes > 0) {
        int32_t v = rec->dropped_bytes > INT32_MAX ? INT32_MAX : (int32_t)rec->dropped_bytes;
        if (append_event(s, REC_EV_RECOVERED, v) != 0)
            goto fail_sys;
    }
    if (append_event(s, REC_EV_START, 0) != 0)
        goto fail_sys;
    return 0;

fail_usage:
    store_close(s);
    return EXIT_USAGE;
fail_sys:
    store_close(s);
    return EXIT_SYSERR;
}

int store_append(struct store *s, struct record *r, unsigned flags)
{
    uint8_t buf[REC_SIZE];
    r->seq = s->next_seq;
    record_encode(r, buf);

    ssize_t n;
    do {
        n = write(s->fd, buf, REC_SIZE);
    } while (n < 0 && errno == EINTR);
    if (n != REC_SIZE) {
        int err = n < 0 ? errno : ENOSPC;
        fprintf(stderr, "store: append seq=%u: %s\n", r->seq, n < 0 ? strerror(err) : "short write");
        /* A partial record would break the prefix; cut back to the last valid byte. */
        if (n > 0 && ftruncate(s->fd, s->size) < 0)
            fprintf(stderr, "store: truncate after short write: %s\n", strerror(errno));
        errno = err;
        return EXIT_SYSERR;
    }
    s->size += REC_SIZE;
    s->next_seq++;
    if (s->unsynced++ == 0)
        s->oldest_unsynced_ms = now_ms(s);

    if (flags & STORE_DURABLE)
        return store_sync(s);
    return store_maybe_sync(s);
}

int store_sync(struct store *s)
{
    if (s->unsynced == 0)
        return 0;
    uint64_t t0 = mono_ns();
    if (fdatasync(s->fd) < 0) {
        fprintf(stderr, "store: fdatasync: %s\n", strerror(errno));
        return EXIT_SYSERR;
    }
    uint64_t us = (mono_ns() - t0) / 1000u;
    s->syncs++;
    s->last_sync_us = us;
    if (us > s->max_sync_us)
        s->max_sync_us = us;
    s->unsynced = 0;
    return 0;
}

int store_maybe_sync(struct store *s)
{
    if (s->unsynced == 0)
        return 0;
    if (s->unsynced >= CFG_SYNC_MAX_RECORDS ||
        now_ms(s) - s->oldest_unsynced_ms >= CFG_SYNC_MAX_AGE_MS)
        return store_sync(s);
    return 0;
}

int store_sync_due_ms(const struct store *s)
{
    if (s->unsynced == 0)
        return -1;
    uint64_t age = now_ms(s) - s->oldest_unsynced_ms;
    return age >= CFG_SYNC_MAX_AGE_MS ? 0 : (int)(CFG_SYNC_MAX_AGE_MS - age);
}

int store_close(struct store *s)
{
    int rc = 0;
    if (s->fd >= 0) {
        rc = store_sync(s);
        close(s->fd);
        s->fd = -1;
    }
    if (s->lock_fd >= 0) {
        close(s->lock_fd); /* releases the flock */
        s->lock_fd = -1;
    }
    return rc;
}

int store_reader_open(struct store_reader *rd, const char *dir)
{
    char path[PATH_MAX];
    rd->fd = -1;
    rd->count = 0;
    if (join(path, sizeof path, dir, CFG_LOG_FILE) < 0)
        return EXIT_USAGE;
    rd->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (rd->fd < 0) {
        fprintf(stderr, "store: open %s: %s\n", path, strerror(errno));
        return EXIT_SYSERR;
    }
    off_t size;
    if (scan_valid(rd->fd, &rd->count, &size) < 0) {
        fprintf(stderr, "store: scan %s: %s\n", path, strerror(errno));
        store_reader_close(rd);
        return EXIT_SYSERR;
    }
    return 0;
}

int store_reader_get(const struct store_reader *rd, uint32_t i, struct record *r)
{
    uint8_t buf[REC_SIZE];
    if (i >= rd->count)
        return EXIT_USAGE;
    if (pread(rd->fd, buf, REC_SIZE, (off_t)i * REC_SIZE) != REC_SIZE)
        return EXIT_SYSERR;
    /* Revalidate: the scan said this was good, but the file is not ours. */
    if (record_decode(buf, r) != REC_OK || r->seq != i)
        return EXIT_CORRUPT;
    return 0;
}

void store_reader_close(struct store_reader *rd)
{
    if (rd->fd >= 0)
        close(rd->fd);
    rd->fd = -1;
}

int store_locked(const char *dir)
{
    char path[PATH_MAX];
    if (join(path, sizeof path, dir, CFG_LOCK_FILE) < 0)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    int rc;
    if (flock(fd, LOCK_SH | LOCK_NB) == 0)
        rc = 0; /* we got it, so no writer holds it; closing releases ours */
    else
        rc = errno == EWOULDBLOCK ? 1 : -1;
    close(fd);
    return rc;
}
