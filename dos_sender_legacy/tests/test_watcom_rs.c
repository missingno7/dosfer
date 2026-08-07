/* DOS/Open Watcom oracle for the fixed V40-L Reed-Solomon path.
 *
 * This test deliberately contains an independent, scalar GF(256) reference
 * and compares every emitted codeword. It must be compiled with -bt=dos -ml
 * so the near-DGROUP RS state and far input/output buffers match the sender. */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "qrcodegen.h"

#define DATA_CODEWORDS 2956
#define ECC_CODEWORDS 750
#define TOTAL_CODEWORDS (DATA_CODEWORDS + ECC_CODEWORDS)
#define BLOCKS 25
#define ECC_LEN 30
#define QR_BUFFER_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(40)

static unsigned char data[DATA_CODEWORDS];
static unsigned char actual[TOTAL_CODEWORDS];
static unsigned char expected[TOTAL_CODEWORDS];
static unsigned char divisor[ECC_LEN];
static unsigned char remainder[ECC_LEN];
static unsigned char frame[2952];
static unsigned char reference_codewords[TOTAL_CODEWORDS];
static unsigned char reference_qr[QR_BUFFER_LEN];

static unsigned char gf_mul(unsigned char x,unsigned char y) {
    unsigned char z=0;
    while(y) {
        if(y&1)z^=x;
        y>>=1;
        x=(unsigned char)((x<<1)^((x&0x80)?0x1D:0));
    }
    return z;
}

static void make_divisor(void) {
    int i,j;
    unsigned char root=1;
    memset(divisor,0,sizeof(divisor));
    divisor[ECC_LEN-1]=1;
    for(i=0;i<ECC_LEN;i++) {
        for(j=0;j<ECC_LEN;j++) {
            divisor[j]=gf_mul(divisor[j],root);
            if(j+1<ECC_LEN)divisor[j]^=divisor[j+1];
        }
        root=gf_mul(root,2);
    }
}

static void make_remainder(const unsigned char *src,int len) {
    int i,j;
    memset(remainder,0,sizeof(remainder));
    for(i=0;i<len;i++) {
        unsigned char factor=(unsigned char)(src[i]^remainder[0]);
        memmove(remainder,remainder+1,ECC_LEN-1);
        remainder[ECC_LEN-1]=0;
        for(j=0;j<ECC_LEN;j++)remainder[j]^=gf_mul(divisor[j],factor);
    }
}

static void make_expected(void) {
    const unsigned char *src=data;
    int block,column;
    for(block=0;block<BLOCKS;block++) {
        int len=block<19?118:119;
        make_remainder(src,len);
        for(column=0;column<118;column++)
            expected[block+column*BLOCKS]=src[column];
        if(block>=19)expected[2950+block-19]=src[118];
        for(column=0;column<ECC_LEN;column++)
            expected[DATA_CODEWORDS+block+column*BLOCKS]=remainder[column];
        src+=len;
    }
}

static void fill_data(unsigned int lane) {
    unsigned long state=0x6A67C69DUL^(unsigned long)lane*0x13579BDFUL;
    unsigned int i;
    for(i=0;i<DATA_CODEWORDS;i++) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        data[i]=(unsigned char)state;
    }
    /* Exercise the exact ECI/byte/pad shape used by sender frames too. */
    if(lane==0) {
        data[0]=0x70;data[1]=0x34;data[2]=0x0B;data[3]=0x88;
        for(i=4;i<DATA_CODEWORDS;i++)data[i]=(i&1)?0xEC:0x11;
    }
}

static void fill_frame(unsigned int lane,unsigned int len) {
    unsigned long state=0x13579BDFUL^(unsigned long)lane*0x2468ACE1UL;
    unsigned int i;
    for(i=0;i<len;i++) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        frame[i]=(unsigned char)state;
    }
}

