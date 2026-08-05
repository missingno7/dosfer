#include "platform_vga.h"
#include "protocol32.h"
#include "qrcodegen.h"
#include <conio.h>
#include <direct.h>
#include <dos.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DOSFER32_BUILD_ID "dosfer32-stream-r23-bulk-vga"
#define QR_SIZE 177u
#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(40)
#define QR_DATA_CODEWORDS 2956u
#define QR_CODEWORDS 3706u
#define SLOT_COUNT 8u
#define MAX_WINDOW32 128u
#define HOLD_DEFAULT 100u
#define DISK_RING_BYTES 65536u
#define DISK_RING_SLOTS 2u
#define DISK_SLOT_BYTES (DISK_RING_BYTES / DISK_RING_SLOTS)
#define QR_QUIET 4u
#define QR_X0 ((320u - (QR_SIZE + 2u * QR_QUIET)) / 2u + QR_QUIET)

enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_PLAYING };

typedef struct {
    uint8_t *input, *codewords, *basis_codewords, *zero_codewords, *correction_codewords;
    uint8_t *matrix, *raster, *last_basis_raster, *wire, *header_xor, *previous_codewords;
    uint8_t headers[4][DOS32_FRAME_HEADER];
    uint32_t *delta_entries;
    int delta_ready;
    uint8_t *records[4], *parity, *xor_raster, *body, *disk_ring;
    uint8_t history[3][DOS32_RECORD_BYTES];
    uint32_t history_global[3];
    unsigned history_count;
    unsigned disk_pos[DISK_RING_SLOTS], disk_len[DISK_RING_SLOTS];
    unsigned disk_slot;
} WorkMemory;

typedef struct {
    FILE *file;
    uint32_t session, file_size, file_offset, record_id, global, total_records;
    uint32_t file_crc;
    unsigned stage;
    char name[128];
} RecordStream;

typedef struct {
    uint8_t state, width, slot, correction_applied, correction_plane, prepare_stage;
    uint16_t window_index, window_count, correction_patch_count;
    uint32_t window, group_global, ordinal, resident_hash, prepare_started, correction_restore_hash;
    uint8_t overlap;
    uint32_t basis_hash[4];
    uint16_t correction_patch_offset[VGA_PLANE_MAX_CORRECTION_PATCHES];
    uint8_t correction_patch_xor[VGA_PLANE_MAX_CORRECTION_PATCHES];
    uint8_t *canonical_parity;
} PreparedGroup;

typedef struct {
    PreparedGroup slots[SLOT_COUNT];
    uint8_t head, tail, preparing;
    unsigned ready;
    uint32_t prepared, prepare_ticks, starvation, gap_ticks, next_ordinal;
    uint32_t early_prepare_ticks, late_prepare_ticks, early_groups, late_groups;
} PlaneQueue;

typedef struct {
    unsigned width, window, hold, focus, verify, dump, dump_exit, dump_groups, noretrace;
    const char *path, *dump_dir;
} Options;

typedef struct {
    int active;
    unsigned groups_limit, groups_done;
    char dir[64];
    FILE *meta;
} DumpState;

static DumpState g_dump;
typedef struct {
    uint32_t qr_ticks, encode_ticks, delta_ticks, matrix_ticks, upload_ticks;
    uint32_t correction_ticks, select_ticks, hold_ticks, disk_ticks, wall_ticks;
    uint32_t disk_fills, disk_bytes, max_prepare_ticks, min_ready, tail_frames;
    uint32_t qr_calls, encode_calls, delta_calls, matrix_calls, upload_calls;
    uint32_t correction_calls, select_calls;
    uint32_t max_qr_ticks, max_encode_ticks, max_delta_ticks, max_matrix_ticks;
    uint32_t max_upload_ticks, max_correction_ticks, max_select_ticks;
} Metrics;
static Metrics metrics;
static FILE *trace_file;
static int g_noretrace;

static unsigned window_count(const RecordStream *s, uint32_t global, unsigned window);
static int stream_next(RecordStream *s, WorkMemory *w, unsigned window, uint16_t *wi, uint16_t *wc);
static int prepare_group_step(PlaneQueue *q, RecordStream *s, WorkMemory *w, Vga32 *vga,
                              unsigned width, unsigned window, int verify);
static int prepare_group(PlaneQueue *q, RecordStream *s, WorkMemory *w, Vga32 *vga,
                         unsigned width, unsigned window, int verify);
static uint32_t now_ticks(void);

static void trace_event(const char *event, unsigned slot, unsigned before, unsigned plane) {
    if (!trace_file) return;
    fprintf(trace_file, "tick=%lu %s slot=%u before=%02X plane=%u\n",
            (unsigned long)now_ticks(), event, slot, before, plane);
    fflush(trace_file);
}

static void trace_group(const char *event, const PreparedGroup *g) {
    if (!trace_file || !g) return;
    fprintf(trace_file, "tick=%lu %s slot=%u group=%lu ordinal=%lu overlap=%u window=%lu state=%u\n",
            (unsigned long)now_ticks(), event, g->slot, (unsigned long)g->group_global,
            (unsigned long)g->ordinal, (unsigned)g->overlap,
            (unsigned long)g->window, (unsigned)g->state);
    fflush(trace_file);
}

static uint32_t now_ticks(void) { return (uint32_t)clock(); }
static uint32_t ticks_ms(uint32_t t) { return (t * 1000UL) / (uint32_t)CLOCKS_PER_SEC; }

static int dump_write_bin(const char *name, const uint8_t *data, unsigned len) {
    char path[80];
    FILE *f;
    if (!g_dump.active) return 0;
    sprintf(path, "%s\\%s", g_dump.dir, name);
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(data, 1, len, f) != len) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

static int dump_begin(const char *dir, unsigned groups_limit) {
    char path[80];
    memset(&g_dump, 0, sizeof(g_dump));
    strncpy(g_dump.dir, dir, sizeof(g_dump.dir) - 1u);
    g_dump.dir[sizeof(g_dump.dir) - 1u] = '\0';
    _mkdir(g_dump.dir);
    sprintf(path, "%s\\D32META.TXT", g_dump.dir);
    g_dump.meta = fopen(path, "wt");
    if (!g_dump.meta) return 0;
    fprintf(g_dump.meta, "build=%s\nqr_size=%u qr_x0=%u raster=%u\n",
            DOSFER32_BUILD_ID, QR_SIZE, QR_X0, VGA_RASTER_BYTES);
    fflush(g_dump.meta);
    g_dump.active = 1;
    g_dump.groups_limit = groups_limit ? groups_limit : 1u;
    return 1;
}

static void dump_end(void) {
    if (g_dump.meta) { fclose(g_dump.meta); g_dump.meta = 0; }
    g_dump.active = 0;
}

static void dump_prepare_plane(unsigned group, unsigned plane, const uint8_t *raster,
                               const uint8_t *codewords, const uint8_t *matrix) {
    char name[16];
    if (!g_dump.active || group >= g_dump.groups_limit) return;
    sprintf(name, "G%03uT%u.RAW", group, plane);
    dump_write_bin(name, raster, VGA_RASTER_BYTES);
    sprintf(name, "G%03uCW%u.BIN", group, plane);
    dump_write_bin(name, codewords, QR_CODEWORDS);
    if (plane == 0u && matrix) {
        sprintf(name, "G%03uMX.BIN", group);
        dump_write_bin(name, matrix, (unsigned)QR_BUFFER);
    }
}

