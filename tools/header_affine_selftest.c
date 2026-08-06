#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../dos_sender/third_party/qrcodegen.h"

#define HEADER 48u
#define CW 3706u

int main(void) {
    uint8_t base[CW], raw[CW], fast[CW], header[HEADER];
    uint8_t basis[384u][CW];
    unsigned bit, i, round;
    memset(header, 0, sizeof(header));
    if (!qrcodegen_dosferHeaderCorrectionV40L(header, base)) return 1;
    for (bit = 0; bit < 384u; ++bit) {
        memset(header, 0, sizeof(header)); header[bit >> 3] = (uint8_t)(1u << (bit & 7u));
        if (!qrcodegen_dosferHeaderCorrectionV40L(header, raw)) return 2;
        for (i = 0; i < CW; ++i) basis[bit][i] = (uint8_t)(raw[i] ^ base[i]);
    }
    for (round = 0; round < 32u; ++round) {
        uint32_t seed = 0x9E3779B9UL ^ round;
        for (i = 0; i < HEADER; ++i) { seed ^= seed << 13; seed ^= seed >> 17; header[i] = (uint8_t)seed; }
        if (!qrcodegen_dosferHeaderCorrectionV40L(header, raw)) return 3;
        memcpy(fast, base, CW);
        for (bit = 0; bit < 384u; ++bit) if (header[bit >> 3] & (uint8_t)(1u << (bit & 7u)))
            for (i = 0; i < CW; ++i) fast[i] ^= basis[bit][i];
        if (memcmp(raw, fast, CW) != 0) { puts("header affine oracle: FAIL"); return 4; }
    }
    puts("header affine oracle: PASS (zero, 384 basis bits, 32 random headers)");
    return 0;
}
