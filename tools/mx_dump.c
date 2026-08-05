#include <stdio.h>
#include <string.h>
#include "qrcodegen.h"

int main(int argc, char **argv) {
    uint8_t cw[3706], m[qrcodegen_BUFFER_LEN_FOR_VERSION(40)];
    FILE *f;
    if (argc < 2) return 2;
    f = fopen(argv[1], "rb");
    if (!f || fread(cw, 1, 3706, f) != 3706) return 1;
    fclose(f);
    if (!qrcodegen_dosferBuildMatrixV40L(cw, m, qrcodegen_Mask_0)) return 1;
    fwrite(m, 1, sizeof(m), stdout);
    return 0;
}