static void dump_symbol(unsigned group, unsigned symbol, uint8_t mask, unsigned slot,
                        Vga32 *vga, int correction) {
    uint8_t *buf, *planes[4], composed[VGA_RASTER_BYTES];
    char name[16];
    unsigned p;
    if (!g_dump.active || group >= g_dump.groups_limit) return;
    buf = (uint8_t *)malloc(VGA_RASTER_BYTES * 4u);
    if (!buf) return;
    for (p = 0; p < 4u; ++p) planes[p] = buf + (size_t)p * VGA_RASTER_BYTES;
    if (!vga32_read_planes(vga, slot, planes)) { free(buf); return; }
    if (symbol == 0u) {
        for (p = 0; p < 4u; ++p) {
            sprintf(name, "G%03uV%u.RAW", group, p);
            dump_write_bin(name, planes[p], VGA_RASTER_BYTES);
        }
    }
    vga32_compose_raster((const uint8_t *const *)planes, mask, composed);
    sprintf(name, "G%03uS%u.RAW", group, symbol);
    dump_write_bin(name, composed, VGA_RASTER_BYTES);
    if (g_dump.meta) {
        fprintf(g_dump.meta, "group=%u sym=%u mask=%02X slot=%u correction=%d file=%s\n",
                group, symbol, (unsigned)mask, slot, correction, name);
        fflush(g_dump.meta);
    }
    free(buf);
}

static const uint8_t status_font[16][7] = {
    /* 0..9 */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* G, C, F, space, fallback */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, {0,0,0,0,0,0,0},
    {0x1F,0x11,0x15,0x11,0x11,0x11,0x1F}, {0x00,0x04,0x0A,0x11,0x00,0x00,0x00}
};

static unsigned status_glyph(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c == 'G') return 10u;
    if (c == 'C') return 11u;
    if (c == 'F') return 12u;
    if (c == ' ') return 13u;
    return 14u;
}

static void status_label(char *out, unsigned group, unsigned coefficient) {
    if (!group) group = 1;
    sprintf(out, "G%uC%c", group, coefficient == 15u ? 'F' : (char)('0' + coefficient));
}
static uint8_t status_byte_at(const char *text, unsigned row, unsigned byte) {
    unsigned x, col, bit; uint8_t out = 0xFF;
    if (row >= 7u) return out;
    x = 8u;
    for (col = 0; text[col]; ++col, x += 6u) {
        unsigned glyph = status_glyph(text[col]);
        for (bit = 0; bit < 5u; ++bit)
            if ((status_font[glyph][row] >> (4u - bit)) & 1u) {
                unsigned px = x + bit;
                if ((px >> 3) == byte) out &= (uint8_t)~(0x80u >> (px & 7u));
            }
    }
    return out;
}

static void status_draw(uint8_t *raster, const char *text) {
    unsigned row, byte;
    for (row = 0; row < 7u; ++row)
        for (byte = 0; byte < 40u; ++byte)
            raster[(190u + row) * 40u + byte] = status_byte_at(text, row, byte);
}

static int correction_add_patch(PreparedGroup *g, uint16_t offset, uint8_t value) {
    unsigned i;
    if (!value) return 1;
    for (i = 0; i < g->correction_patch_count; ++i)
        if (g->correction_patch_offset[i] == offset) {
            g->correction_patch_xor[i] ^= value;
            return 1;
        }
    if (g->correction_patch_count >= VGA_PLANE_MAX_CORRECTION_PATCHES) return 0;
    g->correction_patch_offset[g->correction_patch_count] = offset;
    g->correction_patch_xor[g->correction_patch_count++] = value;
    return 1;
}

static int prepare_status_correction(PreparedGroup *g, WorkMemory *w, Vga32 *vga,
                                     unsigned width, const char *parity_text) {
    unsigned row, byte, i; uint16_t off; uint8_t effect, add;
    for (row = 0; row < 7u; ++row) for (byte = 0; byte < 40u; ++byte) {
        off = (uint16_t)((190u + row) * 40u + byte);
        effect = width == 4u ? vga->zero_raster[off] : 0;
        for (i = 0; i < g->correction_patch_count; ++i)
            if (g->correction_patch_offset[i] == off) effect ^= g->correction_patch_xor[i];
        add = (uint8_t)(w->xor_raster[off] ^ effect ^ status_byte_at(parity_text, row, byte));
        if (!correction_add_patch(g, off, add)) return 0;
    }
    return 1;
}
static const uint8_t reverse4_bits[16] = {
    0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
    0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF
};
static uint8_t reverse8_bits(uint8_t value) {
    return (uint8_t)((reverse4_bits[value & 0x0Fu] << 4) |
                     reverse4_bits[value >> 4]);
}
static void qr_raster_from_matrix(WorkMemory *w) {
    unsigned my, mx, bit, row_off, px;
    uint8_t *row, chunk;
    memset(w->raster, 0xFF, VGA_RASTER_BYTES);
    for (my = 0; my < QR_SIZE; ++my) {
        bit = my * QR_SIZE;
        row_off = (my + QR_QUIET) * 40u;
        row = w->raster + row_off;
        /* QR_X0 is deliberately bit 7 of a byte.  Eight matrix modules
           therefore clear one bit in the first byte and seven in the next;
           this avoids 177 divisions and matrix bit extracts per row. */
        for (mx = 0; mx + 8u <= QR_SIZE; mx += 8u, bit += 8u) {
            unsigned base = (bit >> 3) + 1u;
            uint32_t packed = (uint32_t)w->matrix[base] |
                              ((uint32_t)w->matrix[base + 1u] << 8);
            if (base + 2u < QR_BUFFER)
                packed |= (uint32_t)w->matrix[base + 2u] << 16;
            chunk = (uint8_t)((packed >> (bit & 7u)) & 0xFFu);
            px = QR_X0 + mx;
            row[px >> 3] &= (uint8_t)~(chunk & 1u);
            row[(px >> 3) + 1u] &= (uint8_t)~reverse8_bits((uint8_t)(chunk >> 1));
        }
        if (mx < QR_SIZE) {
            chunk = (uint8_t)((w->matrix[(bit >> 3) + 1u] >> (bit & 7u)) & 1u);
            if (chunk) {
                px = QR_X0 + mx;
                row[px >> 3] &= (uint8_t)~(uint8_t)(0x80u >> (px & 7u));
            }
        }
    }
}

static int qr_prepare_delta_map(WorkMemory *w) {
    const uint16_t *bytes = qrcodegen_dosferPlacementBytes();
    const uint8_t *masks = qrcodegen_dosferPlacementMasks();
    int bits = qrcodegen_dosferPlacementBits();
    int i, bit, x, y; unsigned linear, off; uint8_t m;
    if (!bytes || !masks || bits != (int)(QR_CODEWORDS * 8u)) return 0;
    for (i = 0; i < bits; ++i) {
        m = masks[i]; bit = 0;
        while (bit < 8 && m != (uint8_t)(1u << bit)) ++bit;
        if (bit >= 8) return 0;
        linear = ((unsigned)bytes[i] - 1u) * 8u + (unsigned)bit;
        y = (int)(linear / QR_SIZE); x = (int)(linear % QR_SIZE);
        { unsigned px = QR_X0 + (unsigned)x;
          off = (unsigned)(y + QR_QUIET) * 40u + (px >> 3);
          w->delta_entries[i] = (uint32_t)off | ((uint32_t)(0x80u >> (px & 7)) << 16); }
    }
    memcpy(w->previous_codewords, w->codewords, QR_CODEWORDS);
    w->delta_ready = 1;
    return 1;
}

static void qr_delta_fallback(WorkMemory *w) {
    unsigned i, bit; uint8_t changed; uint32_t entry;
    for (i = 0; i + 1u < QR_CODEWORDS; i += 2u) {
        changed = (uint8_t)(w->codewords[i] ^ w->previous_codewords[i]);
        w->previous_codewords[i] = w->codewords[i];
        if (changed) for (bit = 0; bit < 8; ++bit) if (changed & (uint8_t)(0x80u >> bit)) {
            entry = w->delta_entries[i * 8u + bit];
            w->raster[entry & 0xFFFFu] ^= (uint8_t)(entry >> 16);
        }
        changed = (uint8_t)(w->codewords[i + 1u] ^ w->previous_codewords[i + 1u]);
        w->previous_codewords[i + 1u] = w->codewords[i + 1u];
        if (changed) for (bit = 0; bit < 8; ++bit) if (changed & (uint8_t)(0x80u >> bit)) {
            entry = w->delta_entries[(i + 1u) * 8u + bit];
            w->raster[entry & 0xFFFFu] ^= (uint8_t)(entry >> 16);
        }
    }
}

