/* Host oracle for RGB3 stride-3 wire parity and encoded QR derivation. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dosfer.h"
#include "qrcodegen.h"

#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(40)

static uint32_t state=0x51A7E123u;
static uint32_t next_random(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}

static void xor_codewords(uint8_t *out,const uint8_t *a,const uint8_t *b) {
    unsigned i;
    for(i=0;i<DOSFER_QR_CODEWORDS;++i)out[i]=(uint8_t)(a[i]^b[i]);
}

int main(void) {
    static const uint16_t lengths[]={0,1,3,4,47,128,1024,2904};
    static const uint8_t fixed_stride3[16]={
        0x3C,0x72,0x76,0xA9,0x24,0xF5,0x09,0x81,
        0xB2,0x74,0x08,0x4A,0x5D,0xC9,0x5C,0xD7
    };
    uint8_t *a=malloc(MAX_FRAME_PAYLOAD),*b=malloc(MAX_FRAME_PAYLOAD),
        *c=malloc(MAX_FRAME_PAYLOAD),*parity=malloc(MAX_FRAME_PAYLOAD);
    uint8_t *raw_a=malloc(DOSFER_MAX_FRAME_BYTES),*raw_b=malloc(DOSFER_MAX_FRAME_BYTES),
        *raw_c=malloc(DOSFER_MAX_FRAME_BYTES),*raw_p=malloc(DOSFER_MAX_FRAME_BYTES);
    uint8_t *cw_a=malloc(DOSFER_QR_CODEWORDS),*cw_b=malloc(DOSFER_QR_CODEWORDS),
        *cw_c=malloc(DOSFER_QR_CODEWORDS),*cw_p=malloc(DOSFER_QR_CODEWORDS),
        *derived=malloc(DOSFER_QR_CODEWORDS),*workspace=malloc(QR_BUFFER+1);
    uint8_t header_xor[FRAME_HEADER_SIZE],zero[16]={0};
    unsigned l,mask,i;
    uint16_t n,raw_a_len,raw_b_len,raw_c_len;
    const uint32_t session=0x6A67C69Du,global=120;
    if(!a||!b||!c||!parity||!raw_a||!raw_b||!raw_c||!raw_p||!cw_a||!cw_b||
       !cw_c||!cw_p||!derived||!workspace)return 2;

    /* The RGB channels form independent stride-3 equations across three
     * physical data images: 120,123,126 rather than 120,121,122. */
    n=make_frame(raw_p,FK_BLOCK_XOR,FF_GROUP_XOR_WHITENED,session,0,global,
        0,66,3,3,zero,16);
    if(n!=64||memcmp(raw_p+FRAME_HEADER_SIZE,fixed_stride3,16))return 3;

    for(l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l) {
        uint16_t len=lengths[l];
        for(i=0;i<len;++i){a[i]=(uint8_t)next_random();b[i]=(uint8_t)next_random();
            c[i]=(uint8_t)next_random();parity[i]=(uint8_t)(a[i]^b[i]^c[i]);}
        raw_a_len=make_frame(raw_a,FK_DATA,FF_WHITENED,session,7,global,0,66,1,0,a,len);
        raw_b_len=make_frame(raw_b,FK_DATA,FF_WHITENED,session,7,global+3,3,66,2,0,b,len);
        raw_c_len=make_frame(raw_c,FK_DATA,FF_WHITENED,session,7,global+6,6,66,3,0,c,len);
        n=make_frame(raw_p,FK_BLOCK_XOR,FF_GROUP_XOR_WHITENED,session,7,global,
            0,66,3,3,parity,len);
        if(raw_a_len!=n||raw_b_len!=n||raw_c_len!=n)return 4;
        for(i=0;i<len;++i)
            if(raw_p[FRAME_HEADER_SIZE+i]!=(uint8_t)(raw_a[FRAME_HEADER_SIZE+i]^
               raw_b[FRAME_HEADER_SIZE+i]^raw_c[FRAME_HEADER_SIZE+i]))return 5;
        for(i=0;i<FRAME_HEADER_SIZE;++i)
            header_xor[i]=(uint8_t)(raw_a[i]^raw_b[i]^raw_c[i]^raw_p[i]);
        for(mask=0;mask<8;++mask) {
            if(!qrcodegen_dosferEncodeFrameV40L(raw_a,raw_a_len,cw_a,workspace,(enum qrcodegen_Mask)mask,true)||
               !qrcodegen_dosferEncodeFrameV40L(raw_b,raw_b_len,cw_b,workspace,(enum qrcodegen_Mask)mask,true)||
               !qrcodegen_dosferEncodeFrameV40L(raw_c,raw_c_len,cw_c,workspace,(enum qrcodegen_Mask)mask,true)||
               !qrcodegen_dosferEncodeFrameV40L(raw_p,n,cw_p,workspace,(enum qrcodegen_Mask)mask,true)||
               !qrcodegen_dosferDeriveXor3V40L(cw_a,cw_b,cw_c,header_xor,derived))return 6;
            if(memcmp(cw_p,derived,DOSFER_QR_CODEWORDS))return 7;
        }

        /* A final partial group containing one physical image derives a
         * count-1 parity QR by patching only the protocol header/first RS block. */
        n=make_frame(raw_p,FK_BLOCK_XOR,FF_GROUP_XOR_WHITENED,session,7,global,
            0,66,1,3,a,len);
        for(i=0;i<FRAME_HEADER_SIZE;++i)header_xor[i]=(uint8_t)(raw_a[i]^raw_p[i]);
        if(!qrcodegen_dosferEncodeFrameV40L(raw_p,n,cw_p,workspace,qrcodegen_Mask_3,true)||
           !qrcodegen_dosferCorrectXorV40L(cw_a,1,header_xor,derived)||
           memcmp(cw_p,derived,DOSFER_QR_CODEWORDS))return 8;
    }

    /* Even-count affine derivation is valid without full padding repair only
     * for a completely full 2952-byte QR input frame. */
    for(i=0;i<MAX_FRAME_PAYLOAD;++i){a[i]=(uint8_t)next_random();b[i]=(uint8_t)next_random();
        parity[i]=(uint8_t)(a[i]^b[i]);}
    raw_a_len=make_frame(raw_a,FK_DATA,FF_WHITENED,session,9,global,0,66,1,0,a,MAX_FRAME_PAYLOAD);
    raw_b_len=make_frame(raw_b,FK_DATA,FF_WHITENED,session,9,global+3,3,66,2,0,b,MAX_FRAME_PAYLOAD);
    n=make_frame(raw_p,FK_BLOCK_XOR,FF_GROUP_XOR_WHITENED,session,9,global,
        0,66,2,3,parity,MAX_FRAME_PAYLOAD);
    if(raw_a_len!=2952||raw_b_len!=2952||n!=2952)return 9;
    if(!qrcodegen_dosferEncodeFrameV40L(raw_a,raw_a_len,cw_a,workspace,qrcodegen_Mask_5,true)||
       !qrcodegen_dosferEncodeFrameV40L(raw_b,raw_b_len,cw_b,workspace,qrcodegen_Mask_5,true)||
       !qrcodegen_dosferEncodeFrameV40L(raw_p,n,cw_p,workspace,qrcodegen_Mask_5,true))return 10;
    xor_codewords(derived,cw_a,cw_b);
    for(i=0;i<FRAME_HEADER_SIZE;++i)header_xor[i]=(uint8_t)(raw_a[i]^raw_b[i]^raw_p[i]);
    if(!qrcodegen_dosferCorrectXorV40L(derived,2,header_xor,derived)||
       memcmp(cw_p,derived,DOSFER_QR_CODEWORDS))return 11;

    free(a);free(b);free(c);free(parity);
    free(raw_a);free(raw_b);free(raw_c);free(raw_p);
    free(cw_a);free(cw_b);free(cw_c);free(cw_p);free(derived);free(workspace);
    puts("PASS: strided RGB3 whitening, wire XOR and V40-L affine derivation");
    return 0;
}
