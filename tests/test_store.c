/* Step 3: store open/recover/append/sync. Corruption is injected with raw file ops,
 * which simulates torn writes; it is not a power-loss trial (DESIGN §6 #2). */
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/config.h"
#include "common/exitcodes.h"
#include "store/store.h"
#include "unit.h"

static char dir[] = "/tmp/plant-test-XXXXXX";
static char logpath[PATH_MAX];

static uint64_t fake_ms;
static uint64_t fake_clock(void) { return fake_ms; }

static void reset_dir(void)
{
    unlink(logpath);
    char lock[PATH_MAX];
    snprintf(lock, sizeof lock, "%s/%s", dir, CFG_LOCK_FILE);
    unlink(lock);
}

static off_t file_size(void)
{
    struct stat st;
    return stat(logpath, &st) == 0 ? st.st_size : -1;
}

static struct record soil(int32_t v)
{
    return (struct record){.sensor_id = SENSOR_SOIL, .mono_ns = 1000u + (uint64_t)v, .wall_s = 1, .value = v};
}

/* Fresh log with START + n soil records (values 0..n-1), closed. Returns total records. */
static uint32_t make_log(int n)
{
    struct store s = {0};
    struct store_recovery rec;
    reset_dir();
    CHECK(store_open(&s, dir, &rec) == 0);
    for (int i = 0; i < n; i++) {
        struct record r = soil(i);
        CHECK(store_append(&s, &r, 0) == 0);
    }
    uint32_t total = s.next_seq;
    CHECK(store_close(&s) == 0);
    return total;
}

static void raw_write(off_t off, const void *buf, size_t n)
{
    int fd = open(logpath, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, buf, n, off) == (ssize_t)n);
    close(fd);
}

/* Every record i in the file decodes and has seq i. */
static void check_valid_prefix(uint32_t expect_count)
{
    struct store_reader rd;
    CHECK(store_reader_open(&rd, dir) == 0);
    CHECK(rd.count == expect_count);
    CHECK(file_size() == (off_t)expect_count * REC_SIZE);
    for (uint32_t i = 0; i < rd.count; i++) {
        struct record r;
        CHECK(store_reader_get(&rd, i, &r) == 0 && r.seq == i);
    }
    store_reader_close(&rd);
}

static void get(uint32_t i, struct record *r)
{
    struct store_reader rd;
    CHECK(store_reader_open(&rd, dir) == 0);
    CHECK(store_reader_get(&rd, i, r) == 0);
    store_reader_close(&rd);
}

static void test_fresh_open_writes_start(void)
{
    struct store s = {0};
    struct store_recovery rec;
    reset_dir();
    CHECK(store_open(&s, dir, &rec) == 0);
    CHECK(rec.created == 1 && rec.records == 0 && rec.dropped_bytes == 0);
    CHECK(s.next_seq == 1);
    CHECK(s.unsynced == 0); /* START is durable */
    CHECK(store_close(&s) == 0);

    struct record r;
    get(0, &r);
    CHECK(r.sensor_id == SENSOR_SYSTEM && REC_EVENT_KIND(r.flags) == REC_EV_START);
}

static void test_clean_reopen_keeps_everything(void)
{
    uint32_t total = make_log(10); /* START + 10 */
    CHECK(total == 11);
    struct store s = {0};
    struct store_recovery rec;
    CHECK(store_open(&s, dir, &rec) == 0);
    CHECK(rec.created == 0 && rec.records == 11 && rec.dropped_bytes == 0);
    CHECK(s.next_seq == 12); /* + new START, no RECOVERED */
    store_close(&s);
    check_valid_prefix(12);

    struct record r;
    get(5, &r);
    CHECK(r.sensor_id == SENSOR_SOIL && r.value == 4);
}

static void expect_recovery(uint32_t valid, uint64_t dropped)
{
    struct store s = {0};
    struct store_recovery rec;
    CHECK(store_open(&s, dir, &rec) == 0);
    CHECK(rec.records == valid);
    CHECK(rec.dropped_bytes == dropped);
    store_close(&s);
    check_valid_prefix(valid + 2); /* + RECOVERED + START */

    struct record r;
    get(valid, &r);
    CHECK(REC_EVENT_KIND(r.flags) == REC_EV_RECOVERED && r.value == (int32_t)dropped);
}

static void test_truncated_mid_record(void)
{
    uint32_t total = make_log(10);
    CHECK(truncate(logpath, (off_t)total * REC_SIZE - 5) == 0);
    expect_recovery(total - 1, REC_SIZE - 5);
}

static void test_flipped_crc_in_middle_cuts_there(void)
{
    uint32_t total = make_log(10);
    uint8_t x = 0xFF;
    raw_write(5 * REC_SIZE + 28, &x, 1); /* CRC byte of record 5 */
    expect_recovery(5, (uint64_t)(total - 5) * REC_SIZE);
}

