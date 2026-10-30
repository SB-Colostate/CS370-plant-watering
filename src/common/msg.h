/* Sampler -> hub datagram (AF_UNIX SOCK_DGRAM). Same host and compiler on both ends, so the
 * struct is sent raw; the hub still checks length, magic, version, type and zero padding. */
#ifndef PLANT_MSG_H
#define PLANT_MSG_H

#include <stdint.h>

#define MSG_MAGIC   0x4D50u /* "PM" */
#define MSG_VERSION 1
#define MSG_SAMPLE  1

#define MSG_F_NO_ECHO 0x02u /* same bit as REC_F_NO_ECHO */

struct plant_msg {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t sampler_pid; /* instance id: sampler_seq restarts with each pid */
    uint32_t sampler_seq; /* per-instance counter so the hub can detect drops */
    uint8_t sensor_id;    /* SENSOR_SOIL / SENSOR_LEVEL */
    uint8_t flags;
    uint16_t pad0;        /* must be 0 */
    uint64_t mono_ns;     /* CLOCK_MONOTONIC when the line was completed */
    int32_t value;        /* soil raw 0..1023, or level in mm */
    uint32_t pad1;        /* must be 0 */
};

_Static_assert(sizeof(struct plant_msg) == 32, "plant_msg layout changed");

#endif
