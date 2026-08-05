/* Host RS30 smoke test: portable C V40-L encode only (no DOSBox). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "qrcodegen.h"

#define QR_CODEWORDS 3706u
#define DATA_CODEWORDS 2956u

static void fill_prepacked(uint8_t *in, int variant) {
    unsigned i;
    in[0] = 0x70; in[1] = 0x34; in[2] = 0x0B; in[3] = 0x88;
    for (i = 4; i < DATA_CODEWORDS; ++i) {
        if (variant == 0) in[i] = 0;
        else if (variant == 1) in[i] = 0xFF;
        else if (variant == 2) in[i] = (uint8_t)(i * 73u + 19u);
        else in[i] = (uint8_t)((i * 37u) ^ 0xA5u);
    }
}

int main(void) {
    uint8_t in[DATA_CODEWORDS], cw[QR_CODEWORDS];
    const char *names[] = { "zero", "ff", "seq73", "xor37" };
    int variant, ecc_first = -1, i;

    for (variant = 0; variant < 4; ++variant) {
        fill_prepacked(in, variant);
        if (!qrcodegen_dosferEncodePrepackedV40L(in, cw)) {
            printf("FAIL encode %s\n", names[variant]);
            return 1;
        }
        printf("OK %s: portable encode (%d codewords)\n", names[variant], QR_CODEWORDS);
    }

    for (i = 2956; i < (int)QR_CODEWORDS; ++i) {
        if (cw[i] != 0) { ecc_first = i; break; }
    }
    printf("OK seq73 ECC region populated (first nonzero ECC offset %d)\n",
           ecc_first >= 0 ? ecc_first : 2956);
    return 0;
}