static int verify_far_matrix_lanes(void) {
    static const unsigned int lengths[3]={64,208,2952};
    unsigned char __far *far_codewords;
    unsigned char __far *far_qr;
    unsigned int lane,i;

    far_codewords=(unsigned char __far *)_fmalloc(TOTAL_CODEWORDS);
    far_qr=(unsigned char __far *)_fmalloc(QR_BUFFER_LEN);
    if(!far_codewords||!far_qr) {
        puts("FAIL: could not allocate far matrix test buffers");
        if(far_codewords)_ffree(far_codewords);
        if(far_qr)_ffree(far_qr);
        return 4;
    }
    for(lane=0;lane<3;lane++) {
        fill_frame(lane,lengths[lane]);
        if(!qrcodegen_dosferPackFrameV40L(frame,lengths[lane],data)) {
            puts("FAIL: V40-L frame packer rejected valid frame");
            _ffree(far_codewords);_ffree(far_qr);
            return 5;
        }
        memset(expected,0,sizeof(expected));
        make_expected();
        if(!qrcodegen_dosferEncodeFrameV40L(frame,lengths[lane],
                reference_codewords,reference_qr,qrcodegen_Mask_0,false) ||
           !qrcodegen_dosferEncodeFrameV40L(frame,lengths[lane],
                far_codewords,far_qr,qrcodegen_Mask_0,false)) {
            puts("FAIL: V40-L matrix encoder rejected valid frame");
            _ffree(far_codewords);_ffree(far_qr);
            return 6;
        }
        for(i=0;i<TOTAL_CODEWORDS;i++)if(far_codewords[i]!=expected[i]) {
            printf("FAIL: matrix lane %u scalar codeword %u got %02X expected %02X\n",
                lane+1,i,far_codewords[i],expected[i]);
            _ffree(far_codewords);_ffree(far_qr);
            return 7;
        }
        for(i=0;i<TOTAL_CODEWORDS;i++)if(far_codewords[i]!=reference_codewords[i]) {
            printf("FAIL: matrix lane %u codeword %u got %02X expected %02X\n",
                lane+1,i,far_codewords[i],reference_codewords[i]);
            _ffree(far_codewords);_ffree(far_qr);
            return 8;
        }
        for(i=0;i<QR_BUFFER_LEN;i++)if(far_qr[i]!=reference_qr[i]) {
            printf("FAIL: matrix lane %u byte %u got %02X expected %02X\n",
                lane+1,i,far_qr[i],reference_qr[i]);
            _ffree(far_codewords);_ffree(far_qr);
            return 9;
        }
    }
    _ffree(far_codewords);_ffree(far_qr);
    return 0;
}

/* This is the sender's steady RGB path: pack into a far workspace, then
 * compute the 25 ECC blocks in that same far allocation.  Repeat beyond one
 * RGB window so a lane-specific DS leak cannot hide in a one-shot test. */
static int verify_far_delta_lanes(void) {
    unsigned char __far *workspace[3]={0,0,0};
    unsigned int triplet,lane,block,column;

    for(lane=0;lane<3;lane++)
        workspace[lane]=(unsigned char __far *)_fmalloc(TOTAL_CODEWORDS);
    if(!workspace[0]||!workspace[1]||!workspace[2]) {
        puts("FAIL: could not allocate far delta test buffers");
        for(lane=0;lane<3;lane++)if(workspace[lane])_ffree(workspace[lane]);
        return 10;
    }
    for(triplet=0;triplet<13;triplet++)for(lane=0;lane<3;lane++) {
        unsigned int frame_len=(unsigned int)(64+((triplet*73+lane*47)%2800));
        fill_frame(triplet*3+lane,frame_len);
        if(!qrcodegen_dosferPackFrameV40L(frame,frame_len,data) ||
           !qrcodegen_dosferPackFrameV40L(frame,frame_len,workspace[lane])) {
            puts("FAIL: V40-L delta packer rejected valid frame");
            for(lane=0;lane<3;lane++)_ffree(workspace[lane]);
            return 11;
        }
        memset(expected,0,sizeof(expected));
        make_expected();
        qrcodegen_dosferComputeEccBlocksV40L(workspace[lane],
            workspace[lane]+DATA_CODEWORDS);
        for(block=0;block<BLOCKS;block++)for(column=0;column<ECC_LEN;column++)
            if(workspace[lane][DATA_CODEWORDS+block*ECC_LEN+column]!=
                    expected[DATA_CODEWORDS+block+column*BLOCKS]) {
                printf("FAIL: delta triplet %u lane %u ECC block %u byte %u got %02X expected %02X\n",
                    triplet+1,lane+1,block+1,column+1,
                    workspace[lane][DATA_CODEWORDS+block*ECC_LEN+column],
                    expected[DATA_CODEWORDS+block+column*BLOCKS]);
                for(lane=0;lane<3;lane++)_ffree(workspace[lane]);
                return 12;
            }
    }
    for(lane=0;lane<3;lane++)_ffree(workspace[lane]);
    return 0;
}

/* RGB3 parity first XORs three far codeword streams, then repairs the QR
 * prefix and first ECC block in place. Exercise both public entry points with
 * far input/output storage because this happens at every third RGB batch. */
