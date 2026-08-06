/* Host self-test for the fixed V40-L PLANE affine/correction path. */
#include "protocol32.h"
#include "qrcodegen.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QR_SIZE 177u
#define QR_PREFIX 4u
#define QR_DATA_CODEWORDS 2956u
#define QR_CODEWORDS 3706u
#define VGA_RASTER_BYTES 8000u
#define QR_QUIET 4u
#define QR_X0 ((320u - (QR_SIZE + 2u * QR_QUIET)) / 2u + QR_QUIET)

static uint32_t rng_state = 0xC001D00DUL;
static uint32_t rng32(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

static void random_bytes(uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t x = rng32();
        unsigned j;
        for (j = 0; j < 4u && i < n; ++j, ++i) p[i] = (uint8_t)(x >> (j * 8u));
    }
}

static int encode_wire(const uint8_t *wire, uint8_t *codewords) {
    uint8_t input[QR_DATA_CODEWORDS];
    input[0] = 0x70; input[1] = 0x34; input[2] = 0x0B; input[3] = 0x88;
    memcpy(input + QR_PREFIX, wire, DOS32_FRAME_BYTES);
    return qrcodegen_dosferEncodePrepackedV40L(input, codewords) != 0;
}

static int encode_header_delta(const uint8_t *header, uint8_t *codewords) {
    uint8_t input[QR_DATA_CODEWORDS];
    input[0] = 0x70; input[1] = 0x34; input[2] = 0x0B; input[3] = 0x88;
    memcpy(input + QR_PREFIX, header, DOS32_FRAME_HEADER);
    memset(input + QR_PREFIX + DOS32_FRAME_HEADER, 0,
           QR_DATA_CODEWORDS - QR_PREFIX - DOS32_FRAME_HEADER);
    return qrcodegen_dosferEncodePrepackedV40L(input, codewords) != 0;
}

static void matrix_to_raster(const uint8_t *matrix, uint8_t *raster) {
    unsigned y, x;
    memset(raster, 0xFF, VGA_RASTER_BYTES);
    for (y = 0; y < QR_SIZE; ++y) {
        for (x = 0; x < QR_SIZE; ++x) {
            unsigned bit = y * QR_SIZE + x;
            if ((matrix[1u + (bit >> 3)] >> (bit & 7u)) & 1u) {
                unsigned px = QR_X0 + x;
                raster[(y + QR_QUIET) * 40u + (px >> 3)] &=
                    (uint8_t)~(0x80u >> (px & 7u));
            }
        }
    }
}

static int codewords_to_raster(const uint8_t *cw, uint8_t *matrix, uint8_t *raster) {
    if (!qrcodegen_dosferBuildMatrixV40L(cw, matrix, qrcodegen_Mask_0)) return 0;
    matrix_to_raster(matrix, raster);
    return 1;
}

static int first_difference(const uint8_t *a, const uint8_t *b, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) if (a[i] != b[i]) return (int)i;
    return -1;
}

