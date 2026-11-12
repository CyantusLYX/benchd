/*
 * crc32.c — table-driven CRC32 (IEEE 802.3 polynomial, reflected 0xEDB88320).
 * Same algorithm as zlib's crc32(), so Wireshark/python's zlib.crc32 agree with us.
 */
#include "bench_proto.h"

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t bench_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    if (!crc_ready)
        crc_init();
    while (len--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
