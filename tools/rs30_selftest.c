#include <stdio.h>
#include "../dos_sender/third_party/qrcodegen.h"

int main(void) {
#ifdef DOSFER_RS30_TEST
    int failures = qrcodegen_dosferRs30SelfTest();
    return failures ? 1 : 0;
#else
    fputs("build with -DDOSFER_RS30_TEST\n", stderr);
    return 2;
#endif
}
