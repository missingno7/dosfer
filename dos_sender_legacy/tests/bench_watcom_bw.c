/* Fixed-3000-cycle DOSBox microbenchmark for equivalent V40-L BW paths. */
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include "dosfer.h"
#include "qrcodegen.h"
#include "timing.h"
#include "vga.h"

#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(40)
#define DATA_CODEWORDS 2956
#define CODEWORDS 3706
#define LOOPS 36

static u8 frame[2952];
static u8 payload[2904];

static void init_payload(void) {
    unsigned long state=0xD05FE123UL;
    unsigned int i;
    for(i=0;i<sizeof(payload);++i) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        payload[i]=(u8)state;
    }
}

static void fill_frame(u8 *dest,unsigned long sequence) {
    make_frame(dest,FK_DATA,FF_WHITENED,0x6A67C69DUL,0,sequence,
        (u16)(sequence%66UL),66,1,sequence*(u32)sizeof(payload),
        payload,sizeof(payload));
}

int main(void) {
    u8 far *current=0,*workspace=0,*reference=0;
    u32 delta_build=0,delta_ecc=0,delta_raster=0,delta_present=0;
    u32 direct_build=0,direct_encode=0,direct_copy=0,direct_scatter=0;
    u32 direct_present=0,asm_copy=0,asm_scatter=0,asm_present=0;
    u32 delta_hash,direct_hash,asm_hash,a,b;
    unsigned int step;
    int rc=0;

    init_payload();
    current=(u8 far *)_fmalloc(CODEWORDS);
    reference=(u8 far *)_fmalloc(CODEWORDS);
    workspace=(u8 far *)_fmalloc(QR_BUFFER);
    if(!current||!reference||!workspace) {puts("FAIL: allocation");rc=2;goto done;}
    fill_frame(frame,0);
    if(!qrcodegen_dosferEncodeFrameV40L(frame,sizeof(frame),current,workspace,
            qrcodegen_Mask_0,false)) {puts("FAIL: bootstrap encode");rc=3;goto done;}
    if(!vga_enter(VIDEO_320_60,0)) {puts("FAIL: VGA mode");rc=4;goto done;}
    if(!vga_show_full_qr_at(workspace,current,0,0,0)) {
        puts("FAIL: bootstrap render");rc=5;goto leave_vga;
    }
    if(!vga_direct_ready()) {puts("FAIL: BW direct template");rc=6;goto leave_vga;}

    for(step=0;step<LOOPS;++step) {
        a=timer_ticks();
        workspace[0]=0x70;workspace[1]=0x34;
        workspace[2]=0x0B;workspace[3]=0x88;
        fill_frame(workspace+4,step+1UL);
        b=timer_ticks();delta_build+=b-a;
        a=timer_ticks();
        qrcodegen_dosferComputeEccBlocksV40L(workspace,workspace+DATA_CODEWORDS);
        b=timer_ticks();delta_ecc+=b-a;
        a=timer_ticks();
        if(!vga_apply_v40l_delta(workspace,workspace+DATA_CODEWORDS,current)) {rc=8;goto leave_vga;}
        b=timer_ticks();delta_raster+=b-a;
        a=timer_ticks();if(!vga_show_prepared_at(0,0,0)) {rc=9;goto leave_vga;}
        b=timer_ticks();delta_present+=b-a;
        delta_hash=vga_screen_hash();

        a=timer_ticks();
        workspace[0]=0x70;workspace[1]=0x34;
        workspace[2]=0x0B;workspace[3]=0x88;
        fill_frame(workspace+4,step+1UL);
        b=timer_ticks();direct_build+=b-a;
        a=timer_ticks();
        if(!qrcodegen_dosferEncodePrepackedV40L(workspace,reference)) {
            rc=10;goto leave_vga;
        }
        b=timer_ticks();direct_encode+=b-a;
        a=timer_ticks();if(!vga_direct_reset_template()) {rc=11;goto leave_vga;}
        b=timer_ticks();direct_copy+=b-a;
        a=timer_ticks();if(!vga_direct_scatter(reference)) {rc=12;goto leave_vga;}
        b=timer_ticks();direct_scatter+=b-a;
        direct_hash=vga_screen_hash();
        if(delta_hash!=direct_hash) {
            printf("FAIL: BW direct frame %u delta %08lX direct %08lX\n",
                step+1,delta_hash,direct_hash);rc=13;goto leave_vga;
        }
        a=timer_ticks();if(!vga_show_prepared_at(0,0,0)) {rc=14;goto leave_vga;}
        b=timer_ticks();direct_present+=b-a;

        a=timer_ticks();if(!vga_direct_reset_template()) {rc=15;goto leave_vga;}
        b=timer_ticks();asm_copy+=b-a;
        a=timer_ticks();if(!vga_direct_scatter_asm(reference)) {rc=16;goto leave_vga;}
        b=timer_ticks();asm_scatter+=b-a;
        asm_hash=vga_screen_hash();
        if(asm_hash!=direct_hash) {
            printf("FAIL: BW ASM frame %u direct %08lX asm %08lX\n",
                step+1,direct_hash,asm_hash);rc=17;goto leave_vga;
        }
        a=timer_ticks();if(!vga_show_prepared_at(0,0,0)) {rc=18;goto leave_vga;}
        b=timer_ticks();asm_present+=b-a;
    }
    printf("BW production delta x%u: frame+prefix %lu ms, ECC %lu ms, raster %lu ms, present %lu ms, total %lu ms (%lu/frame)\n",
        LOOPS,timer_elapsed_ms(0,delta_build),
        timer_elapsed_ms(0,delta_ecc),timer_elapsed_ms(0,delta_raster),
        timer_elapsed_ms(0,delta_present),
        timer_elapsed_ms(0,delta_build+delta_ecc+delta_raster+delta_present),
        timer_elapsed_ms(0,delta_build+delta_ecc+delta_raster+delta_present)/LOOPS);
    printf("BW direct C x%u: frame %lu ms, encode %lu ms, template %lu ms, scatter %lu ms, present %lu ms, total %lu ms (%lu/frame)\n",
        LOOPS,timer_elapsed_ms(0,direct_build),timer_elapsed_ms(0,direct_encode),
        timer_elapsed_ms(0,direct_copy),timer_elapsed_ms(0,direct_scatter),
        timer_elapsed_ms(0,direct_present),
        timer_elapsed_ms(0,direct_build+direct_encode+direct_copy+direct_scatter+direct_present),
        timer_elapsed_ms(0,direct_build+direct_encode+direct_copy+direct_scatter+direct_present)/LOOPS);
    printf("BW production direct ASM x%u: encode %lu ms, template %lu ms, scatter %lu ms, present %lu ms, total %lu ms (%lu/frame)\n",
        LOOPS,timer_elapsed_ms(0,direct_encode),timer_elapsed_ms(0,asm_copy),
        timer_elapsed_ms(0,asm_scatter),timer_elapsed_ms(0,asm_present),
        timer_elapsed_ms(0,direct_build+direct_encode+asm_copy+asm_scatter+asm_present),
        timer_elapsed_ms(0,direct_build+direct_encode+asm_copy+asm_scatter+asm_present)/LOOPS);

leave_vga:
    vga_leave();
done:
    if(current)_ffree(current);
    if(reference)_ffree(reference);
    if(workspace)_ffree(workspace);
    return rc;
}
