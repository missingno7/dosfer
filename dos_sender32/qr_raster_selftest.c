/* Host self-test: compare QR matrix -> VGA raster paths. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "../dos_sender/third_party/qrcodegen.h"

#define QR_SIZE 177u
#define VGA_RASTER_BYTES 8000u

static uint8_t reverse_bits[256];

static void init_reverse_bits(void) {
    unsigned i, b;
    for (i = 0; i < 256u; ++i) {
        uint8_t v = (uint8_t)i, r = 0;
        for (b = 0; b < 8u; ++b) { r = (uint8_t)((r << 1) | (v & 1u)); v >>= 1; }
        reverse_bits[i] = r;
    }
}

static void raster_reference(const uint8_t *qr, uint8_t *raster, int invert) {
    int n = QR_SIZE, total = n + 8, x0 = (320 - total) / 2, my, mx, index, dark, px;
    uint8_t mask;
    memset(raster, invert ? 0x00 : 0xFF, VGA_RASTER_BYTES);
    for (my = 0; my < n; ++my) {
        index = my * n;
        for (mx = 0; mx < n; ++mx) {
            dark = (qr[(index >> 3) + 1] >> (index & 7)) & 1;
            ++index;
            if (!dark) continue;
            px = x0 + 4 + mx;
            mask = (uint8_t)(0x80 >> (px & 7));
            if (invert) raster[(4 + my) * 40 + (px >> 3)] |= mask;
            else raster[(4 + my) * 40 + (px >> 3)] &= (uint8_t)~mask;
        }
    }
}

static void raster_per_module_centered(const uint8_t *matrix, uint8_t *raster) {
    unsigned y, i;
    memset(raster, 0, VGA_RASTER_BYTES);
    for (y = 0; y < QR_SIZE; ++y) {
        uint8_t *dst = raster + (y + 4u) * 40u;
        for (i = 0; i < QR_SIZE; ++i) {
            unsigned bit = y * QR_SIZE + i;
            const uint8_t *src = matrix + 1u + (bit >> 3);
            unsigned shift = bit & 7u;
            uint8_t v = (uint8_t)((src[0] >> shift) & 1u);
            unsigned px = 71u + i;
            unsigned byte_offset = px >> 3;
            unsigned bit_offset = px & 7u;
            uint8_t mask = (uint8_t)(0x80u >> bit_offset);
            if (v) dst[byte_offset] |= mask;
        }
    }
    for (i = 0; i < VGA_RASTER_BYTES; ++i) raster[i] = (uint8_t)~raster[i];
}

static void raster_batched_left(const uint8_t *matrix, uint8_t *raster) {
    unsigned y, i;
    memset(raster, 0, VGA_RASTER_BYTES);
    for (y = 0; y < QR_SIZE; ++y) {
        uint8_t *dst = raster + (y + 4u) * 40u;
        for (i = 0; i < 23u; ++i) {
            unsigned bit = y * QR_SIZE + i * 8u;
            const uint8_t *src = matrix + 1u + (bit >> 3);
            unsigned shift = bit & 7u;
            uint32_t packed = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                ((uint32_t)src[2] << 16);
            uint8_t v = (uint8_t)(packed >> shift);
            if (i == 22u) v &= 1u;
            v = reverse_bits[v];
            dst[i] |= (uint8_t)(v >> 4);
            dst[i + 1u] |= (uint8_t)(v << 4);
        }
    }
    for (i = 0; i < VGA_RASTER_BYTES; ++i) raster[i] = (uint8_t)~raster[i];
}

static int raster_diff(const uint8_t *a, const uint8_t *b, int *first_diff) {
    unsigned i;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) {
        if (a[i] != b[i]) { *first_diff = (int)i; return (int)(a[i] ^ b[i]); }
    }
    *first_diff = -1;
    return 0;
}

static int make_test_codewords(uint8_t *codewords) {
    uint8_t input[2956];
    unsigned i;
    input[0] = 0x70; input[1] = 0x34; input[2] = 0x0B; input[3] = 0x88;
    for (i = 4; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 37u + 11u);
    return qrcodegen_dosferEncodePrepackedV40L(input, codewords);
}

int main(void) {
    uint8_t *matrix, *cw, *r_ref, *r_pm, *r_bat;
    int diff, at;

    init_reverse_bits();
    matrix = (uint8_t *)malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(40));
    cw = (uint8_t *)malloc(3706);
    r_ref = (uint8_t *)malloc(VGA_RASTER_BYTES);
    r_pm = (uint8_t *)malloc(VGA_RASTER_BYTES);
    r_bat = (uint8_t *)malloc(VGA_RASTER_BYTES);
    if (!matrix || !cw || !r_ref || !r_pm || !r_bat) return 2;

    if (!make_test_codewords(cw)) { puts("encode failed"); return 1; }
    if (!qrcodegen_dosferBuildMatrixV40L(cw, matrix, qrcodegen_Mask_0)) {
        puts("matrix failed");
        return 1;
    }

    raster_reference(matrix, r_ref, 0);
    raster_per_module_centered(matrix, r_pm);
    raster_batched_left(matrix, r_bat);

    diff = raster_diff(r_ref, r_pm, &at);
    printf("reference vs per-module centered: diff=%d at=%d row=%d col_byte=%d\n",
           diff, at, at >= 0 ? at / 40 : -1, at >= 0 ? at % 40 : -1);

    diff = raster_diff(r_ref, r_bat, &at);
    printf("reference vs batched left: diff=%d at=%d row=%d col_byte=%d\n",
           diff, at, at >= 0 ? at / 40 : -1, at >= 0 ? at % 40 : -1);

    diff = raster_diff(r_pm, r_bat, &at);
    printf("per-module centered vs batched left: diff=%d at=%d\n", diff, at);

    free(matrix); free(cw); free(r_ref); free(r_pm); free(r_bat);
    return 0;
}