static int verify_far_xor3(void) {
    unsigned char __far *input[3]={0,0,0};
    unsigned char __far *result=0;
    unsigned char header[48];
    unsigned long state=0x41C64E6DUL;
    unsigned int round,lane,i;

    for(lane=0;lane<3;lane++)
        input[lane]=(unsigned char __far *)_fmalloc(TOTAL_CODEWORDS);
    result=(unsigned char __far *)_fmalloc(TOTAL_CODEWORDS);
    if(!input[0]||!input[1]||!input[2]||!result) {
        puts("FAIL: could not allocate far XOR3 test buffers");
        for(lane=0;lane<3;lane++)if(input[lane])_ffree(input[lane]);
        if(result)_ffree(result);
        return 13;
    }
    for(round=0;round<13;round++) {
        for(lane=0;lane<3;lane++)for(i=0;i<TOTAL_CODEWORDS;i++) {
            state^=state<<13;state^=state>>17;state^=state<<5;
            input[lane][i]=(unsigned char)state;
        }
        for(i=0;i<48;i++) {
            state^=state<<13;state^=state>>17;state^=state<<5;
            header[i]=(unsigned char)state;
        }
        for(i=0;i<TOTAL_CODEWORDS;i++)
            expected[i]=input[0][i]^input[1][i]^input[2][i];
        memset(data,0,118);memcpy(data+4,header,48);
        make_remainder(data,118);
        for(i=0;i<52;i++)expected[i*25]^=data[i];
        for(i=0;i<ECC_LEN;i++)expected[DATA_CODEWORDS+i*25]^=remainder[i];

        if(!qrcodegen_dosferDeriveXor3V40L(input[0],input[1],input[2],
                header,result)) {
            puts("FAIL: far XOR3 derivation rejected valid buffers");
            for(lane=0;lane<3;lane++)_ffree(input[lane]);_ffree(result);
            return 14;
        }
        for(i=0;i<TOTAL_CODEWORDS;i++)if(result[i]!=expected[i]) {
            printf("FAIL: XOR3 round %u codeword %u got %02X expected %02X\n",
                round+1,i,result[i],expected[i]);
            for(lane=0;lane<3;lane++)_ffree(input[lane]);_ffree(result);
            return 15;
        }

        for(i=0;i<TOTAL_CODEWORDS;i++)
            result[i]=input[0][i]^input[1][i]^input[2][i];
        if(!qrcodegen_dosferCorrectXorV40L(result,3,header,result)) {
            puts("FAIL: far XOR3 correction rejected valid buffers");
            for(lane=0;lane<3;lane++)_ffree(input[lane]);_ffree(result);
            return 16;
        }
        for(i=0;i<TOTAL_CODEWORDS;i++)if(result[i]!=expected[i]) {
            printf("FAIL: XOR3 correction round %u codeword %u got %02X expected %02X\n",
                round+1,i,result[i],expected[i]);
            for(lane=0;lane<3;lane++)_ffree(input[lane]);_ffree(result);
            return 17;
        }
    }
    for(lane=0;lane<3;lane++)_ffree(input[lane]);_ffree(result);
    return 0;
}

int main(void) {
    unsigned int lane,i;
    unsigned char __far *far_data;
    unsigned char __far *far_actual;

    far_data=(unsigned char __far *)_fmalloc(DATA_CODEWORDS);
    far_actual=(unsigned char __far *)_fmalloc(TOTAL_CODEWORDS);
    if(!far_data||!far_actual) {
        puts("FAIL: could not allocate far RS test buffers");
        if(far_data)_ffree(far_data);
        if(far_actual)_ffree(far_actual);
        return 1;
    }
    make_divisor();
    for(lane=0;lane<3;lane++) {
        fill_data(lane);
        _fmemcpy(far_data,data,DATA_CODEWORDS);
        memset(actual,0,sizeof(actual));
        memset(expected,0,sizeof(expected));
        make_expected();
        if(!qrcodegen_dosferEncodePrepackedV40L(far_data,far_actual)) {
            puts("FAIL: V40-L encoder rejected valid data");
            _ffree(far_data);_ffree(far_actual);
            return 2;
        }
        for(i=0;i<TOTAL_CODEWORDS;i++)if(far_actual[i]!=expected[i]) {
            printf("FAIL: lane %u codeword %u got %02X expected %02X\n",
                lane+1,i,far_actual[i],expected[i]);
            _ffree(far_data);_ffree(far_actual);
            return 3;
        }
    }
    _ffree(far_data);_ffree(far_actual);
    if(verify_far_xor3())return 4;
    if(verify_far_delta_lanes())return 4;
    if(verify_far_matrix_lanes())return 4;
    puts("PASS: Open Watcom V40-L R/G/B far codewords, ECC and matrices match near reference");
    return 0;
}
