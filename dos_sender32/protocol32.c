#include "protocol32.h"
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
    unsigned i, bit;
    for (i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (bit = 0; bit < 8; ++bit) c = (c & 1u) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t dos32_crc(const void *data, size_t length) {
    return dos32_crc_finish(dos32_crc_update(dos32_crc_start(), data, length));
}

uint32_t dos32_crc_start(void) { if (!crc_ready) crc_init(); return 0xFFFFFFFFUL; }
uint32_t dos32_crc_update(uint32_t state, const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    if (!crc_ready) crc_init();
    while (length--) state = crc_table[(uint8_t)(state ^ *p++)] ^ (state >> 8);
    return state;
}
uint32_t dos32_crc_finish(uint32_t state) { return state ^ 0xFFFFFFFFUL; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t xorshift(uint32_t x) { x ^= x << 13; x ^= x >> 17; return x ^ (x << 5); }

static void whiten(uint8_t *dst, const uint8_t *src, size_t n,
                   uint32_t session, uint32_t index, int plane) {
    uint32_t state = session ^ (index * 0x9E3779B9UL) ^ 0xD05FE123UL;
    (void)plane;
    if (!state) state = 0xA5A5A5A5UL;
    while (n) {
        uint32_t key = xorshift(state); unsigned i;
        state = key;
        for (i = 0; i < 4 && n; ++i, --n) { *dst++ = (uint8_t)(*src++ ^ (uint8_t)key); key >>= 8; }
    }
}

static uint16_t frame_header(uint8_t *out, uint8_t kind, uint16_t flags,
        uint32_t session, uint32_t window, uint32_t global, uint16_t wi,
        uint16_t wc, uint32_t sid, uint32_t off, uint32_t payload_crc,
        uint16_t payload_length) {
    uint32_t header_crc;
    memset(out, 0, DOS32_FRAME_HEADER);
    memcpy(out, "DQR1", 4); out[4] = 1; out[5] = kind; put16(out + 6, flags);
    put32(out + 8, session); put32(out + 12, window); put32(out + 16, global);
    put16(out + 20, wi); put16(out + 22, wc); put32(out + 24, sid); put32(out + 28, off);
    put16(out + 32, payload_length); put16(out + 34, DOS32_FRAME_HEADER);
    put32(out + 36, payload_crc); put32(out + 40, 0); put32(out + 44, 0);
    header_crc = dos32_crc(out, DOS32_FRAME_HEADER); put32(out + 40, header_crc);
    return (uint16_t)(DOS32_FRAME_HEADER + payload_length);
}

uint16_t dos32_record(uint8_t *out, uint8_t type, uint32_t record_id,
                      uint32_t file_id, const uint8_t *body, uint16_t length) {
    if (!out || length > DOS32_RECORD_BYTES - DOS32_RECORD_HEADER) return 0;
    memset(out, 0, DOS32_RECORD_BYTES); memcpy(out, "DQRC", 4);
    out[4] = 1; out[5] = type; put16(out + 6, 0);
    put32(out + 8, record_id); put32(out + 12, file_id); put32(out + 16, length);
    put32(out + 20, dos32_crc(body, length));
    if (length) memcpy(out + DOS32_RECORD_HEADER, body, length);
    return DOS32_RECORD_BYTES;
}

uint16_t dos32_frame(uint8_t *out, uint8_t kind, uint16_t flags,
                     uint32_t session, uint32_t window, uint32_t global,
                     uint16_t window_index, uint16_t window_count,
                     uint32_t stream_id, uint32_t stream_offset,
                     const uint8_t *payload, uint16_t payload_length) {
    uint32_t pcrc;
    if (!out || !payload || payload_length > DOS32_FRAME_PAYLOAD) return 0;
    if (flags & (DOS32_FLAG_WHITENED | DOS32_FLAG_PLANE_WHITENED))
        whiten(out + DOS32_FRAME_HEADER, payload, payload_length, session,
               (flags & DOS32_FLAG_PLANE_WHITENED) ? stream_id : global,
               (flags & DOS32_FLAG_PLANE_WHITENED) != 0);
    else memcpy(out + DOS32_FRAME_HEADER, payload, payload_length);
    pcrc = dos32_crc(out + DOS32_FRAME_HEADER, payload_length);
    return frame_header(out, kind, flags, session, window, global, window_index,
                        window_count, stream_id, stream_offset, pcrc, payload_length);
}

uint16_t dos32_plane_frame(uint8_t *out, uint32_t session, uint32_t window,
                           uint32_t group_global, uint16_t group_index,
                           uint16_t window_count, uint8_t width,
                           uint8_t coefficient, const uint8_t *payload) {
    uint16_t flags = (width == 4 && coefficient == 0x0F) ? 0 : DOS32_FLAG_PLANE_WHITENED;
    return dos32_frame(out, DOS32_PLANE_CODED, flags, session, window,
        coefficient, group_index, window_count, group_global, width,
        payload, DOS32_FRAME_PAYLOAD);
}