static int run_width(unsigned width, unsigned iteration,
                     uint8_t *matrix, uint8_t **cw, uint8_t **raster) {
    static const uint8_t coefficient[4] = {1, 2, 4, 8};
    uint8_t records[4][DOS32_FRAME_PAYLOAD];
    uint8_t parity[DOS32_FRAME_PAYLOAD];
    uint8_t frames[5][DOS32_FRAME_BYTES];
    uint8_t key[DOS32_FRAME_PAYLOAD];
    uint8_t header_delta[DOS32_FRAME_HEADER];
    uint8_t *correction = cw[5], *zero = cw[6], *expected = cw[7];
    uint8_t *corr_raster = raster[5], *zero_raster = raster[6], *composite = raster[7];
    uint32_t session = 0x12340000UL + iteration;
    uint32_t group = 100u + iteration * 8u;
    unsigned i, j;
    int diff;

    memset(parity, 0, sizeof(parity));
    for (i = 0; i < width; ++i) {
        random_bytes(records[i], sizeof(records[i]));
        for (j = 0; j < sizeof(parity); ++j) parity[j] ^= records[i][j];
    }
    dos32_generate_keystream(key, session, group);
    for (i = 0; i < width; ++i) {
        if (!dos32_plane_frame_whitened(frames[i], session, 7, group, 0, (uint16_t)width,
                                        (uint8_t)width, coefficient[i], records[i], key)) return 0;
        if (!encode_wire(frames[i], cw[i]) || !codewords_to_raster(cw[i], matrix, raster[i])) return 0;
    }
    if (!dos32_plane_frame(frames[width], session, 7, group, 0, (uint16_t)width,
                           (uint8_t)width, width == 3u ? 7u : 15u, parity)) return 0;
    if (!encode_wire(frames[width], cw[4]) || !codewords_to_raster(cw[4], matrix, raster[4])) return 0;

    memcpy(header_delta, frames[width], DOS32_FRAME_HEADER);
    for (i = 0; i < width; ++i)
        for (j = 0; j < DOS32_FRAME_HEADER; ++j) header_delta[j] ^= frames[i][j];

    if (!qrcodegen_dosferHeaderCorrectionV40L(header_delta, correction)) return 0;
    if (!encode_header_delta(header_delta, expected)) return 0;
    diff = first_difference(correction, expected, QR_CODEWORDS);
    if (diff >= 0) {
        printf("header correction mismatch width=%u iteration=%u at=%d\n", width, iteration, diff);
        return 0;
    }

    {
        uint8_t zero_header[DOS32_FRAME_HEADER];
        memset(zero_header, 0, sizeof(zero_header));
        if (!encode_header_delta(zero_header, zero)) return 0;
    }
    memcpy(expected, cw[4], QR_CODEWORDS);
    for (i = 0; i < width; ++i)
        for (j = 0; j < QR_CODEWORDS; ++j) expected[j] ^= cw[i][j];
    if (width == 3u) {
        for (j = 0; j < QR_CODEWORDS; ++j) expected[j] ^= zero[j];
    }
    diff = first_difference(expected, correction, QR_CODEWORDS);
    if (diff >= 0) {
        printf("affine codeword mismatch width=%u iteration=%u at=%d\n", width, iteration, diff);
        return 0;
    }

    /* Recreate the exact VGA correction procedure: width 3 applies only the
       codeword patch; width 4 adds the fixed zero raster as well. */
    if (!codewords_to_raster(cw[5], matrix, corr_raster) ||
        !codewords_to_raster(zero, matrix, zero_raster)) return 0;
    memset(composite, 0, VGA_RASTER_BYTES);
    for (i = 0; i < width; ++i)
        for (j = 0; j < VGA_RASTER_BYTES; ++j) composite[j] ^= raster[i][j];
    if (width == 4u)
        for (j = 0; j < VGA_RASTER_BYTES; ++j) composite[j] ^= zero_raster[j];
    for (j = 0; j < VGA_RASTER_BYTES; ++j)
        composite[j] ^= (uint8_t)(corr_raster[j] ^ zero_raster[j]);
    diff = first_difference(composite, raster[4], VGA_RASTER_BYTES);
    if (diff >= 0) {
        printf("affine raster mismatch width=%u iteration=%u at=%d\n", width, iteration, diff);
        return 0;
    }
    return 1;
}

int main(void) {
    uint8_t *matrix;
    uint8_t *cw[8];
    uint8_t *raster[8];
    unsigned i, iteration;
    matrix = (uint8_t *)malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(40));
    for (i = 0; i < 8u; ++i) {
        cw[i] = (uint8_t *)malloc(QR_CODEWORDS);
        raster[i] = (uint8_t *)malloc(VGA_RASTER_BYTES);
    }
    if (!matrix) return 2;
    for (i = 0; i < 8u; ++i) if (!cw[i] || !raster[i]) return 2;

    for (iteration = 0; iteration < 32u; ++iteration) {
        if (!run_width(3u, iteration, matrix, cw, raster) ||
            !run_width(4u, iteration, matrix, cw, raster)) return 1;
    }
    puts("V40-L PLANE3/PLANE4 affine codeword and raster self-test passed");
    for (i = 0; i < 8u; ++i) { free(cw[i]); free(raster[i]); }
    free(matrix);
    return 0;
}