static void qr_apply_codeword_delta(const WorkMemory *w, uint8_t *pixels,
                                    const uint8_t *from, const uint8_t *to) {
    unsigned i, bit; uint8_t changed; uint32_t entry;
    for (i = 0; i < QR_CODEWORDS; ++i) {
        changed = (uint8_t)(to[i] ^ from[i]);
        for (bit = 0; bit < 8; ++bit) if (changed & (uint8_t)(0x80u >> bit)) {
            entry = w->delta_entries[i * 8u + bit];
            pixels[entry & 0xFFFFu] ^= (uint8_t)(entry >> 16);
        }
    }
}

#if defined(__WATCOMC__) && defined(DOSFER32)
static void dosfer_delta32(const uint8_t *codewords, uint8_t *previous,
                           uint8_t *pixels, const uint32_t *entries, unsigned len);
#endif

static void qr_restore_basis_delta(WorkMemory *w) {
#if defined(__WATCOMC__) && defined(DOSFER32)
    dosfer_delta32(w->previous_codewords, w->zero_codewords, w->raster,
                   w->delta_entries, QR_CODEWORDS);
#else
    qr_apply_codeword_delta(w, w->raster, w->codewords, w->previous_codewords);
#endif
}

#if defined(__WATCOMC__) && defined(DOSFER32)
/* Two codewords per iteration: halves loop overhead on the 3706-byte stream. */
#pragma aux dosfer_delta32 = \
    "shr ecx,1" \
    "jz d32_done" \
    "mov ebp,edx" \
    "d32_loop:" \
    "mov al,[esi]" \
    "mov dl,[edi]" \
    "mov [edi],al" \
    "xor al,dl" \
    "jz d32_cw1" \
    "test al,80h" \
    "jz d32_a1" \
    "movzx edx,word ptr [ebx]" \
    "mov ah,byte ptr [ebx+2]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_a1:" \
    "test al,40h" \
    "jz d32_a2" \
    "movzx edx,word ptr [ebx+4]" \
    "mov ah,byte ptr [ebx+6]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_a2:" \
    "test al,20h" \
    "jz d32_a3" \
    "movzx edx,word ptr [ebx+8]" \
    "mov ah,byte ptr [ebx+10]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_a3:" \
    "test al,10h" \
    "jz d32_a4" \
    "movzx edx,word ptr [ebx+12]" \
    "mov ah,byte ptr [ebx+14]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_a4:" \
    "test al,8" \
    "jz d32_a5" \
    "movzx edx,word ptr [ebx+16]" \
    "mov ah,byte ptr [ebx+18]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_a5:" \
    "test al,4" \
    "jz d32_a6" \
    "movzx edx,word ptr [ebx+20]" \
    "mov ah,byte ptr [ebx+22]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_a6:" \
    "test al,2" \
    "jz d32_a7" \
    "movzx edx,word ptr [ebx+24]" \
    "mov ah,byte ptr [ebx+26]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_a7:" \
    "test al,1" \
    "jz d32_cw1" \
    "movzx edx,word ptr [ebx+28]" \
    "mov ah,byte ptr [ebx+30]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_cw1:" \
    "mov al,[esi+1]" \
    "mov dl,[edi+1]" \
    "mov [edi+1],al" \
    "xor al,dl" \
    "jz d32_next" \
    "test al,80h" \
    "jz d32_b1" \
    "movzx edx,word ptr [ebx+32]" \
    "mov ah,byte ptr [ebx+34]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b1:" \
    "test al,40h" \
    "jz d32_b2" \
    "movzx edx,word ptr [ebx+36]" \
    "mov ah,byte ptr [ebx+38]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b2:" \
    "test al,20h" \
    "jz d32_b3" \
    "movzx edx,word ptr [ebx+40]" \
    "mov ah,byte ptr [ebx+42]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b3:" \
    "test al,10h" \
    "jz d32_b4" \
    "movzx edx,word ptr [ebx+44]" \
    "mov ah,byte ptr [ebx+46]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b4:" \
    "test al,8" \
    "jz d32_b5" \
    "movzx edx,word ptr [ebx+48]" \
    "mov ah,byte ptr [ebx+50]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b5:" \
    "test al,4" \
    "jz d32_b6" \
    "movzx edx,word ptr [ebx+52]" \
    "mov ah,byte ptr [ebx+54]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b6:" \
    "test al,2" \
    "jz d32_b7" \
    "movzx edx,word ptr [ebx+56]" \
    "mov ah,byte ptr [ebx+58]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b7:" \
    "test al,1" \
    "jz d32_next" \
    "movzx edx,word ptr [ebx+60]" \
    "mov ah,byte ptr [ebx+62]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_next:" \
    "add esi,2" "add edi,2" "add ebx,64" "dec ecx" "jnz d32_loop" \
    "d32_done:" \
    parm [esi] [edi] [edx] [ebx] [ecx] \
    modify [eax ebp];
#endif

static int qr_prepare_correction_patches(Vga32 *vga, WorkMemory *w,
        const uint8_t *zero_codewords, const uint8_t *correction_codewords,
        uint16_t *patch_offset, uint8_t *patch_xor, uint16_t *patch_count) {
    unsigned i;
    if (!vga->zero_raster) return 0;
    if (!vga->zero_ready) {
        if (!qrcodegen_dosferBuildMatrixV40L(zero_codewords, w->matrix,
                qrcodegen_Mask_0)) return 0;
        qr_raster_from_matrix(w);
        memcpy(vga->zero_raster, w->raster, VGA_RASTER_BYTES);
        vga->zero_ready = 1;
    } else {
        memcpy(w->raster, vga->zero_raster, VGA_RASTER_BYTES);
    }
    memcpy(w->previous_codewords, zero_codewords, QR_CODEWORDS);
#if defined(__WATCOMC__) && defined(DOSFER32)
    dosfer_delta32(correction_codewords, w->previous_codewords, w->raster,
                   w->delta_entries, QR_CODEWORDS);
#else
    qr_apply_codeword_delta(w, w->raster, zero_codewords, correction_codewords);
#endif
    *patch_count = 0;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) {
        uint8_t x = (uint8_t)(w->raster[i] ^ vga->zero_raster[i]);
        if (!x) continue;
        if (*patch_count >= VGA_PLANE_MAX_CORRECTION_PATCHES) return 0;
        patch_offset[*patch_count] = (uint16_t)i;
        patch_xor[*patch_count] = x;
        ++*patch_count;
    }
    return 1;
}

static int qr_encode_wire_codewords(WorkMemory *w, const uint8_t *wire, uint8_t *out_codewords) {
    w->input[0] = 0x70; w->input[1] = 0x34;
    w->input[2] = 0x0B; w->input[3] = 0x88;
    memcpy(w->input + 4, wire, DOS32_FRAME_BYTES);
    return qrcodegen_dosferEncodePrepackedV40L(w->input, out_codewords) != 0;
}

static int qr_render(WorkMemory *w, const uint8_t *wire, int delta_only) {
    /* PLANE_CODED frames are always fixed-length V40-L records.  Feed the
       already packed ECI/byte header directly to the specialized encoder;
       this bypasses the generic segment planner and its buffer shuffling. */
    uint32_t elapsed;
    ++metrics.qr_calls;
    w->input[0] = 0x70; w->input[1] = 0x34;
    w->input[2] = 0x0B; w->input[3] = 0x88;
    memcpy(w->input + 4, wire, DOS32_FRAME_BYTES);
    { uint32_t t = now_ticks();
      if (!qrcodegen_dosferEncodePrepackedV40L(w->input, w->codewords)) return 0;
      elapsed = now_ticks() - t;
      metrics.encode_ticks += elapsed; ++metrics.encode_calls;
      if (elapsed > metrics.max_encode_ticks) metrics.max_encode_ticks = elapsed; }
    if (delta_only && w->delta_ready) {
        uint32_t t = now_ticks();
#if defined(__WATCOMC__) && defined(DOSFER32)
        dosfer_delta32(w->codewords, w->previous_codewords, w->raster,
                       w->delta_entries, QR_CODEWORDS);
#else
        qr_delta_fallback(w);
#endif
        elapsed = now_ticks() - t;
        metrics.delta_ticks += elapsed; ++metrics.delta_calls;
        if (elapsed > metrics.max_delta_ticks) metrics.max_delta_ticks = elapsed;
        return 1;
    }
    { uint32_t t = now_ticks();
      if (!delta_only || !w->delta_ready) {
          if (!qrcodegen_dosferBuildMatrixV40L(w->codewords, w->matrix,
                  qrcodegen_Mask_0)) return 0;
          qr_raster_from_matrix(w);
          /* A fresh group C1 establishes the delta baseline for C2/C4/C8.
           * Without this reset, the next basis delta would be computed
           * against the previous group's last basis codewords. */
          if (w->delta_ready) memcpy(w->previous_codewords, w->codewords, QR_CODEWORDS);
          if (!w->delta_ready && !qr_prepare_delta_map(w)) return 0;
           elapsed = now_ticks() - t;
           metrics.matrix_ticks += elapsed; ++metrics.matrix_calls;
           if (elapsed > metrics.max_matrix_ticks) metrics.max_matrix_ticks = elapsed;
       } }
    return 1;
}

