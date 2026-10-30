/* Step 2: record encode/decode and CRC-32. */
#include <string.h>

#include "store/record.h"
#include "unit.h"

static const struct record sample = {
    .sensor_id = SENSOR_LEVEL,
    .flags = REC_F_NO_ECHO,
    .seq = 0x01020304u,
    .mono_ns = 0x1122334455667788ull,
    .wall_s = -2, /* negative to prove sign survives the round trip */
    .value = -1,
};

static void test_crc_known_vectors(void)
{
    /* Standard check value for CRC-32/IEEE. */
    CHECK(crc32_ieee("123456789", 9) == 0xCBF43926u);
    CHECK(crc32_ieee("", 0) == 0x00000000u);
}

static void test_layout_is_little_endian(void)
{
    uint8_t b[REC_SIZE];
    record_encode(&sample, b);
    CHECK(b[0] == 0x50 && b[1] == 0x4C); /* "PL" */
    CHECK(b[2] == SENSOR_LEVEL);
    CHECK(b[3] == REC_F_NO_ECHO);
    CHECK(b[4] == 0x04 && b[7] == 0x01);
    CHECK(b[8] == 0x88 && b[15] == 0x11);
    CHECK(b[16] == 0xFE && b[23] == 0xFF);
    CHECK(b[24] == 0xFF && b[27] == 0xFF);
}

static void test_roundtrip(void)
{
    uint8_t b[REC_SIZE];
    struct record r;
    memset(&r, 0, sizeof r);
    record_encode(&sample, b);
    CHECK(record_decode(b, &r) == REC_OK);
    CHECK(r.sensor_id == sample.sensor_id);
    CHECK(r.flags == sample.flags);
    CHECK(r.seq == sample.seq);
    CHECK(r.mono_ns == sample.mono_ns);
    CHECK(r.wall_s == sample.wall_s);
    CHECK(r.value == sample.value);
}

static void test_event_flags(void)
{
    uint8_t f = REC_EVENT_FLAGS(REC_EV_RECOVERED);
    CHECK(f & REC_F_EVENT);
    CHECK(REC_EVENT_KIND(f) == REC_EV_RECOVERED);
}

static void test_every_bit_flip_detected(void)
{
    uint8_t good[REC_SIZE], b[REC_SIZE];
    struct record r;
    record_encode(&sample, good);
    int undetected = 0;
    for (int byte = 0; byte < REC_SIZE; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(b, good, REC_SIZE);
            b[byte] ^= (uint8_t)(1u << bit);
            if (record_decode(b, &r) == REC_OK)
                undetected++;
        }
    }
    CHECK(undetected == 0);
}

static void test_zeroed_record_rejected(void)
{
    /* What an unwritten (hole) region of the file looks like after power loss. */
    uint8_t b[REC_SIZE] = {0};
    struct record r;
    CHECK(record_decode(b, &r) == REC_BAD_MAGIC);
}

int main(void)
{
    RUN(test_crc_known_vectors);
    RUN(test_layout_is_little_endian);
    RUN(test_roundtrip);
    RUN(test_event_flags);
    RUN(test_every_bit_flip_detected);
    RUN(test_zeroed_record_rejected);
    return unit_done("test_record");
}
