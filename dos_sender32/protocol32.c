#include "protocol32.h"
#include <string.h>

/* Slicing-by-four CRC32.  The protected-mode sender runs on a little-endian
 * 386, where four-byte loads are legal even when the input is not aligned. */
static uint32_t crc_table[256];
static uint32_t crc_slice[3][256];
static int crc_ready;

static void crc_init(void) {
    unsigned i, bit;
    for (i = 0; i < 256u; ++i) {
        uint32_t c = i;
        for (bit = 0; bit < 8u; ++bit)
            c = (c & 1u) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    for (i = 0; i < 256u; ++i) {
        uint32_t c = crc_table[i];
        crc_slice[0][i] = (c >> 8) ^ crc_table[(uint8_t)c];
        crc_slice[1][i] = (crc_slice[0][i] >> 8) ^ crc_table[(uint8_t)crc_slice[0][i]];
        crc_slice[2][i] = (crc_slice[1][i] >> 8) ^ crc_table[(uint8_t)crc_slice[1][i]];
    }
    crc_ready = 1;
}

static uint32_t load32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t crc_word(uint32_t state, uint32_t value) {
    state ^= value;
    return crc_slice[2][(uint8_t)state] ^
           crc_slice[1][(uint8_t)(state >> 8)] ^
           crc_slice[0][(uint8_t)(state >> 16)] ^
           crc_table[(uint8_t)(state >> 24)];
}

uint32_t dos32_crc(const void *data, size_t length) {
    return dos32_crc_finish(dos32_crc_update(dos32_crc_start(), data, length));
}

uint32_t dos32_crc_start(void) {
    if (!crc_ready) crc_init();
    return 0xFFFFFFFFUL;
}

uint32_t dos32_crc_update(uint32_t state, const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    if (!crc_ready) crc_init();
    while (length >= 4u) {
        state = crc_word(state, load32le(p));
        p += 4; length -= 4;
    }
    while (length--)
        state = crc_table[(uint8_t)(state ^ *p++)] ^ (state >> 8);
    return state;
}

uint32_t dos32_crc_finish(uint32_t state) { return state ^ 0xFFFFFFFFUL; }

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t xorshift(uint32_t x) {
    x ^= x << 13; x ^= x >> 17; return x ^ (x << 5);
}

/* Copy and CRC in one pass.  If key is non-null, XOR the supplied whitening
 * stream while copying.  This replaces separate copy/whiten and CRC passes. */
static uint32_t copy_key_crc(uint8_t *dst, const uint8_t *src,
                             const uint8_t *key, size_t n) {
    uint32_t state = dos32_crc_start();
    while (n >= 4u) {
        uint32_t value = load32le(src);
        if (key) value ^= load32le(key);
        store32le(dst, value);
        state = crc_word(state, value);
        src += 4; dst += 4; if (key) key += 4; n -= 4;
    }
    while (n--) {
        uint8_t value = *src++;
        if (key) value ^= *key++;
        *dst++ = value;
        state = crc_table[(uint8_t)(state ^ value)] ^ (state >> 8);
    }
    return dos32_crc_finish(state);
}

/* Generate whitening, copy, and CRC in one pass for general DATA frames. */
static uint32_t whiten_copy_crc(uint8_t *dst, const uint8_t *src, size_t n,
                                uint32_t session, uint32_t index) {
    uint32_t state = session ^ (index * 0x9E3779B9UL) ^ 0xD05FE123UL;
    uint32_t crc = dos32_crc_start();
    if (!state) state = 0xA5A5A5A5UL;
    while (n >= 4u) {
        uint32_t value;
        state = xorshift(state);
        value = load32le(src) ^ state;
        store32le(dst, value);
        crc = crc_word(crc, value);
        src += 4; dst += 4; n -= 4;
    }
    if (n) {
        uint32_t key;
        state = xorshift(state); key = state;
        while (n--) {
            uint8_t value = (uint8_t)(*src++ ^ (uint8_t)key);
            *dst++ = value;
            crc = crc_table[(uint8_t)(crc ^ value)] ^ (crc >> 8);
            key >>= 8;
        }
    }
    return dos32_crc_finish(crc);
}

void dos32_generate_keystream(uint8_t *dst, uint32_t session, uint32_t index) {
    uint32_t state = session ^ (index * 0x9E3779B9UL) ^ 0xD05FE123UL;
    unsigned i;
    if (!state) state = 0xA5A5A5A5UL;
    for (i = 0; i < DOS32_FRAME_PAYLOAD; i += 4u) {
        state = xorshift(state);
        store32le(dst + i, state);
    }
}

static uint16_t frame_header(uint8_t *out, uint8_t kind, uint16_t flags,
        uint32_t session, uint32_t window, uint32_t global, uint16_t wi,
        uint16_t wc, uint32_t sid, uint32_t off, uint32_t payload_crc,
        uint16_t payload_length) {
    uint32_t header_crc;
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
    unsigned used;
    if (!out || (length && !body) ||
        length > DOS32_RECORD_BYTES - DOS32_RECORD_HEADER) return 0;
    memcpy(out, "DQRC", 4); out[4] = 1; out[5] = type; put16(out + 6, 0);
    put32(out + 8, record_id); put32(out + 12, file_id); put32(out + 16, length);
    put32(out + 20, dos32_crc(body, length));
    if (length) memcpy(out + DOS32_RECORD_HEADER, body, length);
    used = DOS32_RECORD_HEADER + length;
    if (used < DOS32_RECORD_BYTES)
        memset(out + used, 0, DOS32_RECORD_BYTES - used);
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
        pcrc = whiten_copy_crc(out + DOS32_FRAME_HEADER, payload, payload_length,
                              session, (flags & DOS32_FLAG_PLANE_WHITENED) ? stream_id : global);
    else
        pcrc = copy_key_crc(out + DOS32_FRAME_HEADER, payload, 0, payload_length);
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

uint16_t dos32_plane_frame_whitened(uint8_t *out, uint32_t session, uint32_t window,
                                    uint32_t group_global, uint16_t group_index,
                                    uint16_t window_count, uint8_t width,
                                    uint8_t coefficient, const uint8_t *payload,
                                    const uint8_t *keystream) {
    uint16_t flags = (width == 4 && coefficient == 0x0F) ? 0 : DOS32_FLAG_PLANE_WHITENED;
    uint32_t pcrc;
    if (!out || !payload || !keystream) return 0;
    pcrc = copy_key_crc(out + DOS32_FRAME_HEADER, payload,
                       (flags & DOS32_FLAG_PLANE_WHITENED) ? keystream : 0,
                       DOS32_FRAME_PAYLOAD);
    return frame_header(out, DOS32_PLANE_CODED, flags, session, window,
                        coefficient, group_index, window_count, group_global,
                        width, pcrc, DOS32_FRAME_PAYLOAD);
}
