#include <stdio.h>
#include <string.h>
#include "qrcodegen.h"

static void save_pgm(const char *path, const uint8_t *qrcode, int border, int scale) {
    int size = qrcode[0];
    int total = size + 2 * border;
    int width = total * scale;
    FILE *f = fopen(path, "wb");
    int y, x, sy, sx;
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", width, width);
    for (y = 0; y < total; ++y) {
        for (sy = 0; sy < scale; ++sy) {
            for (x = 0; x < total; ++x) {
                int dark = 0;
                if (x >= border && x < border + size && y >= border && y < border + size)
                    dark = qrcodegen_getModule(qrcode, x - border, y - border) ? 1 : 0;
                for (sx = 0; sx < scale; ++sx)
                    fputc(dark ? 0 : 255, f);
            }
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    uint8_t codewords[3706];
    uint8_t matrix[qrcodegen_BUFFER_LEN_FOR_VERSION(40)];
    FILE *f;
    size_t n;
    char out[512];
    const char *dump_dir;

    if (argc < 2) return 2;
    dump_dir = argv[1];
    sprintf(out, "%s/G000CW0.BIN", dump_dir);
    f = fopen(out, "rb");
    if (!f || fread(codewords, 1, 3706, f) != 3706) return 1;
    fclose(f);
    if (!qrcodegen_dosferBuildMatrixV40L(codewords, matrix, qrcodegen_Mask_0)) return 1;
    sprintf(out, "%s/plane0_matrix.pgm", dump_dir);
    save_pgm(out, matrix, 8, 6);
    printf("wrote %s\n", out);
    return 0;
}
