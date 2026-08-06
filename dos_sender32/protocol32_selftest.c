#include "protocol32.h"
#include <stdio.h>
#include <string.h>

static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
int main(void) {
    uint8_t payload[DOS32_FRAME_PAYLOAD], canonical[DOS32_FRAME_BYTES];
    uint8_t optimized[DOS32_FRAME_BYTES], key[DOS32_FRAME_PAYLOAD];
    unsigned i, c; uint32_t session = 0x12345678UL, group = 37UL;
    static const uint8_t coefficients[] = { 1, 2, 4, 8, 15 };
    for (i = 0; i < DOS32_FRAME_PAYLOAD; ++i) payload[i] = (uint8_t)(i * 73u + 19u);
    dos32_generate_keystream(key, session, group);
    for (c = 0; c < sizeof(coefficients); ++c) {
        uint8_t width = coefficients[c] == 15 ? 4 : 4;
        if (!dos32_plane_frame(canonical, session, 3, group, 1, 4, width,
                               coefficients[c], payload)) return 1;
        if (!dos32_plane_frame_whitened(optimized, session, 3, group, 1, 4,
                               width, coefficients[c], payload, key)) return 2;
        if (memcmp(canonical, optimized, DOS32_FRAME_BYTES) != 0) {
            for (i = 0; i < DOS32_FRAME_BYTES; ++i)
                if (canonical[i] != optimized[i]) { printf("mismatch coefficient %u at %u\n", coefficients[c], i); break; }
            return 3;
        }
        if (canonical[5] != DOS32_PLANE_CODED || get32(canonical + 16) != coefficients[c] ||
            get32(canonical + 24) != group || get32(canonical + 28) != width ||
            get32(canonical + 40) == 0) return 4;
        if (coefficients[c] == 15 && canonical[6] != 0) return 5;
        if (coefficients[c] != 15 && (canonical[6] != 0x00 || canonical[7] != 0x10)) return 6;
    }
    puts("protocol32 canonical/whitened self-test passed (C1/C2/C4/C8/CF)"); return 0;
}
