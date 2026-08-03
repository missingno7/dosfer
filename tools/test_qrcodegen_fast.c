#include <stdio.h>
#include <string.h>
#include "qrcodegen.h"

extern unsigned char reedSolomonMultiply(unsigned char x, unsigned char y);
extern int getNumDataCodewords(int version, enum qrcodegen_Ecc ecl);
extern long getPenaltyScore(const unsigned char *qrcode);

static unsigned char slow_multiply(unsigned char x, unsigned char y) {
    unsigned int z = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (unsigned char)z;
}

static unsigned long matrix_hash(const unsigned char *qr) {
    unsigned long hash = 2166136261UL;
    int i, len = (qr[0] * qr[0] + 7) / 8 + 1;
    for (i = 0; i < len; i++) { hash ^= qr[i]; hash *= 16777619UL; }
    return hash;
}

int main(void) {
    unsigned int x, y;
    unsigned char data[2400], temp[qrcodegen_BUFFER_LEN_FOR_VERSION(40)];
    unsigned char first[qrcodegen_BUFFER_LEN_FOR_VERSION(40)];
    unsigned char again[qrcodegen_BUFFER_LEN_FOR_VERSION(40)];
    int i;
    unsigned long hash15, hash8, hash40;

    for (x = 0; x < 256; x++)
        for (y = 0; y < 256; y++)
            if (reedSolomonMultiply((unsigned char)x, (unsigned char)y) !=
                    slow_multiply((unsigned char)x, (unsigned char)y)) {
                fprintf(stderr, "GF mismatch at %u,%u\n", x, y);
                return 1;
            }

    for (i = 0; i < (int)sizeof(data); i++)
        data[i] = (unsigned char)(i * 73 + 19);
    memcpy(temp, data, 412);
    if (!qrcodegen_encodeBinary(temp, 412, first,
            qrcodegen_Ecc_MEDIUM, 15, 15, qrcodegen_Mask_0, false)) {
        fputs("v15-M test vector did not fit\n", stderr);
        return 1;
    }
    hash15 = matrix_hash(first);
    memcpy(temp, data, 412);
    if (!qrcodegen_encodeBinary(temp, 412, again,
            qrcodegen_Ecc_MEDIUM, 15, 15, qrcodegen_Mask_0, false) ||
            memcmp(first, again, (size_t)((first[0] * first[0] + 7) / 8 + 1)) != 0) {
        fputs("cached encode changed QR output\n", stderr);
        return 1;
    }
    memcpy(temp, data, 152);
    if (!qrcodegen_encodeBinary(temp, 152, first,
            qrcodegen_Ecc_MEDIUM, 8, 8, qrcodegen_Mask_0, false)) return 1;
    hash8 = matrix_hash(first);
    memcpy(temp, data, 2331);
    if (!qrcodegen_encodeBinary(temp, 2331, first,
            qrcodegen_Ecc_MEDIUM, 40, 40, qrcodegen_Mask_0, false)) return 1;
    hash40 = matrix_hash(first);
    printf("qrcodegen checks passed, FNV v8=%08lX v15=%08lX v40=%08lX\n",
        hash8, hash15, hash40);
    for(i=0;i<8;i++){
        memcpy(temp,data,2331);
        if(!qrcodegen_encodeBinary(temp,2331,first,qrcodegen_Ecc_MEDIUM,40,40,
                (enum qrcodegen_Mask)i,false))return 1;
        printf("mask%d=%08lX/%ld%s",i,matrix_hash(first),getPenaltyScore(first),i==7?"\n":" ");
    }
    {static const int versions[]={8,10,12,15,20,25,30,35,40};
     for(i=0;i<9;i++){int ver=versions[i],cw=getNumDataCodewords(ver,qrcodegen_Ecc_MEDIUM);
         int binary=cw-(ver<10?2:3);printf("v%d dataCW=%d protocolPayload=%d\n",ver,cw,binary-48);}}
    return 0;
}
