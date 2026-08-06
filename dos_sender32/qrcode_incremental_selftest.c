/* Host self-test: cooperative fixed V40-L encoder must exactly match the
 * canonical generic QR ECC/interleave implementation. */
#include "qrcodegen.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_CODEWORDS 2956u
#define ALL_CODEWORDS 3706u

/* qrcodegen.c exposes this under its `testable` linkage macro. */
void addEccAndInterleave(uint8_t data[], int version,
                         enum qrcodegen_Ecc ecl, uint8_t result[]);

static uint32_t state = 0x74B1D23FUL;
static uint32_t rng32(void) {
    uint32_t x = state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    state = x;
    return x;
}

static void fill(uint8_t *data, unsigned length) {
    unsigned i = 0;
    while (i < length) {
        uint32_t x = rng32();
        unsigned j;
        for (j = 0; j < 4u && i < length; ++j, ++i)
            data[i] = (uint8_t)(x >> (j * 8u));
    }
}

int main(void) {
    uint8_t *generic_data = (uint8_t *)malloc(DATA_CODEWORDS);
    uint8_t *step_data = (uint8_t *)malloc(DATA_CODEWORDS);
    uint8_t *generic = (uint8_t *)malloc(ALL_CODEWORDS);
    uint8_t *step = (uint8_t *)malloc(ALL_CODEWORDS);
    unsigned iteration;
    if (!generic_data || !step_data || !generic || !step) return 2;

    for (iteration = 0; iteration < 128u; ++iteration) {
        qrcodegen_dosferV40LEncoder encoder;
        int rc, steps = 0;
        fill(generic_data, DATA_CODEWORDS);
        memcpy(step_data, generic_data, DATA_CODEWORDS);
        addEccAndInterleave(generic_data, 40, qrcodegen_Ecc_LOW, generic);
        qrcodegen_dosferV40LBegin(&encoder, step_data, step);
        do {
            rc = qrcodegen_dosferV40LStep(&encoder);
            ++steps;
        } while (rc == 0);
        if (rc < 0 || steps != 25 || memcmp(generic, step, ALL_CODEWORDS) != 0) {
            unsigned i;
            for (i = 0; i < ALL_CODEWORDS && generic[i] == step[i]; ++i) {}
            printf("incremental V40-L mismatch iteration=%u steps=%d byte=%u\n",
                   iteration, steps, i);
            return 1;
        }
    }
    puts("cooperative V40-L encoder matches canonical interleave (128 cases)");
    free(generic_data); free(step_data); free(generic); free(step);
    return 0;
}
