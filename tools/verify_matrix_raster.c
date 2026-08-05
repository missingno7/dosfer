#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrcodegen.h"

#define QR_SIZE 177u
#define QR_QUIET 4u
#define QR_X0 ((320u - (QR_SIZE + 2u * QR_QUIET)) / 2u + QR_QUIET)
#define VGA_RASTER_BYTES 8000u
#define QR_CODEWORDS 3706u
#define DOS32_FRAME_HEADER 48u

static void qr_raster_from_matrix(const uint8_t *matrix, uint8_t *raster) {
    unsigned x, y, i;
    memset(raster, 0, VGA_RASTER_BYTES);
    for (y = 0; y < QR_SIZE; ++y) {
        uint8_t *dst = raster + (y + QR_QUIET) * 40u;
        for (x = 0; x < QR_SIZE; ++x) {
            unsigned px;
            if (!qrcodegen_getModule(matrix, (int)x, (int)y)) continue;
            px = QR_X0 + x;
            dst[px >> 3] |= (uint8_t)(0x80u >> (px & 7u));
        }
    }
    for (i = 0; i < VGA_RASTER_BYTES; ++i) raster[i] = (uint8_t)~raster[i];
}

static int load_file(const char *path, uint8_t *buf, size_t cap, size_t *len) {
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, cap, f);
    fclose(f);
    *len = n;
    return 1;
}

static unsigned mask_agree_left(const uint8_t *raster) {
    unsigned x, y, agree = 0, total = 0;
    for (y = 0; y < QR_SIZE; ++y) {
        for (x = 0; x < 44u; ++x) {
            unsigned px = QR_X0 + x, py = QR_QUIET + y;
            unsigned bit = (raster[py * 40u + (px >> 3)] >> (7u - (px & 7u))) & 1u;
            total++;
            if (bit == (unsigned)((x + y) & 1u)) agree++;
        }
    }
    printf("left-quarter mask0 agreement: %u/%u (%.1f%%)\n", agree, total,
           total ? 100.0 * agree / total : 0.0);
    return agree;
}

static void xor_rasters(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    unsigned i;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) out[i] = (uint8_t)(a[i] ^ b[i]);
}

static void xor_codewords4(const uint8_t *a, const uint8_t *b, const uint8_t *c,
                           const uint8_t *d, uint8_t *out) {
    unsigned i;
    for (i = 0; i < QR_CODEWORDS; ++i) out[i] = (uint8_t)(a[i] ^ b[i] ^ c[i] ^ d[i]);
}

static int recompute_header_xor(const char *dump_dir, uint8_t *header_xor) {
    /* Without dumped wire frames, header correction cannot be replayed exactly. */
    (void)dump_dir;
    memset(header_xor, 0, DOS32_FRAME_HEADER);
    return 0;
}

int main(int argc, char **argv) {
    uint8_t codewords[QR_CODEWORDS], matrix[qrcodegen_BUFFER_LEN_FOR_VERSION(40)];
    uint8_t raster[VGA_RASTER_BYTES], dumped[VGA_RASTER_BYTES];
    size_t len;
    unsigned diffs, i;
    const char *dump_dir;

    if (argc < 2) {
        fputs("usage: verify_matrix_raster <dump_dir>\n", stderr);
        return 2;
    }
    dump_dir = argv[1];
    {
        char path[512];
        sprintf(path, "%s/G000CW0.BIN", dump_dir);
        if (!load_file(path, codewords, sizeof(codewords), &len) || len != QR_CODEWORDS) {
            fprintf(stderr, "bad codewords: %s\n", path);
            return 1;
        }
        sprintf(path, "%s/G000T0.RAW", dump_dir);
        if (!load_file(path, dumped, sizeof(dumped), &len) || len != VGA_RASTER_BYTES) {
            fprintf(stderr, "bad raster: %s\n", path);
            return 1;
        }
    }
    if (!qrcodegen_dosferBuildMatrixV40L(codewords, matrix, qrcodegen_Mask_0)) {
        fputs("matrix build failed\n", stderr);
        return 1;
    }
    qr_raster_from_matrix(matrix, raster);
    diffs = 0;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) if (raster[i] != dumped[i]) diffs++;
    printf("host rebuild vs G000T0.RAW: %u/%u byte diffs\n", diffs, VGA_RASTER_BYTES);
    printf("rebuilt raster: "); mask_agree_left(raster);
    printf("dumped  raster: "); mask_agree_left(dumped);

    {
        uint8_t cw[4][QR_CODEWORDS], parity_cw[QR_CODEWORDS], header_xor[DOS32_FRAME_HEADER];
        uint8_t planes[4][VGA_RASTER_BYTES], xor_raster[VGA_RASTER_BYTES];
        uint8_t parity_raster[VGA_RASTER_BYTES], correction[VGA_RASTER_BYTES];
        uint8_t s4[VGA_RASTER_BYTES];
        unsigned p, plane_diffs = 0;
        char path[512];
        memset(header_xor, 0, sizeof(header_xor));
        for (p = 0; p < 4u; ++p) {
            sprintf(path, "%s/G000CW%u.BIN", dump_dir, p);
            if (!load_file(path, cw[p], QR_CODEWORDS, &len) || len != QR_CODEWORDS) {
                fprintf(stderr, "bad codewords plane %u\n", p);
                return 1;
            }
            sprintf(path, "%s/G000T%u.RAW", dump_dir, p);
            if (!load_file(path, planes[p], VGA_RASTER_BYTES, &len) || len != VGA_RASTER_BYTES) {
                fprintf(stderr, "bad plane raster %u\n", p);
                return 1;
            }
            plane_diffs += p;
        }
        (void)plane_diffs;
        memset(header_xor, 0, sizeof(header_xor));
        recompute_header_xor(dump_dir, header_xor);
        if (!qrcodegen_dosferDeriveXor4V40L(cw[0], cw[1], cw[2], cw[3], header_xor, parity_cw)) {
            fputs("parity derive failed\n", stderr);
            return 1;
        }
        if (!qrcodegen_dosferBuildMatrixV40L(parity_cw, matrix, qrcodegen_Mask_0)) {
            fputs("parity matrix build failed\n", stderr);
            return 1;
        }
        qr_raster_from_matrix(matrix, parity_raster);
        memset(xor_raster, 0, VGA_RASTER_BYTES);
        for (p = 0; p < 4u; ++p) xor_rasters(xor_raster, planes[p], xor_raster);
        xor_rasters(parity_raster, xor_raster, correction);
        printf("parity raster: "); mask_agree_left(parity_raster);
        printf("correction raster (derived): "); mask_agree_left(correction);
        sprintf(path, "%s/G000S4.RAW", dump_dir);
        if (!load_file(path, s4, VGA_RASTER_BYTES, &len) || len != VGA_RASTER_BYTES) {
            fprintf(stderr, "bad S4 raster\n");
            return 1;
        }
        diffs = 0;
        for (i = 0; i < VGA_RASTER_BYTES; ++i) if (parity_raster[i] != s4[i]) diffs++;
        printf("derived parity raster vs G000S4.RAW: %u/%u byte diffs\n", diffs, VGA_RASTER_BYTES);
        printf("G000S4 parity: "); mask_agree_left(s4);
    }
    return diffs ? 1 : 0;
}
