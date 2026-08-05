#include "protocol32.h"
#include <stdio.h>
#include <string.h>

static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t step(uint32_t x) { x ^= x << 13; x ^= x >> 17; return x ^ (x << 5); }
static void unwhiten(uint8_t *p, size_t n, uint32_t session, uint32_t group) {
    uint32_t s = session ^ (group * 0x9E3779B9UL) ^ 0xD05FE123UL;
    size_t i; if (!s) s = 0xA5A5A5A5UL;
    for (i = 0; i < n; ++i) { if ((i & 3u) == 0) s = step(s); p[i] ^= (uint8_t)(s >> ((i & 3u) * 8)); }
}
int main(void) {
    uint8_t a[DOS32_RECORD_BYTES], b[DOS32_RECORD_BYTES], frame[DOS32_FRAME_BYTES], plain[DOS32_FRAME_PAYLOAD];
    unsigned i; uint32_t session = 0x12345678UL, group = 0;
    for (i = 0; i < DOS32_RECORD_BYTES; ++i) { a[i] = (uint8_t)i; b[i] = (uint8_t)(i ^ 0xA5); plain[i] = a[i] ^ b[i]; }
    if (!dos32_record(a, DOS32_SESSION, 0, 0, (const uint8_t *)"x", 1)) return 1;
    if (!dos32_plane_frame(frame, session, 0, group, 0, 4, 4, 1, b)) return 2;
    if (frame[5] != DOS32_PLANE_CODED || get32(frame + 24) != group || get32(frame + 28) != 4) return 3;
    unwhiten(frame + DOS32_FRAME_HEADER, DOS32_FRAME_PAYLOAD, session, group);
    if (memcmp(frame + DOS32_FRAME_HEADER, b, DOS32_RECORD_BYTES) != 0) return 4;
    if (!dos32_plane_frame(frame, session, 0, group, 0, 4, 4, 15, plain)) return 5;
    if (frame[6] != 0 || get32(frame + 16) != 15 || get32(frame + 24) != group) return 6;
    puts("protocol32 self-test passed"); return 0;
}