static int prepare_group_claim(PlaneQueue *q, RecordStream *s, WorkMemory *w,
                               unsigned width, unsigned window) {
    PreparedGroup *g;
    uint16_t got_wi, got_wc;
    uint32_t start_global;
    unsigned i, remaining, overlap = 0;
    if (q->preparing != 0xFF || q->ready >= SLOT_COUNT ||
        s->global >= s->total_records || window == 0) return 0;
    got_wi = (uint16_t)(s->global % window);
    got_wc = (uint16_t)window_count(s, s->global, window);
    remaining = (unsigned)got_wc - (unsigned)got_wi;
    start_global = s->global;
    if ((uint32_t)remaining < width) {
        /* A short window/EOF tail is made into a real PLANE group by
           overlapping the immediately preceding records.  No filler record
           is invented: the receiver sees an exact duplicate for the overlap
           and the remaining records at their original indexes. */
        overlap = width - remaining;
        start_global = s->global - overlap;
        if (s->global < overlap || w->history_count < overlap ||
            w->history_global[w->history_count - overlap] != start_global)
            return 0;
        got_wi = (uint16_t)(start_global % window);
        if ((unsigned)got_wi + width > got_wc) return 0;
    } else if (s->global + width > s->total_records) {
        return 0;
    }
    /* Tail may still point at the slot currently on screen.  Scan for any
       free page instead of spinning forever on a busy index. */
    for (i = 0; i < SLOT_COUNT; ++i) {
        uint8_t idx = (uint8_t)((q->tail + i) & 7u);
        if (q->slots[idx].state == SLOT_FREE) {
            q->tail = idx;
            break;
        }
    }
    g = &q->slots[q->tail];
    if (g->state != SLOT_FREE) return 0;
    g->state = SLOT_FILLING; g->slot = q->tail; g->width = (uint8_t)width;
    g->window_index = got_wi; g->window_count = got_wc;
    g->correction_applied = 0;
    /* Plane 0 is hidden while the last basis (C4/C8) is visible. */
    g->correction_plane = 0;
    g->correction_patch_count = 0; g->prepare_stage = 0;
    g->window = start_global / window; g->group_global = start_global;
    g->ordinal = ++q->next_ordinal; g->overlap = (uint8_t)overlap;
    g->prepare_started = now_ticks();
    trace_group("group claim", g);
    memset(w->xor_raster, 0, VGA_RASTER_BYTES);
    memset(w->parity, 0, DOS32_RECORD_BYTES);
    memset(w->header_xor, 0, DOS32_FRAME_HEADER);
    q->preparing = q->tail;
    return 1;
}

static int prepare_group_step(PlaneQueue *q, RecordStream *s, WorkMemory *w, Vga32 *vga,
                              unsigned width, unsigned window, int verify) {
    static const uint8_t coefficient[4] = { 1, 2, 4, 8 };
    PreparedGroup *g; uint16_t got_wi, got_wc; unsigned p, i;
    if (q->preparing == 0xFF && !prepare_group_claim(q, s, w, width, window)) return 0;
    g = &q->slots[q->preparing];
    if (g->prepare_stage < g->width) {
        int rc;
        p = g->prepare_stage;
        if (p < g->overlap) {
            unsigned h = w->history_count - g->overlap + p;
            memcpy(w->records[0], w->history[h], DOS32_RECORD_BYTES);
            got_wi = (uint16_t)(g->window_index + p);
            got_wc = g->window_count;
            rc = 1;
        } else {
            rc = stream_next(s, w, window, &got_wi, &got_wc);
        }
        if (rc <= 0 || got_wi != g->window_index + p || got_wc != g->window_count) return -1;
        for (i = 0; i < DOS32_RECORD_BYTES; ++i) w->parity[i] ^= w->records[0][i];
        if (!dos32_plane_frame(w->wire, s->session, g->window, g->group_global, g->window_index,
                g->window_count, (uint8_t)width, coefficient[p], w->records[0])) return -1;
        memcpy(w->headers[p], w->wire, DOS32_FRAME_HEADER);
        for (i = 0; i < DOS32_FRAME_HEADER; ++i) w->header_xor[i] ^= w->wire[i];
         { uint32_t a = now_ticks();
           uint32_t elapsed;
          /* Once the placement map exists, stay in sparse-delta mode across
             groups  including the next C1.  Forcing a full matrix on every
             group plane-0 was the dominant PLANE prepare cost. */
          if (!qr_render(w, w->wire, w->delta_ready != 0)) return -1;
          { char label[16]; status_label(label, (unsigned)g->ordinal, coefficient[p]); status_draw(w->raster, label); }
           elapsed = now_ticks() - a;
           metrics.qr_ticks += elapsed;
           if (elapsed > metrics.max_qr_ticks) metrics.max_qr_ticks = elapsed; }
        if (verify) {
            memcpy(g->canonical_parity, w->raster, VGA_RASTER_BYTES);
            if (!qrcodegen_dosferBuildMatrixV40L(w->codewords, w->matrix,
                    qrcodegen_Mask_0)) return -1;
            qr_raster_from_matrix(w);
            { char label[16]; status_label(label, (unsigned)g->ordinal, coefficient[p]); status_draw(w->raster, label); }
            if (memcmp(g->canonical_parity, w->raster, VGA_RASTER_BYTES) != 0) return -1;
            memcpy(w->raster, g->canonical_parity, VGA_RASTER_BYTES);
        }
        memcpy(w->basis_codewords + (size_t)p * QR_CODEWORDS, w->codewords, QR_CODEWORDS);
        if (p + 1u == g->width && w->last_basis_raster)
            memcpy(w->last_basis_raster, w->raster, VGA_RASTER_BYTES);
         { uint32_t a = now_ticks(), elapsed;
           if (!vga32_store_fast(vga, p, g->slot, w->raster)) return -1;
           if (verify && !vga32_verify(vga, p, g->slot, w->raster)) return -1;
           elapsed = now_ticks() - a;
           metrics.upload_ticks += elapsed; ++metrics.upload_calls;
           if (elapsed > metrics.max_upload_ticks) metrics.max_upload_ticks = elapsed; }
        if (g_dump.active && g_dump.groups_done < g_dump.groups_limit)
            dump_prepare_plane(g_dump.groups_done, p, w->raster, w->codewords,
                               p == 0u ? w->matrix : 0);
        g->basis_hash[p] = verify ? vga32_hash(vga, p, g->slot) : 0;
        for (i = 0; i < VGA_RASTER_BYTES; ++i) w->xor_raster[i] ^= w->raster[i];
        ++g->prepare_stage;
        return 0;
    }
    if (g->prepare_stage == g->width) {
        uint8_t header_delta[DOS32_FRAME_HEADER];
        if (!dos32_plane_frame(w->wire, s->session, g->window, g->group_global, g->window_index,
                g->window_count, (uint8_t)width, width == 3 ? 7 : 15, w->parity)) return -1;
        if (width == 3 || width == 4) {
             uint32_t a = now_ticks(), elapsed;
             ++metrics.correction_calls;
            for (i = 0; i < DOS32_FRAME_HEADER; ++i) header_delta[i] = w->wire[i];
            for (p = 0; p < width; ++p)
                for (i = 0; i < DOS32_FRAME_HEADER; ++i) header_delta[i] ^= w->headers[p][i];
            if (!qrcodegen_dosferHeaderCorrectionV40L(header_delta, w->correction_codewords)) return -1;
            /* Encode Q(0) without touching the live delta baseline/raster. */
            w->input[0] = 0x70; w->input[1] = 0x34;
            w->input[2] = 0x0B; w->input[3] = 0x88;
            memset(w->input + 4, 0, DOS32_FRAME_BYTES);
            if (!qrcodegen_dosferEncodePrepackedV40L(w->input, w->zero_codewords)) return -1;
            if (!qr_prepare_correction_patches(vga, w, w->zero_codewords, w->correction_codewords,
                    g->correction_patch_offset, g->correction_patch_xor,
                    &g->correction_patch_count)) return -1;
            { char label[16]; status_label(label, (unsigned)g->ordinal, width == 3u ? 7u : 15u);
              if (!prepare_status_correction(g, w, vga, width, label)) return -1; }
            if (verify) {
                /* Oracle for the complete parity QR, not merely Q(delta). */
                if (!qr_encode_wire_codewords(w, w->wire, w->codewords) ||
                    !qrcodegen_dosferBuildMatrixV40L(w->codewords, w->matrix,
                        qrcodegen_Mask_0)) return -1;
                qr_raster_from_matrix(w);
                { char label[16]; status_label(label, (unsigned)g->ordinal, width == 3u ? 7u : 15u); status_draw(w->raster, label); }
                memcpy(g->canonical_parity, w->raster, VGA_RASTER_BYTES);
            }
            /* Restore last-basis raster + codeword baseline for the next C1 delta. */
            if (w->last_basis_raster)
                memcpy(w->raster, w->last_basis_raster, VGA_RASTER_BYTES);
            memcpy(w->previous_codewords,
                   w->basis_codewords + (size_t)(g->width - 1u) * QR_CODEWORDS,
                   QR_CODEWORDS);
             elapsed = now_ticks() - a;
             metrics.correction_ticks += elapsed;
             if (elapsed > metrics.max_correction_ticks) metrics.max_correction_ticks = elapsed;
        } else if (w->delta_ready) {
            /* PLANE3: previous_codewords already tracks the last basis via delta. */
        }
        g->resident_hash = g->basis_hash[0];
        {
            uint32_t pt = now_ticks() - g->prepare_started;
            g->state = SLOT_READY; ++q->ready; ++q->prepared;
            q->prepare_ticks += pt;
            if (pt > metrics.max_prepare_ticks) metrics.max_prepare_ticks = pt;
            if (q->prepared <= 8u) {
                q->early_prepare_ticks += pt; ++q->early_groups;
            } else {
                q->late_prepare_ticks += pt; ++q->late_groups;
            }
            if (q->ready < metrics.min_ready) metrics.min_ready = q->ready;
            trace_group("group ready", g);
        }
        q->tail = (uint8_t)((q->tail + 1u) & 7u);
        q->preparing = 0xFF;
        return 1;
    }
    return 0;
}

