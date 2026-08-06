/* Host oracle for the delta-first V40-L recurrence. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../dos_sender/third_party/qrcodegen.h"

#define DATA 2956u
#define CW 3706u
#define BLOCKS 25u
#define SHORT 118u
#define LONG 119u
#define ECC 30u

static unsigned data_off(unsigned b) { return b <= 19u ? b * SHORT : 19u * SHORT + (b - 19u) * LONG; }
static unsigned data_len(unsigned b) { return b < 19u ? SHORT : LONG; }
static unsigned data_cw(unsigned b, unsigned j) { return j < SHORT ? b + j * BLOCKS : 2950u + b - 19u; }
static unsigned ecc_cw(unsigned b, unsigned j) { return DATA + b + j * BLOCKS; }

static void delta_codewords(const uint8_t *before, const uint8_t *after, uint8_t *out) {
    const uint8_t *step = qrcodegen_dosferRsStep();
    unsigned b, j, k;
    memset(out, 0, CW);
    for (b = 0; b < BLOCKS; ++b) {
        uint8_t ecc[ECC];
        memset(ecc, 0, sizeof(ecc));
        for (j = 0; j < data_len(b); ++j) {
            uint8_t delta = (uint8_t)(before[data_off(b) + j] ^ after[data_off(b) + j]);
            uint8_t next[ECC];
            const uint8_t *row = step + (unsigned)(delta ^ ecc[0]) * DOSFER_RS_STRIDE_EXPOSED;
            for (k = 0; k < ECC; ++k) next[k] = (uint8_t)((k + 1u < ECC ? ecc[k + 1u] : 0u) ^ row[k]);
            memcpy(ecc, next, sizeof(ecc));
            out[data_cw(b, j)] = delta;
        }
        for (j = 0; j < ECC; ++j) out[ecc_cw(b, j)] = ecc[j];
    }
}

static void fill(uint8_t *p, unsigned kind, uint32_t seed) {
    unsigned i;
    p[0] = 0x70; p[1] = 0x34; p[2] = 0x0B; p[3] = 0x88;
    for (i = 4; i < DATA; ++i) {
        if (kind == 0) p[i] = 0;
        else if (kind == 1) p[i] = 0xFF;
        else { seed ^= seed << 13; seed ^= seed >> 17; p[i] = (uint8_t)seed; }
    }
}

static int check(const uint8_t *before, const uint8_t *after, const char *name) {
    uint8_t a[CW], b[CW], d[CW];
    unsigned i;
    uint8_t before_copy[DATA], after_copy[DATA];
    memcpy(before_copy, before, DATA); memcpy(after_copy, after, DATA);
    if (!qrcodegen_dosferEncodePrepackedV40L(before_copy, a) ||
        !qrcodegen_dosferEncodePrepackedV40L(after_copy, b)) return 0;
    delta_codewords(before, after, d);
    for (i = 0; i < CW; ++i) if ((uint8_t)(a[i] ^ b[i]) != d[i]) {
        printf("FAIL %s at codeword %u: canonical=%02X delta=%02X\n", name, i,
               (unsigned)(a[i] ^ b[i]), (unsigned)d[i]);
        return 0;
    }
    return 1;
}

int main(void) {
    uint8_t before[DATA], after[DATA];
    unsigned i, cases = 0;
    int ok = 1;
    fill(before, 0, 1); fill(after, 0, 1); ok &= check(before, after, "identical"); ++cases;
    fill(after, 1, 1); ok &= check(before, after, "zero-to-FF"); ++cases;
    fill(before, 2, 0x12345678UL); fill(after, 2, 0x9ABCDEF0UL); ok &= check(before, after, "pseudorandom"); ++cases;
    for (i = 0; i < DATA; i += 17u) {
        memcpy(after, before, DATA); after[i] ^= (uint8_t)(1u << (i & 7u));
        if (!check(before, after, "single-bit sample")) ok = 0; ++cases;
    }
    for (i = 0; i < DATA; i += 31u) {
        memcpy(after, before, DATA); after[i] ^= (uint8_t)(0xA5u + i);
        if (!check(before, after, "single-byte sample")) ok = 0; ++cases;
    }
    /* Exercise the long-block boundary explicitly. */
    for (i = 2240u; i < DATA; ++i) { memcpy(after, before, DATA); after[i] ^= 0x5Au; if (!check(before, after, "long-block boundary")) ok = 0; ++cases; }
    printf("V40-L delta oracle: %s (%u cases, 3706 codewords each)\n", ok ? "PASS" : "FAIL", cases);
    return ok ? 0 : 1;
}
