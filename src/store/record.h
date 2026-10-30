/* On-disk log record (mechanism D). 32 bytes, little-endian, encoded field by field:
 *
 *   off size field      notes
 *    0   2   magic      REC_MAGIC
 *    2   1   sensor_id  SENSOR_*
 *    3   1   flags      REC_F_* in the low nibble, event kind (REC_EV_*) in the high nibble
 *    4   4   seq        global, +1 per record; record i of the log has seq i
 *    8   8   mono_ns    CLOCK_MONOTONIC when the reading was taken (only comparable within one boot)
 *   16   8   wall_s     CLOCK_REALTIME seconds; informational only (Pi 3 has no RTC)
 *   24   4   value      soil raw 0..1023, level in mm, or the event payload
 *   28   4   crc32      CRC-32/IEEE over bytes 0..27
 */
#ifndef PLANT_RECORD_H
#define PLANT_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define REC_SIZE  32
#define REC_MAGIC 0x4C50u /* "PL" on disk */

enum { SENSOR_SYSTEM = 0, SENSOR_SOIL = 1, SENSOR_LEVEL = 2 };

#define REC_F_EVENT   0x01u
#define REC_F_NO_ECHO 0x02u /* level reading had no echo; value is meaningless */

enum {
    REC_EV_START = 1,           /* hub started; value = 0 */
    REC_EV_RECOVERED = 2,       /* recovery truncated a torn tail; value = bytes dropped */
    REC_EV_GAP = 3,             /* sampler datagrams missing; value = count missed */
    REC_EV_SAMPLER_RESTART = 4, /* new sampler pid seen; value = new pid */
};

#define REC_EVENT_FLAGS(kind) ((uint8_t)(REC_F_EVENT | ((unsigned)(kind) << 4)))
#define REC_EVENT_KIND(flags) ((unsigned)(flags) >> 4)

struct record {
    uint8_t sensor_id;
    uint8_t flags;
    uint32_t seq;
    uint64_t mono_ns;
    int64_t wall_s;
    int32_t value;
};

enum rec_status { REC_OK = 0, REC_BAD_MAGIC, REC_BAD_CRC };

uint32_t crc32_ieee(const void *buf, size_t len);
void record_encode(const struct record *r, uint8_t out[REC_SIZE]);
/* Checks magic and CRC only; seq continuity is the store's job. */
enum rec_status record_decode(const uint8_t in[REC_SIZE], struct record *r);

#endif