static void test_zero_filled_tail(void)
{
    uint32_t total = make_log(10);
    static const uint8_t zeros[4096];
    raw_write((off_t)total * REC_SIZE, zeros, sizeof zeros);
    expect_recovery(total, sizeof zeros);
}

static void test_valid_crc_wrong_seq(void)
{
    uint32_t total = make_log(10);
    struct record r = soil(7);
    r.seq = 999; /* stale block from elsewhere: CRC fine, seq wrong */
    uint8_t b[REC_SIZE];
    record_encode(&r, b);
    raw_write((off_t)total * REC_SIZE, b, REC_SIZE);
    expect_recovery(total, REC_SIZE);
}

static void test_reader_ignores_torn_tail_and_never_writes(void)
{
    uint32_t total = make_log(3);
    raw_write((off_t)total * REC_SIZE, "garbage!!", 9);
    off_t before = file_size();
    struct store_reader rd;
    CHECK(store_reader_open(&rd, dir) == 0);
    CHECK(rd.count == total);
    store_reader_close(&rd);
    CHECK(file_size() == before);
}

static void test_second_writer_refused(void)
{
    struct store a = {0}, b = {0};
    struct store_recovery rec;
    reset_dir();
    CHECK(store_open(&a, dir, &rec) == 0);
    CHECK(store_locked(dir) == 1);
    CHECK(store_open(&b, dir, &rec) == EXIT_USAGE);
    CHECK(b.fd == -1 && b.lock_fd == -1);
    store_close(&a);
    CHECK(store_locked(dir) == 0);
    CHECK(store_open(&b, dir, &rec) == 0);
    store_close(&b);
}

static void test_sync_triggers(void)
{
    struct store s = {.now_ms = fake_clock};
    struct store_recovery rec;
    reset_dir();
    fake_ms = 1000;
    CHECK(store_open(&s, dir, &rec) == 0);
    uint64_t base = s.syncs;
    CHECK(store_sync_due_ms(&s) == -1);

    /* Count trigger: the 50th unsynced record forces a sync. */
    for (int i = 0; i < CFG_SYNC_MAX_RECORDS - 1; i++) {
        struct record r = soil(i);
        store_append(&s, &r, 0);
    }
    CHECK(s.unsynced == CFG_SYNC_MAX_RECORDS - 1 && s.syncs == base);
    struct record r = soil(0);
    CHECK(store_append(&s, &r, 0) == 0);
    CHECK(s.unsynced == 0 && s.syncs == base + 1);

    /* Age trigger: fires at exactly CFG_SYNC_MAX_AGE_MS after the oldest unsynced record. */
    CHECK(store_append(&s, &r, 0) == 0);
    CHECK(store_sync_due_ms(&s) == CFG_SYNC_MAX_AGE_MS);
    fake_ms += CFG_SYNC_MAX_AGE_MS - 1;
    CHECK(store_sync_due_ms(&s) == 1);
    CHECK(store_maybe_sync(&s) == 0 && s.unsynced == 1);
    fake_ms += 1;
    CHECK(store_sync_due_ms(&s) == 0);
    CHECK(store_maybe_sync(&s) == 0 && s.unsynced == 0);

    /* Durable append syncs immediately. */
    CHECK(store_append(&s, &r, STORE_DURABLE) == 0);
    CHECK(s.unsynced == 0);
    store_close(&s);
}

static void test_short_write_leaves_valid_prefix(void)
{
    struct store s = {0};
    struct store_recovery rec;
    reset_dir();
    CHECK(store_open(&s, dir, &rec) == 0);
    off_t size = s.size;

    /* Allow only half a record more: write() comes back short. */
    struct rlimit old, lim;
    getrlimit(RLIMIT_FSIZE, &old);
    lim = old;
    lim.rlim_cur = (rlim_t)size + REC_SIZE / 2;
    signal(SIGXFSZ, SIG_IGN);
    CHECK(setrlimit(RLIMIT_FSIZE, &lim) == 0);
    struct record r = soil(1);
    CHECK(store_append(&s, &r, 0) == EXIT_SYSERR);
    CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
    signal(SIGXFSZ, SIG_DFL);

    CHECK(s.next_seq == 1 && s.size == size);
    CHECK(file_size() == size);
    /* The store keeps working once space is back. */
    CHECK(store_append(&s, &r, 0) == 0 && r.seq == 1);
    store_close(&s);
    check_valid_prefix(2);
}

int main(void)
{
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(logpath, sizeof logpath, "%s/%s", dir, CFG_LOG_FILE);

    RUN(test_fresh_open_writes_start);
    RUN(test_clean_reopen_keeps_everything);
    RUN(test_truncated_mid_record);
    RUN(test_flipped_crc_in_middle_cuts_there);
    RUN(test_zero_filled_tail);
    RUN(test_valid_crc_wrong_seq);
    RUN(test_reader_ignores_torn_tail_and_never_writes);
    RUN(test_second_writer_refused);
    RUN(test_sync_triggers);
    RUN(test_short_write_leaves_valid_prefix);

    reset_dir();
    rmdir(dir);
    return unit_done("test_store");
}
