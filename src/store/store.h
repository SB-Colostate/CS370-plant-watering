/* Crash-consistent append-only log (mechanism D).
 *
 * One file, <dir>/plant.plog, of 32-byte records (record.h). Record i has seq i.
 * Invariants:
 *   - After store_open, the file is a valid prefix: every record passes magic, CRC and seq == index.
 *   - store_append write()s the whole record before returning, so a process crash loses nothing
 *     that was appended; power loss loses at most the unsynced window.
 *   - Unsynced window: < CFG_SYNC_MAX_RECORDS records and < CFG_SYNC_MAX_AGE_MS, provided the caller
 *     calls store_maybe_sync() by store_sync_due_ms().
 *   - One writer: store_open holds an exclusive flock on <dir>/LOCK. Readers never write.
 */
#ifndef PLANT_STORE_H
#define PLANT_STORE_H

#include <stdint.h>
#include <sys/types.h>

#include "store/record.h"

#define STORE_DURABLE 0x1u /* fdatasync before store_append returns */

struct store {
    int fd;
    int lock_fd;
    uint32_t next_seq;
    off_t size;                  /* bytes of valid records == next_seq * REC_SIZE */
    uint32_t unsynced;           /* records written but not yet fdatasync'd */
    uint64_t oldest_unsynced_ms; /* now_ms() when the first unsynced record was written */
    uint64_t (*now_ms)(void);    /* injectable clock; NULL means CLOCK_MONOTONIC */
    /* stats, for plantctl/heartbeat and DESIGN §6 #1 */
    uint64_t syncs;
    uint64_t last_sync_us;
    uint64_t max_sync_us;
};

struct store_recovery {
    uint32_t records;       /* valid records found before any were appended */
    uint64_t dropped_bytes; /* torn/invalid tail truncated away */
    int created;            /* log file did not exist and was created */
};

/* s must be zero-initialized (set s->now_ms first to inject a clock).
 * Creates dir if missing (one level), takes the lock, recovers, then appends a durable
 * RECOVERED event (if anything was dropped) and a durable START event.
 * Returns 0, EXIT_USAGE (lock held / path too long) or EXIT_SYSERR. */
int store_open(struct store *s, const char *dir, struct store_recovery *rec);

/* Assigns r->seq. Returns 0 or EXIT_SYSERR (file left at its previous valid size). */
int store_append(struct store *s, struct record *r, unsigned flags);

int store_sync(struct store *s);
/* Syncs if the record-count or age trigger has fired. */
int store_maybe_sync(struct store *s);
/* Milliseconds until the age trigger fires; -1 if nothing is unsynced. For poll() timeouts. */
int store_sync_due_ms(const struct store *s);
/* Syncs and releases everything. Returns the sync result. */
int store_close(struct store *s);

/* Read-only view of the valid prefix. Never modifies the file. */
struct store_reader {
    int fd;
    uint32_t count;
    off_t file_size; /* > count * REC_SIZE means a torn tail the writer will cut on next open */
};

int store_reader_open(struct store_reader *rd, const char *dir);
int store_reader_get(const struct store_reader *rd, uint32_t i, struct record *r);
void store_reader_close(struct store_reader *rd);

/* 1 if a writer holds <dir>/LOCK, 0 if not, -1 on error. */
int store_locked(const char *dir);

#endif
