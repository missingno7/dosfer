#ifndef DOSFER_H
#define DOSFER_H

#include <stdio.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

#define DQR_VERSION 1
#define FRAME_HEADER_SIZE 48
#define RECORD_HEADER_SIZE 24
#define MAX_FRAME_PAYLOAD 2905
#define MAX_QR_BYTES (FRAME_HEADER_SIZE + MAX_FRAME_PAYLOAD)
#define MAX_WINDOW 64
#define PATH_BYTES 128

enum { FK_DATA=1, FK_END_WINDOW=2, FK_CALIBRATION=3, FK_CHAIN_XOR=4,
       FK_BLOCK_XOR=5, FK_PLANE_CODED=6 };
enum { FF_REPEATED=0x0001, FF_PAIR_WHITENED=0x0004, FF_WHITENED=0x0008 };
enum { RT_SESSION=1, RT_DIRECTORY=2, RT_FILE_BEGIN=3,
       RT_FILE_DATA=4, RT_FILE_END=5, RT_TRANSFER_END=6 };

typedef struct {
    u8 qr_version;
    u8 ecc;              /* 0=L, 1=M, 2=Q, 3=H */
    u8 module_pixels;
    u8 repetitions;
    u16 frame_payload;
    u16 hold_ms;
    u16 window_frames;
    u8 invert;
    u8 speaker;
    u8 redundancy;       /* 0=none, otherwise DATA frames per block parity */
    u8 chain_width;      /* 0=block parity, otherwise even overlap width */
    u8 plane_width;      /* 0=off, 3=PLANE3, 4=PLANE4 */
} Config;

typedef struct {
    u8 kind;             /* 1 file, 2 directory */
    u8 attributes;
    u16 dos_date;
    u16 dos_time;
    u32 size;
    u32 file_id;
    char source[PATH_BYTES];
    char relative[PATH_BYTES];
} ManifestEntry;

typedef struct {
    u8 far *payload;
    u16 payload_capacity;
    u16 payload_len;
    u32 stream_id;
    u32 stream_offset;
    u32 global_index;
} PendingFrame;

typedef struct {
    PendingFrame frames[MAX_WINDOW];
    u16 count;
    u32 id;
} Window;

u32 crc32_update(u32 crc, const void *data, u16 len);
u32 crc32_bytes(const void *data, u16 len);
void put_u16(u8 *p, u16 v);
void put_u32(u8 *p, u32 v);
u16 make_record(u8 *out, u16 out_capacity, u8 type, u32 record_id,
                u32 file_id, const u8 *body, u16 body_len);
u16 make_frame(u8 *out, u8 kind, u16 flags, u32 session, u32 window,
               u32 global_index, u16 window_index, u16 window_count,
               u32 stream_id, u32 stream_offset, const u8 *payload,
               u16 payload_len);
u16 make_frame_header_crc(u8 *out, u8 kind, u16 flags, u32 session, u32 window,
               u32 global_index, u16 window_index, u16 window_count,
               u32 stream_id, u32 stream_offset, u32 payload_crc,
               u16 payload_len);
u16 make_plane_frame(u8 *out,u32 session,u32 window,u32 group_global,
                     u16 group_index,u16 window_count,u8 group_width,u8 coefficient,
                     const u8 far *payload,u16 payload_len);

#endif
