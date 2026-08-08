/* Fixed-3000-cycle DOSBox microbenchmark for the actual RGB3 steady path.
 * Timings are per displayed RGB triplet (three independent V40-L frames). */
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
#define LOOPS 12

static u8 frame[2952];
static u8 payload[2904];

static void init_payload(void) {
    unsigned long state=0xD05FE123UL;
    unsigned int i;
    for(i=0;i<sizeof(payload);i++) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        payload[i]=(u8)state;
    }
}

static void fill_frame(unsigned long sequence) {
    make_frame(frame,FK_DATA,FF_WHITENED,0x6A67C69DUL,0,sequence,
        (u16)(sequence%66UL),66,1,sequence*(u32)sizeof(payload),
        payload,sizeof(payload));
}

int main(void) {
    u8 far *current[3]={0,0,0},*workspace[3]={0,0,0};
    const u8 *data[3],*ecc[3],*qr[3];
    u8 *current_arg[3];
    u32 delta_build=0,delta_pack=0,delta_ecc=0,delta_raster=0,delta_present=0,delta_total;
    u32 direct_build=0,direct_encode=0,direct_copy=0,direct_scatter=0;
    u32 direct_present=0,direct_total,asm_copy=0,asm_scatter=0,asm_present=0,asm_total;
    u32 protocol_copy=0,protocol_crc=0,protocol_whiten=0,protocol_header=0;
    u32 delta_hash,direct_hash,asm_hash,a,b;
#ifdef DOSFER_MAP_STATS
    u32 map_loads,red_xors,green_xors,blue_xors,map_entries,within_bytes,global_bytes;
#endif
    unsigned int step,channel;
    int rc=0;

    init_payload();
    for(channel=0;channel<3;channel++) {
        current[channel]=(u8 far *)_fmalloc(CODEWORDS);
        workspace[channel]=(u8 far *)_fmalloc(QR_BUFFER);
        if(!current[channel]||!workspace[channel]) {puts("FAIL: allocation");rc=2;goto done;}
        fill_frame(channel);
        if(!qrcodegen_dosferEncodeFrameV40L(frame,sizeof(frame),current[channel],
                workspace[channel],qrcodegen_Mask_0,false)) {puts("FAIL: bootstrap encode");rc=3;goto done;}
        qr[channel]=workspace[channel];current_arg[channel]=current[channel];
    }
    for(step=0;step<LOOPS;step++)for(channel=0;channel<3;channel++) {
        unsigned long sequence=(unsigned long)(step*3+channel+3);
        a=timer_ticks();
        memcpy(frame+FRAME_HEADER_SIZE,payload,sizeof(payload));
        b=timer_ticks();protocol_copy+=b-a;
        a=timer_ticks();
        make_frame(frame,FK_DATA,0,0x6A67C69DUL,0,sequence,
            (u16)(sequence%66UL),66,1,sequence*(u32)sizeof(payload),
            payload,sizeof(payload));
        b=timer_ticks();protocol_crc+=b-a;
        a=timer_ticks();
        make_frame(frame,FK_DATA,FF_WHITENED,0x6A67C69DUL,0,sequence,
            (u16)(sequence%66UL),66,1,sequence*(u32)sizeof(payload),
            payload,sizeof(payload));
        b=timer_ticks();protocol_whiten+=b-a;
        a=timer_ticks();
        make_frame(frame,FK_DATA,0,0x6A67C69DUL,0,sequence,
            (u16)(sequence%66UL),66,1,sequence*(u32)sizeof(payload),0,0);
        b=timer_ticks();protocol_header+=b-a;
    }
    if(!vga_enter(VIDEO_320_60,1)) {puts("FAIL: VGA mode");rc=4;goto done;}
    if(!vga_show_full_qr3_at(qr,current_arg,0,0,0)) {puts("FAIL: bootstrap render");rc=5;goto leave_vga;}
#ifdef DOSFER_MAP_STATS
    vga_rgb3_map_layout_stats(&map_entries,&within_bytes,&global_bytes);
    vga_rgb3_map_stats_reset();
#endif
    if(!vga_rgb3_direct_ready()) {puts("FAIL: production RGB direct template setup");rc=9;goto leave_vga;}

    for(step=0;step<LOOPS;step++) {
        for(channel=0;channel<3;channel++) {
            a=timer_ticks();
            fill_frame((unsigned long)(step*3+channel+3));
            b=timer_ticks();delta_build+=b-a;
            a=timer_ticks();
            if(!qrcodegen_dosferPackFrameV40L(frame,sizeof(frame),workspace[channel])) {rc=6;goto leave_vga;}
            b=timer_ticks();delta_pack+=b-a;
            a=timer_ticks();
            qrcodegen_dosferComputeEccBlocksV40L(workspace[channel],workspace[channel]+DATA_CODEWORDS);
            b=timer_ticks();delta_ecc+=b-a;
            data[channel]=workspace[channel];ecc[channel]=workspace[channel]+DATA_CODEWORDS;
            current_arg[channel]=current[channel];
        }
        a=timer_ticks();
        if(!vga_apply_v40l_delta3(data,ecc,current_arg)) {rc=7;goto leave_vga;}
        b=timer_ticks();delta_raster+=b-a;
        a=timer_ticks();
        if(!vga_show_prepared3_at(0,0,0)) {rc=8;goto leave_vga;}
        b=timer_ticks();delta_present+=b-a;
        delta_hash=vga_screen_hash();

        for(channel=0;channel<3;channel++) {
            a=timer_ticks();
            fill_frame((unsigned long)(step*3+channel+3));
            b=timer_ticks();direct_build+=b-a;
            a=timer_ticks();
            if(!qrcodegen_dosferEncodeFrameV40L(frame,sizeof(frame),current[channel],
                    workspace[channel],qrcodegen_Mask_0,true)) {rc=10;goto leave_vga;}
            b=timer_ticks();direct_encode+=b-a;
            current_arg[channel]=current[channel];
        }
        a=timer_ticks();
        if(!vga_rgb3_direct_reset_template()) {rc=11;goto leave_vga;}
        b=timer_ticks();direct_copy+=b-a;
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter((const u8 *const *)current_arg)) {rc=11;goto leave_vga;}
        b=timer_ticks();direct_scatter+=b-a;
        direct_hash=vga_screen_hash();
        if(delta_hash!=direct_hash) {
            printf("FAIL: RGB direct triplet %u delta %08lX direct %08lX\n",
                step+1,delta_hash,direct_hash);rc=12;goto leave_vga;
        }
        a=timer_ticks();
        if(!vga_show_prepared3_at(0,0,0)) {rc=13;goto leave_vga;}
        b=timer_ticks();direct_present+=b-a;
        a=timer_ticks();
        if(!vga_rgb3_direct_reset_template()) {rc=14;goto leave_vga;}
        b=timer_ticks();asm_copy+=b-a;
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_asm((const u8 *const *)current_arg)) {rc=15;goto leave_vga;}
        b=timer_ticks();asm_scatter+=b-a;
        asm_hash=vga_screen_hash();
        if(asm_hash!=direct_hash) {
            printf("FAIL: RGB ASM triplet %u direct %08lX asm %08lX\n",
                step+1,direct_hash,asm_hash);rc=16;goto leave_vga;
        }
        a=timer_ticks();
        if(!vga_show_prepared3_at(0,0,0)) {rc=18;goto leave_vga;}
        b=timer_ticks();asm_present+=b-a;
    }
    delta_total=delta_build+delta_pack+delta_ecc+delta_raster+delta_present;
    direct_total=direct_build+direct_encode+direct_copy+direct_scatter+direct_present;
    asm_total=direct_build+direct_encode+asm_copy+asm_scatter+asm_present;
    printf("RGB3 production delta x%u: frame %lu ms, pack %lu ms, 3ECC %lu ms, raster %lu ms, present %lu ms, total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,delta_build),timer_elapsed_ms(0,delta_pack),timer_elapsed_ms(0,delta_ecc),
        timer_elapsed_ms(0,delta_raster),timer_elapsed_ms(0,delta_present),
        timer_elapsed_ms(0,delta_total),timer_elapsed_ms(0,delta_total)/(u32)LOOPS);
    printf("RGB3 direct C baseline x%u: frame %lu ms, pack+3ECC+interleave %lu ms, raster %lu ms, present %lu ms, total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,direct_build),timer_elapsed_ms(0,direct_encode),
        timer_elapsed_ms(0,direct_copy+direct_scatter),timer_elapsed_ms(0,direct_present),
        timer_elapsed_ms(0,direct_total),timer_elapsed_ms(0,direct_total)/(u32)LOOPS);
    printf("RGB3 production direct ASM x%u: raster %lu ms, present %lu ms, total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,asm_copy+asm_scatter),timer_elapsed_ms(0,asm_present),timer_elapsed_ms(0,asm_total),
        timer_elapsed_ms(0,asm_total)/(u32)LOOPS);
    printf("RGB3 direct QR x%u: encode %lu ms = pack %lu + ECC %lu + interleave/overhead %lu ms\n",
        LOOPS,timer_elapsed_ms(0,direct_encode),timer_elapsed_ms(0,delta_pack),
        timer_elapsed_ms(0,delta_ecc),timer_elapsed_ms(0,direct_encode-delta_pack-delta_ecc));
    printf("RGB3 protocol x%u triplets: raw copy %lu ms, copy+CRC %lu ms, whiten+CRC %lu ms, header-only %lu ms\n",
        LOOPS,timer_elapsed_ms(0,protocol_copy),timer_elapsed_ms(0,protocol_crc),
        timer_elapsed_ms(0,protocol_whiten),timer_elapsed_ms(0,protocol_header));
    printf("RGB3 exact scatter x%u: template copy %lu ms; C %lu ms (%lu/triplet); ASM %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,direct_copy),timer_elapsed_ms(0,direct_scatter),
        timer_elapsed_ms(0,direct_scatter)/(u32)LOOPS,
        timer_elapsed_ms(0,asm_scatter),timer_elapsed_ms(0,asm_scatter)/(u32)LOOPS);
#ifdef DOSFER_MAP_STATS
    vga_rgb3_map_stats(&map_loads,&red_xors,&green_xors,&blue_xors);
    printf("RGB3 map x%u: %lu loads (%lu/triplet), XOR R/G/B %lu/%lu/%lu (%lu total, %lu/triplet)\n",
        LOOPS,map_loads,map_loads/(u32)LOOPS,red_xors,green_xors,blue_xors,
        red_xors+green_xors+blue_xors,
        (red_xors+green_xors+blue_xors)/(u32)LOOPS);
    printf("RGB3 map layout: %lu entries, %lu within-codeword byte targets (%lu local merges), %lu global byte targets\n",
        map_entries,within_bytes,map_entries-within_bytes,global_bytes);
#endif

leave_vga:
    vga_leave();
done:
    for(channel=0;channel<3;channel++) {
        if(current[channel])_ffree(current[channel]);
        if(workspace[channel])_ffree(workspace[channel]);
    }
    return rc;
}
