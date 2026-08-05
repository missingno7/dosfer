#include "platform_vga.h"
#include "protocol32.h"
#include "qrcodegen.h"
#include <conio.h>
#include <dos.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DOSFER32_BUILD_ID "dosfer32-stream-r7-delta-rs"
#define QR_SIZE 177u
#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(40)
#define QR_DATA_CODEWORDS 2956u
#define QR_CODEWORDS 3706u
#define SLOT_COUNT 8u
#define MAX_WINDOW32 128u
#define HOLD_DEFAULT 100u
#define DISK_RING_BYTES 32768u

enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_PLAYING };

typedef struct {
    uint8_t *input, *codewords, *basis_codewords, *zero_codewords;
    uint8_t *matrix, *raster, *wire, *header_xor, *previous_codewords;
    uint32_t *delta_entries;
    int delta_ready;
    uint8_t *records[4], *parity, *xor_raster, *body, *disk_ring;
    unsigned disk_pos, disk_len;
} WorkMemory;

typedef struct {
    FILE *file;
    uint32_t session, file_size, file_offset, record_id, global, total_records;
    uint32_t file_crc;
    unsigned stage;
    char name[128];
} RecordStream;

typedef struct {
    uint8_t state, width, slot, correction_applied, correction_plane;
    uint16_t window_index, window_count;
    uint32_t window, group_global, resident_hash;
    uint32_t basis_hash[4];
    uint8_t *correction, *canonical_parity;
} PreparedGroup;

typedef struct {
    PreparedGroup slots[SLOT_COUNT];
    uint8_t head, tail;
    unsigned ready;
    uint32_t prepared, prepare_ticks, starvation, gap_ticks;
} PlaneQueue;

typedef struct { unsigned width, window, hold, focus, verify; const char *path; } Options;
typedef struct { uint32_t qr_ticks, encode_ticks, delta_ticks, matrix_ticks, upload_ticks, correction_ticks, select_ticks, hold_ticks; } Metrics;
static Metrics metrics;
static FILE *trace_file;
static uint8_t reverse_bits[256];
static int reverse_bits_ready;

static void trace_event(const char *event, unsigned slot, unsigned before, unsigned plane) {
    if (!trace_file) return;
    fprintf(trace_file, "%s slot=%u before=%02X plane=%u\n", event, slot, before, plane);
    fflush(trace_file);
}

static uint32_t now_ticks(void) { return (uint32_t)clock(); }
static uint32_t ticks_ms(uint32_t t) { return (t * 1000UL) / (uint32_t)CLOCKS_PER_SEC; }

static void qr_raster_from_matrix(WorkMemory *w) {
    unsigned i, y;
    if (!reverse_bits_ready) {
        for (i = 0; i < 256u; ++i) {
            uint8_t v = (uint8_t)i, r = 0; unsigned b;
            for (b = 0; b < 8u; ++b) { r = (uint8_t)((r << 1) | (v & 1u)); v >>= 1; }
            reverse_bits[i] = r;
        }
        reverse_bits_ready = 1;
    }
    memset(w->raster, 0, VGA_RASTER_BYTES);
    /* Matrix bits are LSB-first over one contiguous 177x177 stream (rows are
       not byte aligned); VGA raster bytes are MSB-first and the QR starts at
       pixel (4,4). Read each 8-module chunk at its actual bit offset, reverse
       it, then shift it across the four-pixel horizontal offset. */
    for (y = 0; y < QR_SIZE; ++y) {
        uint8_t *dst = w->raster + (y + 4u) * 40u + 8u;
        for (i = 0; i < 23u; ++i) {
            unsigned bit = y * QR_SIZE + i * 8u;
            const uint8_t *src = w->matrix + 1u + (bit >> 3);
            unsigned shift = bit & 7u;
            uint32_t packed = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                ((uint32_t)src[2] << 16);
            uint8_t v = (uint8_t)(packed >> shift);
            if (i == 22u) v &= 1u;
            v = reverse_bits[v];
            if (i == 0u) {
                dst[0] |= (uint8_t)(v >> 3);
                dst[1] |= (uint8_t)((v & 0x07) << 5);
            } else {
                dst[i] |= (uint8_t)(v >> 4);
                dst[i + 1u] |= (uint8_t)(v << 4);
            }
        }
    }
    /* The fixed odd-parity palette maps a set plane bit to white and a clear
       bit to black.  qrcodegen marks dark modules with 1, so the temporary
       raster above is the photographic inverse of a normal QR (white field,
       black modules).  Invert the complete raster before it reaches VGA;
       Android's fast decoder deliberately does not enable inverted-QRs. */
    for (i = 0; i < VGA_RASTER_BYTES; ++i) w->raster[i] = (uint8_t)~w->raster[i];
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
        off = (unsigned)(y + 4) * 40u + (unsigned)((x + 4) >> 3);
        w->delta_entries[i] = (uint32_t)off | ((uint32_t)(0x80u >> ((x + 4) & 7)) << 16);
    }
    memcpy(w->previous_codewords, w->codewords, QR_CODEWORDS);
    w->delta_ready = 1;
    return 1;
}