/* Finish at most one in-flight group.  Returns 1 when a group became ready,
   0 when idle/blocked, -1 on hard failure.  Never spins when no free slot. */
static int prepare_group(PlaneQueue *q, RecordStream *s, WorkMemory *w, Vga32 *vga,
                         unsigned width, unsigned window, int verify) {
    int rc;
    if (q->preparing == 0xFF && !prepare_group_claim(q, s, w, width, window)) return 0;
    do {
        rc = prepare_group_step(q, s, w, vga, width, window, verify);
    } while (rc == 0 && q->preparing != 0xFF);
    return rc;
}

static void put32_local(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t file_length(FILE *f) {
    long pos, end;
    pos = ftell(f); if (pos < 0 || fseek(f, 0, SEEK_END) != 0) return 0;
    end = ftell(f); fseek(f, pos, SEEK_SET); return end < 0 ? 0 : (uint32_t)end;
}

static const char *base_name(const char *path) {
    const char *p = path, *q;
    for (q = path; *q; ++q) if (*q == '\\' || *q == '/') p = q + 1;
    return p;
}

static unsigned data_records(uint32_t bytes) { return (unsigned)((bytes + 2875UL) / 2876UL); }

static size_t ring_read(RecordStream *s, WorkMemory *w, uint8_t *dst, size_t want) {
    size_t got = 0, take;
    while (got < want) {
        unsigned slot = w->disk_slot;
        if (w->disk_pos[slot] == w->disk_len[slot]) {
            uint32_t t = now_ticks();
            w->disk_len[slot] = (unsigned)fread(w->disk_ring + (size_t)slot * DISK_SLOT_BYTES,
                                                1, DISK_SLOT_BYTES, s->file);
            w->disk_pos[slot] = 0;
            metrics.disk_ticks += now_ticks() - t;
            ++metrics.disk_fills;
            metrics.disk_bytes += w->disk_len[slot];
            if (!w->disk_len[slot]) break;
        }
        take = w->disk_len[slot] - w->disk_pos[slot];
        if (take > want - got) take = want - got;
        memcpy(dst + got, w->disk_ring + (size_t)slot * DISK_SLOT_BYTES + w->disk_pos[slot], take);
        w->disk_pos[slot] += (unsigned)take; got += take;
        if (w->disk_pos[slot] >= w->disk_len[slot]) w->disk_slot ^= 1u;
    }
    return got;
}

static int stream_open(RecordStream *s, const char *path, uint32_t session) {
    const char *name = base_name(path); size_t n = strlen(name);
    memset(s, 0, sizeof(*s)); s->file = fopen(path, "rb");
    if (!s->file || n == 0 || n >= sizeof(s->name)) return 0;
    /* Full stdio buffer: host-mounted DOSBox I/O is otherwise syscall-heavy. */
    setvbuf(s->file, 0, _IOFBF, 8192);
    memcpy(s->name, name, n + 1); s->file_size = file_length(s->file); s->session = session;
    s->total_records = 4UL + data_records(s->file_size); /* session, begin, end, transfer */
    return 1;
}

static void stream_close(RecordStream *s) { if (s->file) fclose(s->file); s->file = 0; }

static unsigned window_count(const RecordStream *s, uint32_t global, unsigned window) {
    uint32_t start = (global / window) * window, left = s->total_records - start;
    return (unsigned)(left > window ? window : left);
}

static int stream_next(RecordStream *s, WorkMemory *w, unsigned window, uint16_t *wi, uint16_t *wc) {
    uint16_t body_len; uint32_t file_id = 1; unsigned cap = 2876u; size_t got;
    uint8_t *body = w->body;
    if (s->stage >= 5 || s->global >= s->total_records) return 0;
    memset(w->records[0], 0, DOS32_RECORD_BYTES);
    if (s->stage == 0) {
        put32_local(body, s->session); body[4] = 0; body[5] = 6; memcpy(body + 6, "DOSFER", 6);
        body_len = 12; s->stage = 1;
        dos32_record(w->records[0], DOS32_SESSION, s->record_id++, 0, body, body_len);
    } else if (s->stage == 1) {
        unsigned path_len = (unsigned)strlen(s->name);
        memset(body, 0, 12); put32_local(body + 6, s->file_size);
        body[10] = (uint8_t)(path_len >> 8); body[11] = (uint8_t)path_len;
        memcpy(body + 12, s->name, path_len); body_len = (uint16_t)(12 + path_len);
        s->stage = 2; s->file_crc = dos32_crc_start();
        dos32_record(w->records[0], DOS32_FILE_BEGIN, s->record_id++, file_id, body, body_len);
    } else if (s->stage == 2) {
        if (s->file_offset >= s->file_size) { s->stage = 3; return stream_next(s, w, window, wi, wc); }
        put32_local(body, s->file_offset);
        got = ring_read(s, w, body + 4, cap);
        if (!got && s->file_offset < s->file_size) return -1;
        s->file_crc = dos32_crc_update(s->file_crc, body + 4, got);
        s->file_offset += (uint32_t)got; body_len = (uint16_t)(got + 4);
        dos32_record(w->records[0], DOS32_FILE_DATA, s->record_id++, file_id, body, body_len);
    } else if (s->stage == 3) {
        put32_local(body, s->file_size); put32_local(body + 4, dos32_crc_finish(s->file_crc));
        s->stage = 4; dos32_record(w->records[0], DOS32_FILE_END, s->record_id++, file_id, body, 8);
    } else {
        put32_local(body, 1); put32_local(body + 4, 0); put32_local(body + 8, 0); put32_local(body + 12, s->file_size);
        s->stage = 5; dos32_record(w->records[0], DOS32_TRANSFER_END, s->record_id++, 0, body, 16);
    }
    *wi = (uint16_t)(s->global % window); *wc = (uint16_t)window_count(s, s->global, window);
    if (w->history_count < 3u) {
        memcpy(w->history[w->history_count], w->records[0], DOS32_RECORD_BYTES);
        w->history_global[w->history_count] = s->global;
        ++w->history_count;
    } else {
        memmove(w->history[0], w->history[1], 2u * DOS32_RECORD_BYTES);
        memmove(w->history_global, w->history_global + 1, 2u * sizeof(uint32_t));
        memcpy(w->history[2], w->records[0], DOS32_RECORD_BYTES);
        w->history_global[2] = s->global;
    }
    ++s->global; return 1;
}

static PreparedGroup *next_ready_group(PlaneQueue *q, const PreparedGroup *current) {
    PreparedGroup *best = 0; unsigned i;
    for (i = 0; i < SLOT_COUNT; ++i) {
        PreparedGroup *candidate = &q->slots[i];
        if (candidate->state != SLOT_READY || candidate->group_global <= current->group_global) continue;
        if (!best || candidate->group_global < best->group_global) best = candidate;
    }
    return best;
}
static int show_group(PlaneQueue *q, PreparedGroup *g, RecordStream *s, WorkMemory *w,
                      Vga32 *vga, unsigned width, unsigned window, unsigned hold,
                      int focus, int verify, int dump_group) {
    unsigned symbol, count = g->width + 1u; uint32_t deadline, start;
    g->state = SLOT_PLAYING; --q->ready;
    for (symbol = 0; symbol < count; ++symbol) {
        uint8_t mask = symbol == g->width ? (uint8_t)((1u << g->width) - 1u) : (uint8_t)(1u << symbol);
        if (mask == (uint8_t)((1u << g->width) - 1u)) {
            trace_event("correction apply begin", g->slot, mask, g->correction_plane);
            if (!vga32_apply_correction(vga, g->correction_plane, g->slot,
                    g->correction_patch_offset, g->correction_patch_xor,
                    g->correction_patch_count, g->width == 4, &g->correction_restore_hash, verify)) return 0;
            g->correction_applied = 1;
            trace_event("correction apply end", g->slot, 0, g->correction_plane);
            if (verify && g->canonical_parity &&
                !vga32_verify_composed(vga, g->slot, mask, g->canonical_parity)) return 0;
            trace_event("parity show", g->slot, 0, g->correction_plane);
        }
        { uint32_t a = now_ticks(), elapsed;
          if (!vga32_show_raw(vga, g->slot, mask, !g_noretrace)) return 0;
          elapsed = now_ticks() - a;
          metrics.select_ticks += elapsed; ++metrics.select_calls;
          if (elapsed > metrics.max_select_ticks) metrics.max_select_ticks = elapsed; }
        if (g_dump.active && dump_group < (int)g_dump.groups_limit)
            dump_symbol((unsigned)dump_group, symbol, mask, g->slot, vga,
                        g->correction_applied && mask == (uint8_t)((1u << g->width) - 1u));
        trace_event("symbol show", g->slot, mask, g->correction_plane);
        if (symbol == 0 && focus) {
            int key, pr;
            while (!kbhit()) {
                if (q->ready < SLOT_COUNT) {
                    pr = prepare_group_step(q, s, w, vga, width, window, verify);
                    if (pr < 0) return 0;
                    if (pr == 0 && q->preparing == 0xFF) delay(1);
                } else delay(1);
            }
            do key = getch(); while (key != 13 && key != 27);
            if (key == 27) return -1;
        }
        start = now_ticks(); deadline = start + (uint32_t)hold * CLOCKS_PER_SEC / 1000UL;
        { uint32_t a = now_ticks(); while ((long)(now_ticks() - deadline) < 0) {
            int pr;
            if (q->ready < SLOT_COUNT) {
                pr = prepare_group_step(q, s, w, vga, width, window, verify);
                if (pr < 0) return 0;
            }
        } metrics.hold_ticks += now_ticks() - a; }
        if (mask == (uint8_t)((1u << g->width) - 1u)) {
            trace_event("parity hold end", g->slot, mask, g->correction_plane);
            /* Keep this slot visible while restoring its hidden correction.
               The next group is selected before this slot is recycled, so
               producer uploads can never mutate the visible bridge raster. */
            if (!vga32_show_raw(vga, g->slot, 2u, !g_noretrace)) return 0;
            trace_event("parity bridge C2", g->slot, 2u, 0);            trace_event("correction restore begin", g->slot, 0, g->correction_plane);
            if (!vga32_restore_correction(vga, g->correction_plane, g->slot,
                     g->correction_patch_offset, g->correction_patch_xor,
                     g->correction_patch_count, g->width == 4, g->correction_restore_hash, verify)) return 0;
            g->correction_applied = 0;
            trace_event("correction restore end", g->slot, 0, g->correction_plane);
            if (verify) for (unsigned p = 0; p < g->width; ++p)
                if (vga32_hash(vga, p, g->slot) != g->basis_hash[p]) return 0;
            {
                PreparedGroup *next = next_ready_group(q, g);
                while (!next && s->global + width <= s->total_records) {
                    int pr = prepare_group(q, s, w, vga, width, window, verify);
                    if (pr < 0) return 0;
                    if (pr == 0) break;
                    next = next_ready_group(q, g);
                }
                if (next) {
                    if (!vga32_show_raw(vga, next->slot, 1u, !g_noretrace)) return 0;
                    trace_event("next group C1 handoff", next->slot, 1u, 0);
                    trace_group("next group metadata", next);
                    /* The CRTC start write is performed at the retrace edge,
                       but some adapters latch it on the following scanout.
                       Do not recycle/upload the old slot until that latch has
                       completed, or its new C1 can flash for one frame. */
                    if (!g_noretrace) vga32_wait_retrace();
                    q->head = next->slot;
                }
            }
        }
    }
    g->state = SLOT_FREE; return 1;
}

#define TAIL_VGA_SLOT 7u

static int show_tail(RecordStream *s, WorkMemory *w, Vga32 *vga, unsigned window, unsigned hold) {
    uint16_t wi, wc; uint32_t global, sid = 0, off = 0; uint32_t deadline;
    /* Plane groups need `width` records.  Short window/EOF remainders are plain
       DATA frames  never invent duplicate records (that breaks reconstruction).
       Display them with the normal monochrome palette, not PLANE odd-parity. */
    unsigned visible = hold ? hold : 100u;
    if (stream_next(s, w, window, &wi, &wc) <= 0) return 0;
    global = s->global - 1UL;
    if (w->records[0][5] == DOS32_FILE_BEGIN || w->records[0][5] == DOS32_FILE_DATA ||
        w->records[0][5] == DOS32_FILE_END) {
        sid = ((uint32_t)w->records[0][12] << 24) | ((uint32_t)w->records[0][13] << 16) |
              ((uint32_t)w->records[0][14] << 8) | w->records[0][15];
        if (w->records[0][5] == DOS32_FILE_DATA)
            off = ((uint32_t)w->records[0][24] << 24) | ((uint32_t)w->records[0][25] << 16) |
                  ((uint32_t)w->records[0][26] << 8) | w->records[0][27];
    }
    if (!dos32_frame(w->wire, DOS32_DATA, DOS32_FLAG_WHITENED, s->session,
            global / window, global, wi, wc, sid, off, w->records[0], DOS32_FRAME_PAYLOAD) ||
        !qr_render(w, w->wire, 0) ||
        !vga32_show_data_qr(vga, TAIL_VGA_SLOT, w->raster, !g_noretrace)) return 0;
    ++metrics.tail_frames;
    deadline = now_ticks() + (uint32_t)visible * CLOCKS_PER_SEC / 1000UL;
    while ((long)(now_ticks() - deadline) < 0) {}
    return 1;
}

static void usage(void) {
    puts("DOSFER32 file [/RE:PLANE3|/RE:PLANE4] [/WINDOW:n] [/HOLD:ms]");
    puts("  [/NOFOCUS] [/NORETRACE] [/VERIFY] [/DUMP[:dir]] [/DUMPGROUPS:n] [/DUMPEXIT]");
}

static void options(int argc, char **argv, Options *o) {
    int i;
    o->width = 4; o->window = 32; o->hold = HOLD_DEFAULT; o->focus = 1; o->verify = 0;
    o->dump = 0; o->dump_exit = 0; o->dump_groups = 1; o->noretrace = 0;
    o->path = 0; o->dump_dir = "D32DUMP";
    for (i = 1; i < argc; ++i) {
        if (!strnicmp(argv[i], "/RE:PLANE3", 10)) o->width = 3;
        else if (!strnicmp(argv[i], "/RE:PLANE4", 10)) o->width = 4;
        else if (!strnicmp(argv[i], "/WINDOW:", 8)) o->window = (unsigned)atoi(argv[i] + 8);
        else if (!strnicmp(argv[i], "/HOLD:", 6)) o->hold = (unsigned)atoi(argv[i] + 6);
        else if (!stricmp(argv[i], "/NOFOCUS")) o->focus = 0;
        else if (!stricmp(argv[i], "/NORETRACE")) o->noretrace = 1;
        else if (!stricmp(argv[i], "/VERIFY")) o->verify = 1;
        else if (!stricmp(argv[i], "/DUMP")) o->dump = 1;
        else if (!strnicmp(argv[i], "/DUMP:", 6)) { o->dump = 1; o->dump_dir = argv[i] + 6; }
        else if (!strnicmp(argv[i], "/DUMPGROUPS:", 12)) o->dump_groups = (unsigned)atoi(argv[i] + 12);
        else if (!stricmp(argv[i], "/DUMPEXIT")) o->dump_exit = 1;
        else if (argv[i][0] != '/') o->path = argv[i];
    }
    if (o->window < 4) o->window = 4; if (o->window > MAX_WINDOW32) o->window = MAX_WINDOW32;
    if (!o->dump_groups) o->dump_groups = 1;
}

static void report_profile(const PlaneQueue *q, int completed, unsigned width, uint32_t useful_bytes) {
    FILE *f = fopen("DOSFER32.PRO", "wt");
    unsigned long groups = (unsigned long)q->prepared, symbols = groups * (width + 1u);
    unsigned long prepare_ms = (unsigned long)ticks_ms(q->prepare_ticks);
    unsigned long hold_ms = (unsigned long)ticks_ms(metrics.hold_ticks);
    unsigned long select_ms = (unsigned long)ticks_ms(metrics.select_ticks);
    unsigned long disk_ms = (unsigned long)ticks_ms(metrics.disk_ticks);
    unsigned long wall_ms = (unsigned long)ticks_ms(metrics.wall_ticks);
    unsigned long elapsed_ms = wall_ms ? wall_ms : (prepare_ms + hold_ms + select_ms);
    unsigned long symbols_per_sec_milli = elapsed_ms ? symbols * 1000000UL / elapsed_ms : 0;
    unsigned long useful_bytes_sec = elapsed_ms ? (unsigned long)useful_bytes * 1000UL / elapsed_ms : 0;
    unsigned long early_ms = (unsigned long)ticks_ms(q->early_prepare_ticks);
    unsigned long late_ms = (unsigned long)ticks_ms(q->late_prepare_ticks);
    if (!f) f = stdout;
    fprintf(f,
        "build=%s\ncompleted=%d\ngroups=%lu\nsymbols=%lu\n"
        "wall_ms=%lu\nprepare_ms=%lu\nprepare_ms_group=%lu\nmax_prepare_ms=%lu\n"
        "early_groups=%lu\nearly_prepare_ms_group=%lu\nlate_groups=%lu\nlate_prepare_ms_group=%lu\n"
        "qr_ms=%lu\nencode_ms=%lu\ndelta_ms=%lu\nmatrix_ms=%lu\nupload_ms=%lu\n"
        "correction_ms=%lu\nselector_ms=%lu\ndisk_ms=%lu\ndisk_fills=%lu\ndisk_bytes=%lu\n"
        "qr_calls=%lu\nencode_calls=%lu\ndelta_calls=%lu\nmatrix_calls=%lu\n"
        "upload_calls=%lu\ncorrection_calls=%lu\nselector_calls=%lu\n"
        "max_qr_ms=%lu\nmax_encode_ms=%lu\nmax_delta_ms=%lu\nmax_matrix_ms=%lu\n"
        "max_upload_ms=%lu\nmax_correction_ms=%lu\nmax_selector_ms=%lu\n"
        "hold_ms=%lu\nvisible_ms_symbol=%lu\nsustained_symbols_s=%lu.%03lu\n"
        "useful_bytes_s=%lu\nstarvation=%lu\nmin_ready=%lu\ntail_frames=%lu\n",
        DOSFER32_BUILD_ID, completed, groups, symbols,
        wall_ms, prepare_ms, groups ? prepare_ms / groups : 0,
        (unsigned long)ticks_ms(metrics.max_prepare_ticks),
        (unsigned long)q->early_groups,
        q->early_groups ? early_ms / q->early_groups : 0,
        (unsigned long)q->late_groups,
        q->late_groups ? late_ms / q->late_groups : 0,
        (unsigned long)ticks_ms(metrics.qr_ticks), (unsigned long)ticks_ms(metrics.encode_ticks),
        (unsigned long)ticks_ms(metrics.delta_ticks), (unsigned long)ticks_ms(metrics.matrix_ticks),
        (unsigned long)ticks_ms(metrics.upload_ticks),
        (unsigned long)ticks_ms(metrics.correction_ticks),
        select_ms, disk_ms, (unsigned long)metrics.disk_fills, (unsigned long)metrics.disk_bytes,
        (unsigned long)metrics.qr_calls, (unsigned long)metrics.encode_calls,
        (unsigned long)metrics.delta_calls, (unsigned long)metrics.matrix_calls,
        (unsigned long)metrics.upload_calls, (unsigned long)metrics.correction_calls,
        (unsigned long)metrics.select_calls,
        (unsigned long)ticks_ms(metrics.max_qr_ticks), (unsigned long)ticks_ms(metrics.max_encode_ticks),
        (unsigned long)ticks_ms(metrics.max_delta_ticks), (unsigned long)ticks_ms(metrics.max_matrix_ticks),
        (unsigned long)ticks_ms(metrics.max_upload_ticks), (unsigned long)ticks_ms(metrics.max_correction_ticks),
        (unsigned long)ticks_ms(metrics.max_select_ticks),
        hold_ms, symbols ? hold_ms / symbols : 0,
        symbols_per_sec_milli / 1000UL, symbols_per_sec_milli % 1000UL, useful_bytes_sec,
        (unsigned long)q->starvation, (unsigned long)metrics.min_ready,
        (unsigned long)metrics.tail_frames);
    if (f != stdout) fclose(f); else fflush(f);
}

int main(int argc, char **argv) {
    Options o; RecordStream stream; WorkMemory *w; PlaneQueue *q; Vga32 vga; uint32_t session;
    unsigned target, i; int rc = 0, completed = 0, first_group = 1; uint32_t start;
    options(argc, argv, &o); if (!o.path) { usage(); return 2; }
    g_noretrace = o.noretrace != 0;
    memset(&metrics, 0, sizeof(metrics));
    metrics.min_ready = SLOT_COUNT;
    /* Keep the trace name strictly 8.3 so it works on real DOS, not only
       DOSBox's long-name layer. */
    trace_file = fopen("DOSFER32.TRC", "wt");
    session = (uint32_t)time(0) ^ 0xD05F3201UL; if (!session) session = 1;
    printf("DOSFER32 %s (Open Watcom + DOS/4GW)\n", DOSFER32_BUILD_ID);
    printf("Mode: PLANE%u  window=%u  hold=%u ms%s%s\n", o.width, o.window, o.hold,
           o.focus ? "" : "  nofocus", o.noretrace ? "  noretrace" : "");
    if (o.dump) {
        if (!dump_begin(o.dump_dir, o.dump_groups)) { puts("DOSFER32: dump directory open failed"); return 2; }
        printf("Dump: %s  groups=%u%s\n", g_dump.dir, g_dump.groups_limit, o.dump_exit ? "  exit-after-dump" : "");
        o.focus = 0;
    }
    w = (WorkMemory *)calloc(1, sizeof(*w)); q = (PlaneQueue *)calloc(1, sizeof(*q)); memset(&vga, 0, sizeof(vga));
    if (!w || !q) { puts("DOSFER32: workspace allocation failed"); free(w); free(q); return 2; }
    q->preparing = 0xFF;
    w->input = (uint8_t *)malloc(QR_DATA_CODEWORDS); w->codewords = (uint8_t *)malloc(QR_CODEWORDS);
    w->basis_codewords = (uint8_t *)malloc(4u * QR_CODEWORDS);
    w->zero_codewords = (uint8_t *)malloc(QR_CODEWORDS);
    w->correction_codewords = (uint8_t *)malloc(QR_CODEWORDS);
    w->previous_codewords = (uint8_t *)malloc(QR_CODEWORDS);
    w->delta_entries = (uint32_t *)malloc(8u * QR_CODEWORDS * sizeof(uint32_t));
    w->matrix = (uint8_t *)malloc(QR_BUFFER);
    w->raster = (uint8_t *)malloc(VGA_RASTER_BYTES);
    w->last_basis_raster = (uint8_t *)malloc(VGA_RASTER_BYTES);
    w->wire = (uint8_t *)malloc(DOS32_FRAME_BYTES);
    w->header_xor = (uint8_t *)malloc(DOS32_FRAME_HEADER);
    w->parity = (uint8_t *)malloc(DOS32_RECORD_BYTES); w->xor_raster = (uint8_t *)malloc(VGA_RASTER_BYTES); w->body = (uint8_t *)malloc(2880); w->disk_ring = (uint8_t *)malloc(DISK_RING_BYTES);
    for (i = 0; i < 4; ++i) w->records[i] = (uint8_t *)malloc(DOS32_RECORD_BYTES);
    if (o.verify) for (i = 0; i < SLOT_COUNT; ++i)
        q->slots[i].canonical_parity = (uint8_t *)malloc(VGA_RASTER_BYTES);
    if (!w->input || !w->codewords || !w->basis_codewords || !w->zero_codewords ||
        !w->correction_codewords || !w->previous_codewords || !w->delta_entries || !w->matrix ||
        !w->raster || !w->last_basis_raster || !w->wire || !w->header_xor || !w->parity ||
        !w->xor_raster || !w->body ||
        !w->disk_ring || !vga32_enter(&vga)) {
        puts("DOSFER32: protected-mode workspace/VGA allocation failed"); rc = 2; goto done;
    }
    if (o.verify) for (i = 0; i < SLOT_COUNT; ++i)
        if (!q->slots[i].canonical_parity) { puts("DOSFER32: verify buffer allocation failed"); rc = 2; goto done; }
    printf("CRTC slot step=%u words (%u bytes)\n", (unsigned)vga.slot_step,
           (unsigned)vga.slot_step * 2u);
    if (!stream_open(&stream, o.path, session)) { puts("DOSFER32: cannot open source file"); rc = 2; goto done; }
    /* Prefetch both disk half-buffers so the first DATA records do not stall
       on a cold fread during the initial display burst. */
    {
        uint32_t t = now_ticks();
        w->disk_len[0] = (unsigned)fread(w->disk_ring, 1, DISK_SLOT_BYTES, stream.file);
        w->disk_len[1] = (unsigned)fread(w->disk_ring + DISK_SLOT_BYTES, 1, DISK_SLOT_BYTES, stream.file);
        w->disk_pos[0] = w->disk_pos[1] = 0; w->disk_slot = 0;
        metrics.disk_ticks += now_ticks() - t;
        metrics.disk_fills += (w->disk_len[0] ? 1u : 0u) + (w->disk_len[1] ? 1u : 0u);
        metrics.disk_bytes += w->disk_len[0] + w->disk_len[1];
    }
    printf("Records: %lu, free protected memory: %lu bytes\n", (unsigned long)stream.total_records, (unsigned long)_memavl());
    /* Prefill a couple of groups, then keep alternating play / produce.  Short
       window/EOF remainders (WINDOW % PLANE width != 0) are shown as DATA via
       show_tail  padding with duplicate records invents globals and breaks
       Android reconstruction. */
    target = 2u;
    start = now_ticks();
    while (q->ready < target && prepare_group(q, &stream, w, &vga, o.width, o.window, o.verify) > 0) {}
    rc = 1;
    for (;;) {
        while (q->ready < SLOT_COUNT &&
               prepare_group(q, &stream, w, &vga, o.width, o.window, o.verify) > 0) {}
        if (q->ready) {
            PreparedGroup *g = &q->slots[q->head];
            int dump_group = g_dump.active ? (int)g_dump.groups_done : -1;
            trace_event("group begin", g->slot, 0, g->width);
            trace_group("group begin metadata", g);
            rc = show_group(q, g, &stream, w, &vga, o.width, o.window, o.hold,
                            first_group && o.focus, o.verify, dump_group);
            trace_event(rc > 0 ? "group done" : "group fail", g->slot, (unsigned)(rc < 0 ? 0xFF : rc), g->width);
            first_group = 0;
            if (g_dump.active) {
                ++g_dump.groups_done;
                if (o.dump_exit || g_dump.groups_done >= g_dump.groups_limit) {
                    rc = -2;
                    break;
                }
            }
            if (rc <= 0) break;
            if (q->ready < metrics.min_ready) metrics.min_ready = q->ready;
            continue;
        }
        if (stream.global >= stream.total_records) break;
        if (!show_tail(&stream, w, &vga, o.window, o.hold)) { rc = 0; break; }
    }
    /* Keep the final symbol visible briefly even with /HOLD:0 so the last
       tail/DATA frame is not erased by the mode-3 restore. */
    if (rc > 0) {
        uint32_t linger = now_ticks() + (uint32_t)(o.hold ? o.hold : 100u) * CLOCKS_PER_SEC / 1000UL;
        while ((long)(now_ticks() - linger) < 0) {}
    }
    metrics.wall_ticks = now_ticks() - start;
    if (rc > 0) completed = 1;
    stream_close(&stream);
done:
    dump_end();
    vga32_leave(&vga);
    report_profile(q, completed, o.width, stream.file_size);
    if (completed) puts("DOSFER32 transfer complete");
    if (q) for (i = 0; i < SLOT_COUNT; ++i) free(q->slots[i].canonical_parity);
    if (w) { for (i = 0; i < 4; ++i) free(w->records[i]); free(w->input); free(w->codewords);
        free(w->basis_codewords); free(w->zero_codewords); free(w->correction_codewords);
        free(w->previous_codewords); free(w->delta_entries); free(w->matrix); free(w->raster);
        free(w->last_basis_raster); free(w->wire); free(w->header_xor); free(w->parity);
        free(w->xor_raster); free(w->body);
        free(w->disk_ring); }
    free(q); free(w); if (trace_file) { fclose(trace_file); trace_file = 0; }
    return rc == -2 ? 0 : (rc < 0 ? 1 : rc);
}
