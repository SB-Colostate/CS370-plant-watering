#include "store/record.h"

#include <stdbool.h>

static uint32_t crc_table[256];
static bool crc_table_ready;

static void crc_table_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_table_ready = true;
}

uint32_t crc32_ieee(const void *buf, size_t len)
{
    if (!crc_table_ready)
        crc_table_init();
    const uint8_t *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

void record_encode(const struct record *r, uint8_t out[REC_SIZE])
{
    put_le(out + 0, REC_MAGIC, 2);
    out[2] = r->sensor_id;
    out[3] = r->flags;
    put_le(out + 4, r->seq, 4);
    put_le(out + 8, r->mono_ns, 8);
    put_le(out + 16, (uint64_t)r->wall_s, 8);
    put_le(out + 24, (uint32_t)r->value, 4);
    put_le(out + 28, crc32_ieee(out, 28), 4);
}

enum rec_status record_decode(const uint8_t in[REC_SIZE], struct record *r)
{
    if (get_le(in + 0, 2) != REC_MAGIC)
        return REC_BAD_MAGIC;
    if (get_le(in + 28, 4) != crc32_ieee(in, 28))
        return REC_BAD_CRC;
    r->sensor_id = in[2];
    r->flags = in[3];
    r->seq = (uint32_t)get_le(in + 4, 4);
    r->mono_ns = get_le(in + 8, 8);
    r->wall_s = (int64_t)get_le(in + 16, 8);
    r->value = (int32_t)(uint32_t)get_le(in + 24, 4);
    return REC_OK;
}