static void qr_delta_fallback(WorkMemory *w) {
    unsigned i, bit; uint8_t changed; uint32_t entry;
    for (i = 0; i < QR_CODEWORDS; ++i) {
        changed = (uint8_t)(w->codewords[i] ^ w->previous_codewords[i]);
        w->previous_codewords[i] = w->codewords[i];
        if (!changed) continue;
        for (bit = 0; bit < 8; ++bit) if (changed & (uint8_t)(0x80u >> bit)) {
            entry = w->delta_entries[i * 8u + bit];
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

static void qr_apply_parity_delta(WorkMemory *w) {
#if defined(__WATCOMC__) && defined(DOSFER32)
    memcpy(w->zero_codewords, w->previous_codewords, QR_CODEWORDS);
    dosfer_delta32(w->codewords, w->zero_codewords, w->raster,
                   w->delta_entries, QR_CODEWORDS);
#else
    qr_apply_codeword_delta(w, w->raster, w->previous_codewords, w->codewords);
#endif
}

static void qr_restore_basis_delta(WorkMemory *w) {
#if defined(__WATCOMC__) && defined(DOSFER32)
    dosfer_delta32(w->previous_codewords, w->zero_codewords, w->raster,
                   w->delta_entries, QR_CODEWORDS);
#else
    qr_apply_codeword_delta(w, w->raster, w->codewords, w->previous_codewords);
#endif
}

#if defined(__WATCOMC__) && defined(DOSFER32)
#pragma aux dosfer_delta32 = \
    "test ecx,ecx" \
    "jz d32_done" \
    "mov ebp,edx" \
    "d32_loop:" \
    "mov al,[esi]" \
    "mov dl,[edi]" \
    "mov [edi],al" \
    "xor al,dl" \
    "test al,80h" \
    "jz d32_b1" \
    "movzx edx,word ptr [ebx]" \
    "mov ah,byte ptr [ebx+2]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b1:" \
    "test al,40h" \
    "jz d32_b2" \
    "movzx edx,word ptr [ebx+4]" \
    "mov ah,byte ptr [ebx+6]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b2:" \
    "test al,20h" \
    "jz d32_b3" \
    "movzx edx,word ptr [ebx+8]" \
    "mov ah,byte ptr [ebx+10]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b3:" \
    "test al,10h" \
    "jz d32_b4" \
    "movzx edx,word ptr [ebx+12]" \
    "mov ah,byte ptr [ebx+14]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b4:" \
    "test al,8" \
    "jz d32_b5" \
    "movzx edx,word ptr [ebx+16]" \
    "mov ah,byte ptr [ebx+18]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b5:" \
    "test al,4" \
    "jz d32_b6" \
    "movzx edx,word ptr [ebx+20]" \
    "mov ah,byte ptr [ebx+22]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b6:" \
    "test al,2" \
    "jz d32_b7" \
    "movzx edx,word ptr [ebx+24]" \
    "mov ah,byte ptr [ebx+26]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_b7:" \
    "test al,1" \
    "jz d32_next" \
    "movzx edx,word ptr [ebx+28]" \
    "mov ah,byte ptr [ebx+30]" \
    "xor byte ptr [ebp+edx],ah" \
    "d32_next:" \
    "inc esi" "inc edi" "add ebx,32" "dec ecx" "jnz d32_loop" \
    "d32_done:" \
    parm [esi] [edi] [edx] [ebx] [ecx] \
    modify [eax ebp];
#endif

static int qr_render(WorkMemory *w, const uint8_t *wire, int delta_only) {
    /* PLANE_CODED frames are always fixed-length V40-L records.  Feed the
       already packed ECI/byte header directly to the specialized encoder;
       this bypasses the generic segment planner and its buffer shuffling. */
    w->input[0] = 0x70; w->input[1] = 0x34;
    w->input[2] = 0x0B; w->input[3] = 0x88;
    memcpy(w->input + 4, wire, DOS32_FRAME_BYTES);
    { uint32_t t = now_ticks();
      if (!qrcodegen_dosferEncodePrepackedV40L(w->input, w->codewords)) return 0;
      metrics.encode_ticks += now_ticks() - t; }
    if (delta_only && w->delta_ready) {
        uint32_t t = now_ticks();
#if defined(__WATCOMC__) && defined(DOSFER32)
        dosfer_delta32(w->codewords, w->previous_codewords, w->raster,
                       w->delta_entries, QR_CODEWORDS);
#else
        qr_delta_fallback(w);
#endif
        metrics.delta_ticks += now_ticks() - t;
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
      }
      metrics.matrix_ticks += now_ticks() - t; }
    return 1;
}

static int qr_render_codewords(WorkMemory *w, const uint8_t *codewords) {
    if (w->delta_ready) {
        /* Function modules are invariant for V40-L.  A parity raster can
         * therefore be formed from the resident last-basis raster by toggling
         * only the codeword bits that changed.  Do not advance the basis
         * baseline: the next group starts from C8/C4, not from parity. */
        qr_apply_parity_delta(w);
        return 1;
    }
    if (!qrcodegen_dosferBuildMatrixV40L(codewords, w->matrix,
            qrcodegen_Mask_0)) return 0;
    qr_raster_from_matrix(w);
    return 1;
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
        if (w->disk_pos == w->disk_len) {
            w->disk_len = (unsigned)fread(w->disk_ring, 1, DISK_RING_BYTES, s->file);
            w->disk_pos = 0;
            if (!w->disk_len) break;
        }
        take = w->disk_len - w->disk_pos; if (take > want - got) take = want - got;
        memcpy(dst + got, w->disk_ring + w->disk_pos, take);
        w->disk_pos += (unsigned)take; got += take;
    }
    return got;
}

static int stream_open(RecordStream *s, const char *path, uint32_t session) {
    const char *name = base_name(path); size_t n = strlen(name);
    memset(s, 0, sizeof(*s)); s->file = fopen(path, "rb");
    if (!s->file || n == 0 || n >= sizeof(s->name)) return 0;
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
    ++s->global; return 1;
}

static int prepare_group(PlaneQueue *q, RecordStream *s, WorkMemory *w, Vga32 *vga,
                         unsigned width, unsigned window, int verify) {
    static const uint8_t coefficient[4] = { 1, 2, 4, 8 };
    PreparedGroup *g; uint16_t wi, wc; uint32_t t; unsigned p, i;
    if (q->ready >= SLOT_COUNT || s->global + width > s->total_records) return 0;
    wi = (uint16_t)(s->global % window); wc = (uint16_t)window_count(s, s->global, window);
    if ((unsigned)wi + width > wc) return 0;
    g = &q->slots[q->tail]; if (g->state != SLOT_FREE) return 0;
    g->state = SLOT_FILLING; g->slot = q->tail; g->width = (uint8_t)width; g->window_index = wi; g->window_count = wc;
    g->correction_applied = 0; g->correction_plane = 0;
    g->window = s->global / window; g->group_global = s->global; t = now_ticks();
    memset(w->xor_raster, 0, VGA_RASTER_BYTES); memset(w->parity, 0, DOS32_RECORD_BYTES);
    memset(w->header_xor, 0, DOS32_FRAME_HEADER);
    for (p = 0; p < width; ++p) {
        uint16_t got_wi, got_wc; int rc;
        rc = stream_next(s, w, window, &got_wi, &got_wc); if (rc <= 0 || got_wi != wi + p || got_wc != wc) return 0;
        for (i = 0; i < DOS32_RECORD_BYTES; ++i) w->parity[i] ^= w->records[0][i];
        if (!dos32_plane_frame(w->wire, s->session, g->window, g->group_global, wi,
                wc, (uint8_t)width, coefficient[p], w->records[0])) return 0;
        for (i = 0; i < DOS32_FRAME_HEADER; ++i) w->header_xor[i] ^= w->wire[i];
        { uint32_t a = now_ticks(); if (!qr_render(w, w->wire, p != 0)) return 0; metrics.qr_ticks += now_ticks() - a; }
        if (verify && p != 0) {
            /* Delta rendering is a performance path; compare it against the
             * canonical matrix renderer while the slot is still private. */
            memcpy(g->canonical_parity, w->raster, VGA_RASTER_BYTES);
            if (!qrcodegen_dosferBuildMatrixV40L(w->codewords, w->matrix,
                    qrcodegen_Mask_0)) return 0;
            qr_raster_from_matrix(w);
            if (memcmp(g->canonical_parity, w->raster, VGA_RASTER_BYTES) != 0) return 0;
            memcpy(w->raster, g->canonical_parity, VGA_RASTER_BYTES);
        }
        memcpy(w->basis_codewords + (size_t)p * QR_CODEWORDS, w->codewords, QR_CODEWORDS);
        { uint32_t a = now_ticks();
          if (!vga32_store_fast(vga, p, g->slot, w->raster)) return 0;
          if (verify && !vga32_verify(vga, p, g->slot, w->raster)) return 0;
          metrics.upload_ticks += now_ticks() - a; }
        g->basis_hash[p] = verify ? vga32_hash(vga, p, g->slot) : 0;
        for (i = 0; i < VGA_RASTER_BYTES; ++i) w->xor_raster[i] ^= w->raster[i];
    }
    g->correction_plane = 0; g->resident_hash = g->basis_hash[0];
    { uint32_t a = now_ticks(); uint8_t header_delta[DOS32_FRAME_HEADER];
    if (!dos32_plane_frame(w->wire, s->session, g->window, g->group_global, wi,
            wc, (uint8_t)width, width == 3 ? 7 : 15, w->parity)) return 0;
    for (i = 0; i < DOS32_FRAME_HEADER; ++i) header_delta[i] = w->wire[i] ^ w->header_xor[i];
    memset(w->zero_codewords, 0, QR_CODEWORDS);
    if (!qrcodegen_dosferDeriveXor4V40L(
            w->basis_codewords + 0u * QR_CODEWORDS,
            w->basis_codewords + 1u * QR_CODEWORDS,
            w->basis_codewords + 2u * QR_CODEWORDS,
            width == 4 ? w->basis_codewords + 3u * QR_CODEWORDS : w->zero_codewords,
            header_delta, w->codewords) || !qr_render_codewords(w, w->codewords)) return 0;
    memcpy(g->canonical_parity, w->raster, VGA_RASTER_BYTES);
    for (i = 0; i < VGA_RASTER_BYTES; ++i) g->correction[i] = w->raster[i] ^ w->xor_raster[i];
    metrics.correction_ticks += now_ticks() - a; }
    for (i = 0; i < VGA_RASTER_BYTES; ++i) {
        if ((w->xor_raster[i] ^ g->correction[i]) != w->raster[i]) return 0;
    }
    /* Keep the stateful delta renderer anchored on the last basis symbol;
     * parity was only a temporary private raster used to derive correction. */
    if (w->delta_ready) qr_restore_basis_delta(w);
    g->state = SLOT_READY; ++q->ready; ++q->prepared; q->prepare_ticks += now_ticks() - t;
    q->tail = (uint8_t)((q->tail + 1u) & 7u); return 1;
}

static int show_group(PlaneQueue *q, PreparedGroup *g, RecordStream *s, WorkMemory *w,
                      Vga32 *vga, unsigned width, unsigned window, unsigned hold,
                      int focus, int verify) {
    unsigned symbol, count = g->width + 1u, correction_plane = 0u; uint32_t deadline, start;
    g->state = SLOT_PLAYING; --q->ready;
    for (symbol = 0; symbol < count; ++symbol) {
        uint8_t mask = symbol == g->width ? (uint8_t)((1u << g->width) - 1u) : (uint8_t)(1u << symbol);
        if (mask == ((1u << g->width) - 1u)) {
            trace_event("correction apply begin", g->slot, (1u << (g->width - 1u)), correction_plane);
            if (!vga32_xor(vga, correction_plane, g->slot, g->correction)) return 0;
            g->correction_applied = 1;
            trace_event("correction apply end", g->slot, 0, correction_plane);
            if (verify && !vga32_verify_composed(vga, g->slot, mask, g->canonical_parity)) return 0;
            trace_event("parity show", g->slot, 0, correction_plane);
        }
        { uint32_t a = now_ticks(); if (!vga32_show(vga, g->slot, mask)) return 0; metrics.select_ticks += now_ticks() - a; }
        trace_event("symbol show", g->slot, mask, correction_plane);
        if (symbol == 0 && focus) {
            int key;
            while (!kbhit()) {
                if (q->ready < SLOT_COUNT && !prepare_group(q, s, w, vga, width, window, verify)) delay(1);
            }
            do key = getch(); while (key != 13 && key != 27);
            if (key == 27) return -1;
        }
        start = now_ticks(); deadline = start + (uint32_t)hold * CLOCKS_PER_SEC / 1000UL;
        { uint32_t a = now_ticks(); while ((long)(now_ticks() - deadline) < 0) {
            if (q->ready < SLOT_COUNT && !prepare_group(q, s, w, vga, width, window, verify)) delay(1);
        } metrics.hold_ticks += now_ticks() - a; }
        if (mask == ((1u << g->width) - 1u)) {
            trace_event("parity hold end", g->slot, mask, correction_plane);
            /* Parity still contributes plane 0, so first switch to a complete
               valid QR whose mask excludes the correction plane.  If the next
               slot is not ready yet, keep parity visible while the producer
               finishes it; never expose a black or partially restored raster. */
            if (!q->ready) {
                while (!q->ready && s->global + width <= s->total_records) {
                    if (!prepare_group(q, s, w, vga, width, window, verify)) break;
                }
            }
            if (q->ready) {
                PreparedGroup *next = &q->slots[(q->head + 1u) & 7u];
                if (next->state != SLOT_READY || !vga32_show(vga, next->slot, 1u)) return 0;
                trace_event("next symbol show", next->slot, 0, 0);
            } else {
                /* No future group remains (or the tail is incomplete).  C2 is
                   a resident basis QR and excludes correction plane 0. */
                if (!vga32_show(vga, g->slot, 2u)) return 0;
                trace_event("next symbol show", g->slot, 2u, 0);
            }
            trace_event("correction restore begin", g->slot, 0, correction_plane);
            if (!vga32_xor(vga, correction_plane, g->slot, g->correction)) return 0;
            g->correction_applied = 0;
            trace_event("correction restore end", g->slot, 0, correction_plane);
            if (verify) for (unsigned p = 0; p < g->width; ++p)
                if (vga32_hash(vga, p, g->slot) != g->basis_hash[p]) return 0;
        }
    }
    g->state = SLOT_FREE; return 1;
}

static int show_tail(RecordStream *s, WorkMemory *w, Vga32 *vga, unsigned window, unsigned hold) {
    uint16_t wi, wc; uint32_t global; uint32_t deadline;
    if (stream_next(s, w, window, &wi, &wc) <= 0) return 0;
    global = s->global - 1UL;
    if (!dos32_frame(w->wire, DOS32_DATA, DOS32_FLAG_WHITENED, s->session,
            global / window, global, wi, wc, 0, 0, w->records[0], DOS32_FRAME_PAYLOAD) ||
        !qr_render(w, w->wire, 0) || !vga32_store(vga, 0, 0, w->raster) || !vga32_show(vga, 0, 1)) return 0;
    deadline = now_ticks() + (uint32_t)hold * CLOCKS_PER_SEC / 1000UL;
    while ((long)(now_ticks() - deadline) < 0) {}
    return 1;
}

static void usage(void) { puts("DOSFER32 file [/RE:PLANE3|/RE:PLANE4] [/WINDOW:n] [/HOLD:ms] [/VERIFY]"); }

static void options(int argc, char **argv, Options *o) {
    int i; o->width = 4; o->window = 32; o->hold = HOLD_DEFAULT; o->focus = 1; o->verify = 0; o->path = 0;
    for (i = 1; i < argc; ++i) {
        if (!strnicmp(argv[i], "/RE:PLANE3", 10)) o->width = 3;
        else if (!strnicmp(argv[i], "/RE:PLANE4", 10)) o->width = 4;
        else if (!strnicmp(argv[i], "/WINDOW:", 8)) o->window = (unsigned)atoi(argv[i] + 8);
        else if (!strnicmp(argv[i], "/HOLD:", 6)) o->hold = (unsigned)atoi(argv[i] + 6);
        else if (!stricmp(argv[i], "/NOFOCUS")) o->focus = 0;
        else if (!stricmp(argv[i], "/VERIFY")) o->verify = 1;
        else if (argv[i][0] != '/') o->path = argv[i];
    }
    if (o->window < 4) o->window = 4; if (o->window > MAX_WINDOW32) o->window = MAX_WINDOW32;
}

static void report_profile(const PlaneQueue *q, int completed, unsigned width, uint32_t useful_bytes) {
    FILE *f = fopen("DOSFER32.PRO", "wt");
    unsigned long groups = (unsigned long)q->prepared, symbols = groups * (width + 1u);
    unsigned long prepare_ms = (unsigned long)ticks_ms(q->prepare_ticks);
    unsigned long hold_ms = (unsigned long)ticks_ms(metrics.hold_ticks);
    unsigned long elapsed_ms = prepare_ms + hold_ms;
    unsigned long symbols_per_sec_milli = elapsed_ms ? symbols * 1000000UL / elapsed_ms : 0;
    unsigned long useful_bytes_sec = elapsed_ms ? (unsigned long)useful_bytes * 1000UL / elapsed_ms : 0;
    if (!f) f = stdout;
    fprintf(f, "build=%s\ncompleted=%d\ngroups=%lu\nsymbols=%lu\nprepare_ms=%lu\nprepare_ms_group=%lu\nqr_ms=%lu\nencode_ms=%lu\ndelta_ms=%lu\nmatrix_ms=%lu\nupload_ms=%lu\ncorrection_ms=%lu\nselector_ms=%lu\nhold_ms=%lu\nvisible_ms_symbol=%lu\nsustained_symbols_s=%lu.%03lu\nuseful_bytes_s=%lu\nstarvation=%lu\n",
        DOSFER32_BUILD_ID, completed, groups, symbols, prepare_ms,
        groups ? prepare_ms / groups : 0,
        (unsigned long)ticks_ms(metrics.qr_ticks), (unsigned long)ticks_ms(metrics.encode_ticks),
        (unsigned long)ticks_ms(metrics.delta_ticks), (unsigned long)ticks_ms(metrics.matrix_ticks),
        (unsigned long)ticks_ms(metrics.upload_ticks),
        (unsigned long)ticks_ms(metrics.correction_ticks),
        (unsigned long)ticks_ms(metrics.select_ticks),
        hold_ms, symbols ? hold_ms / symbols : 0, symbols_per_sec_milli / 1000UL, symbols_per_sec_milli % 1000UL, useful_bytes_sec,
        (unsigned long)q->starvation);
    if (f != stdout) fclose(f); else fflush(f);
}

int main(int argc, char **argv) {
    Options o; RecordStream stream; WorkMemory *w; PlaneQueue *q; Vga32 vga; uint32_t session;
    unsigned target, i; int rc = 0, completed = 0, first_group = 1; uint32_t start, end;
    options(argc, argv, &o); if (!o.path) { usage(); return 2; }
    /* Keep the trace name strictly 8.3 so it works on real DOS, not only
       DOSBox's long-name layer. */
    trace_file = fopen("DOSFER32.TRC", "wt");
    session = (uint32_t)time(0) ^ 0xD05F3201UL; if (!session) session = 1;
    printf("DOSFER32 %s (Open Watcom + DOS/4GW)\n", DOSFER32_BUILD_ID);
    printf("Mode: PLANE%u  window=%u  hold=%u ms\n", o.width, o.window, o.hold);
    w = (WorkMemory *)calloc(1, sizeof(*w)); q = (PlaneQueue *)calloc(1, sizeof(*q)); memset(&vga, 0, sizeof(vga));
    if (!w || !q) { puts("DOSFER32: workspace allocation failed"); free(w); free(q); return 2; }
    w->input = (uint8_t *)malloc(QR_DATA_CODEWORDS); w->codewords = (uint8_t *)malloc(QR_CODEWORDS);
    w->basis_codewords = (uint8_t *)malloc(4u * QR_CODEWORDS);
    w->zero_codewords = (uint8_t *)malloc(QR_CODEWORDS);
    w->previous_codewords = (uint8_t *)malloc(QR_CODEWORDS);
    w->delta_entries = (uint32_t *)malloc(8u * QR_CODEWORDS * sizeof(uint32_t));
    w->matrix = (uint8_t *)malloc(QR_BUFFER);
    w->raster = (uint8_t *)malloc(VGA_RASTER_BYTES); w->wire = (uint8_t *)malloc(DOS32_FRAME_BYTES);
    w->header_xor = (uint8_t *)malloc(DOS32_FRAME_HEADER);
    w->parity = (uint8_t *)malloc(DOS32_RECORD_BYTES); w->xor_raster = (uint8_t *)malloc(VGA_RASTER_BYTES); w->body = (uint8_t *)malloc(2880); w->disk_ring = (uint8_t *)malloc(DISK_RING_BYTES);
    for (i = 0; i < 4; ++i) w->records[i] = (uint8_t *)malloc(DOS32_RECORD_BYTES);
    for (i = 0; i < SLOT_COUNT; ++i) {
        q->slots[i].correction = (uint8_t *)malloc(VGA_RASTER_BYTES);
        q->slots[i].canonical_parity = (uint8_t *)malloc(VGA_RASTER_BYTES);
    }
    if (!w->input || !w->codewords || !w->basis_codewords || !w->zero_codewords || !w->previous_codewords || !w->delta_entries || !w->matrix || !w->raster || !w->wire || !w->header_xor || !w->parity || !w->xor_raster || !w->body || !w->disk_ring ||
        !vga32_enter(&vga)) { puts("DOSFER32: protected-mode workspace/VGA allocation failed"); rc = 2; goto done; }
    printf("CRTC slot step=%u words (%u bytes)\n", (unsigned)vga.slot_step,
           (unsigned)vga.slot_step * 2u);
    if (!stream_open(&stream, o.path, session)) { puts("DOSFER32: cannot open source file"); rc = 2; goto done; }
    printf("Records: %lu, free protected memory: %lu bytes\n", (unsigned long)stream.total_records, (unsigned long)_memavl());
    /* Keep two groups ready initially so the producer has free slots to fill
       during the visible focus/hold intervals. It can grow to all eight slots
       cooperatively without blocking startup on a full queue. */
    target = 2u;
    while (q->ready < target && prepare_group(q, &stream, w, &vga, o.width, o.window, o.verify)) {}
    rc = 1;
    while (q->ready) {
        PreparedGroup *g = &q->slots[q->head];
        start = now_ticks(); rc = show_group(q, g, &stream, w, &vga, o.width, o.window, o.hold, first_group && o.focus, o.verify); first_group = 0; if (rc <= 0) break;
        q->head = (uint8_t)((q->head + 1u) & 7u);
        if (!q->ready && stream.global + o.width <= stream.total_records &&
            (stream.global % o.window) + o.width <= window_count(&stream, stream.global, o.window)) ++q->starvation;
        while (q->ready < SLOT_COUNT && prepare_group(q, &stream, w, &vga, o.width, o.window, o.verify)) {}
        end = now_ticks(); (void)start; (void)end;
    }
    while (rc > 0 && stream.global < stream.total_records) {
        if (!show_tail(&stream, w, &vga, o.window, o.hold)) { rc = 0; break; }
    }
    if (rc > 0) completed = 1;
    stream_close(&stream);
done:
    vga32_leave(&vga);
    report_profile(q, completed, o.width, stream.file_size);
    if (completed) puts("DOSFER32 transfer complete");
    if (q) for (i = 0; i < SLOT_COUNT; ++i) { free(q->slots[i].correction); free(q->slots[i].canonical_parity); }
    if (w) { for (i = 0; i < 4; ++i) free(w->records[i]); free(w->input); free(w->codewords); free(w->basis_codewords); free(w->zero_codewords); free(w->previous_codewords); free(w->delta_entries); free(w->matrix); free(w->raster); free(w->wire); free(w->header_xor); free(w->parity); free(w->xor_raster); free(w->body); free(w->disk_ring); }
    free(q); free(w); if (trace_file) { fclose(trace_file); trace_file = 0; } return rc < 0 ? 1 : rc;
}
