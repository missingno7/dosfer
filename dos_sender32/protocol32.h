#ifndef DOSFER32_PROTOCOL_H
#define DOSFER32_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define DOS32_FRAME_HEADER 48u
#define DOS32_RECORD_HEADER 24u
#define DOS32_FRAME_PAYLOAD 2904u
#define DOS32_FRAME_BYTES (DOS32_FRAME_HEADER + DOS32_FRAME_PAYLOAD)
#define DOS32_RECORD_BYTES DOS32_FRAME_PAYLOAD

enum {
    DOS32_DATA = 1, DOS32_END_WINDOW = 2, DOS32_PLANE_CODED = 6,
    DOS32_SESSION = 1, DOS32_DIRECTORY = 2, DOS32_FILE_BEGIN = 3,
    DOS32_FILE_DATA = 4, DOS32_FILE_END = 5, DOS32_TRANSFER_END = 6
};
enum { DOS32_FLAG_WHITENED = 0x0008, DOS32_FLAG_PLANE_WHITENED = 0x0010 };

uint32_t dos32_crc(const void *data, size_t length);
uint32_t dos32_crc_start(void);
uint32_t dos32_crc_update(uint32_t state, const void *data, size_t length);
uint32_t dos32_crc_finish(uint32_t state);
uint16_t dos32_record(uint8_t *out, uint8_t type, uint32_t record_id,
                      uint32_t file_id, const uint8_t *body, uint16_t length);
uint16_t dos32_frame(uint8_t *out, uint8_t kind, uint16_t flags,
                     uint32_t session, uint32_t window, uint32_t global,
                     uint16_t window_index, uint16_t window_count,
                     uint32_t stream_id, uint32_t stream_offset,
                     const uint8_t *payload, uint16_t payload_length);
uint16_t dos32_plane_frame(uint8_t *out, uint32_t session, uint32_t window,
                           uint32_t group_global, uint16_t group_index,
                           uint16_t window_count, uint8_t width,
                           uint8_t coefficient, const uint8_t *payload);

#endif
