/* Host-side oracle for the fixed V40-L pack/ECC/sequential-emitter path.
 * Build from the project root with a C99 compiler, for example:
 *
 *   cc -O2 -std=c99 -Ithird_party tests/test_v40_stream.c \
 *      third_party/qrcodegen.c -o test_v40_stream
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "qrcodegen.h"

#define QR_SIZE 177
#define QR_QUIET 4
#define QR_X0 ((320-(QR_SIZE+QR_QUIET*2))/2)
#define QR_Y0 QR_QUIET
#define STRIDE 40
#define RASTER_BYTES 8000
#define CODEWORDS 3706
#define DATA_CODEWORDS 2956
#define ECC_BYTES 750

static uint32_t rng_state=0x12345678u;
static const uint16_t block_offset[25]={
    0,118,236,354,472,590,708,826,944,1062,
    1180,1298,1416,1534,1652,1770,1888,2006,2124,
    2242,2361,2480,2599,2718,2837
};

static uint32_t next_random(void) {
    rng_state^=rng_state<<13;
    rng_state^=rng_state>>17;
    rng_state^=rng_state<<5;
    return rng_state;
}

static void emit_codewords(const uint8_t *data,const uint8_t *ecc,uint8_t *out) {
    unsigned row,block,cw=0;
    for(row=0;row<118;row++)
        for(block=0;block<25;block++)
            out[cw++]=data[block_offset[block]+row];
    for(block=19;block<25;block++)
        out[cw++]=data[block_offset[block]+118];
    for(row=0;row<30;row++)
        for(block=0;block<25;block++)
            out[cw++]=ecc[block*30+row];
    if(cw!=CODEWORDS)abort();
}

static void matrix_to_raster(const uint8_t *qr,uint8_t *raster) {
    int y,x,index=0,px;
    memset(raster,0xFF,RASTER_BYTES);
    for(y=0;y<QR_SIZE;y++)for(x=0;x<QR_SIZE;x++,index++) {
        if((qr[(index>>3)+1]>>(index&7))&1) {
            px=QR_X0+QR_QUIET+x;
            raster[(QR_Y0+y)*STRIDE+(px>>3)]&=
                (uint8_t)~(0x80>>(px&7));
        }
    }
}

static void apply_delta(const uint8_t *next,uint8_t *current,
        uint8_t *raster,const uint16_t *modules) {
    unsigned i,bit;
    for(i=0;i<CODEWORDS;i++) {
        uint8_t changed=(uint8_t)(next[i]^current[i]);
        current[i]=next[i];
        for(bit=0;bit<8;bit++)if(changed&(uint8_t)(0x80>>bit)) {
            uint16_t pos=modules[i*8+bit];
            int y=pos/QR_SIZE,x=pos-y*QR_SIZE,px=QR_X0+QR_QUIET+x;
            raster[(QR_Y0+y)*STRIDE+(px>>3)]^=
                (uint8_t)(0x80>>(px&7));
        }
    }
}

int main(void) {
    uint8_t *frame=malloc(2952),*canonical=malloc(CODEWORDS),
        *emitted=malloc(CODEWORDS),*current=malloc(CODEWORDS),
        *workspace=malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(40)),
        *qr=malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(40)),
        *raster=malloc(RASTER_BYTES),*expected=malloc(RASTER_BYTES);
    uint8_t *data=workspace,*ecc=workspace+DATA_CODEWORDS;
    int mask,test,i;

    if(!frame||!canonical||!emitted||!current||!workspace||!qr||
            !raster||!expected)return 2;

    for(mask=0;mask<8;mask++) {
        uint16_t *modules;
        int bits;
        for(i=0;i<2952;i++)frame[i]=(uint8_t)next_random();
        if(!qrcodegen_dosferEncodeFrameV40L(frame,2952,canonical,qr,
                (enum qrcodegen_Mask)mask,false))return 3;
        qrcodegen_dosferPackFrameV40L(frame,2952,data);
        qrcodegen_dosferComputeEccBlocksV40L(data,ecc);
        emit_codewords(data,ecc,emitted);
        if(memcmp(canonical,emitted,CODEWORDS))return 4;

        bits=qrcodegen_dosferPlacementBits();
        modules=qrcodegen_dosferTakePlacementModules();
        if(bits!=CODEWORDS*8||!modules)return 5;
        qrcodegen_dosferReleaseMatrixCache();
        memcpy(current,canonical,CODEWORDS);
        matrix_to_raster(qr,raster);

        for(test=0;test<250;test++) {
            uint16_t len=(uint16_t)(next_random()%2953u);
            for(i=0;i<len;i++)frame[i]=(uint8_t)next_random();
            if(!qrcodegen_dosferEncodeFrameV40L(frame,len,canonical,qr,
                    (enum qrcodegen_Mask)mask,false))return 6;
            qrcodegen_dosferPackFrameV40L(frame,len,data);
            qrcodegen_dosferComputeEccBlocksV40L(data,ecc);
            emit_codewords(data,ecc,emitted);
            if(memcmp(canonical,emitted,CODEWORDS))return 7;
            apply_delta(emitted,current,raster,modules);
            matrix_to_raster(qr,expected);
            if(memcmp(raster,expected,RASTER_BYTES))return 8;
        }
        free(modules);
        qrcodegen_dosferReleaseMatrixCache();
    }

    puts("PASS: fixed V40-L emitter matches canonical codewords and raster");
    return 0;
}
