/* Fixed-3000-cycle DOSBox microbenchmark for the actual RGB3 steady path.
 * Timings are per displayed RGB triplet (three independent V40-L frames). */
#include <stdio.h>
#include <dos.h>
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

static u8 far *align_far16(u8 far *p) {
    u32 paragraphs=((u32)FP_OFF(p)+15UL)>>4;
    return (u8 far *)MK_FP((u16)(FP_SEG(p)+paragraphs),0);
}

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
    u8 far *current_raw[3]={0,0,0};
    u8 far *fused_workspace[3]={0,0,0};
    u8 far *fused_raw[3]={0,0,0};
    u8 far *fused_codewords[3]={0,0,0};
    u8 far *parity_codewords=0;
    u8 parity_header_xor[FRAME_HEADER_SIZE];
    const u8 *data[3],*ecc[3],*qr[3];
    const u8 *fused_data[3];
    u8 *fused_ecc_out[3];
    u8 *current_arg[3];
    u32 delta_build=0,delta_pack=0,delta_ecc=0,delta_raster=0,delta_present=0,delta_total;
    u32 direct_build=0,direct_encode=0,direct_copy=0,direct_scatter=0;
    u32 direct_present=0,direct_total,asm_copy=0,asm_scatter=0,asm_present=0,asm_total;
    u32 stripe_copy=0,stripe_scatter=0,stripe_present=0,stripe_total;
    u32 production_raster=0;
    u32 isolated_stripe=0,isolated_fallback=0,isolated_setup=0;
    u32 group_baseline=0,group_scatter=0,grouped_full=0;
    u32 phase_baseline=0,phase_scatter=0,phased_full=0;
    u32 phase_kernel=0,phase_edges=0;
    u32 fused_total,production_grouped_total;
    u32 protocol_copy=0,protocol_crc=0,protocol_whiten=0,protocol_header=0;
    u32 fused_ecc=0,fused_encode=0;
    u32 parity_derive=0;
    u32 delta_hash,direct_hash,asm_hash,stripe_hash,grouped_hash,phased_hash,a,b;
    u16 stripe_runs=0,stripe_covered=0;
    u16 geometry_runs[4],geometry_codewords[4],other_runs,other_codewords;
    u16 stripe_groups,group_source_codewords,group_destination_bytes;
    u16 phase_groups,phase_source_codewords,phase_destination_bytes,
        phase_edge_contributions;
#ifdef DOSFER_MAP_STATS
    u32 map_loads,red_xors,green_xors,blue_xors,map_entries,within_bytes,global_bytes;
#endif
    unsigned int step,channel;
    int rc=0;

    init_payload();
    memset(parity_header_xor,0,sizeof(parity_header_xor));
    parity_codewords=(u8 far *)_fmalloc(CODEWORDS);
    if(!parity_codewords) {puts("FAIL: parity allocation");rc=2;goto done;}
    for(channel=0;channel<3;channel++) {
        current_raw[channel]=(u8 far *)_fmalloc(CODEWORDS+15);
        if(current_raw[channel])current[channel]=align_far16(current_raw[channel]);
        workspace[channel]=(u8 far *)_fmalloc(QR_BUFFER);
        fused_raw[channel]=(u8 far *)_fmalloc(QR_BUFFER+15);
        if(fused_raw[channel])fused_workspace[channel]=align_far16(fused_raw[channel]);
        fused_codewords[channel]=(u8 far *)_fmalloc(CODEWORDS);
        if(!current[channel]||!workspace[channel]||!fused_workspace[channel]||
                !fused_codewords[channel]) {
            puts("FAIL: allocation");rc=2;goto done;
        }
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
    if(!vga_direct_ready()) {puts("FAIL: production RGB direct template setup");rc=9;goto leave_vga;}
    vga_rgb3_stripe_stats(&stripe_runs,&stripe_covered);
    if(!stripe_runs) {puts("FAIL: no RGB stripe runs");rc=23;goto leave_vga;}
    vga_rgb3_stripe_geometry_stats(geometry_runs,geometry_codewords,
        &other_runs,&other_codewords);
    vga_rgb3_stripe_group_stats(&stripe_groups,&group_source_codewords,
        &group_destination_bytes);
    if(!stripe_groups) {puts("FAIL: no RGB stripe groups");rc=35;goto leave_vga;}
    vga_rgb3_stripe_phase_stats(&phase_groups,&phase_source_codewords,
        &phase_destination_bytes,&phase_edge_contributions);

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
        for(channel=0;channel<3;channel++) {
            _fmemcpy(fused_workspace[channel],workspace[channel],DATA_CODEWORDS);
            fused_data[channel]=fused_workspace[channel];
            fused_ecc_out[channel]=fused_workspace[channel]+DATA_CODEWORDS;
        }
        a=timer_ticks();
        qrcodegen_dosferComputeEccBlocks3V40L(fused_data,fused_ecc_out);
        b=timer_ticks();fused_ecc+=b-a;
        for(channel=0;channel<3;channel++)
            if(_fmemcmp(fused_ecc_out[channel],ecc[channel],750)) {
                printf("FAIL: fused RGB ECC triplet %u channel %u\n",
                    step+1,channel+1);rc=19;goto leave_vga;
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
        if(!qrcodegen_dosferEncodePrepacked3V40L(fused_workspace,
                fused_codewords)) {rc=20;goto leave_vga;}
        b=timer_ticks();fused_encode+=b-a;
        for(channel=0;channel<3;channel++)
            if(_fmemcmp(fused_codewords[channel],current[channel],CODEWORDS)) {
                printf("FAIL: fused RGB codewords triplet %u channel %u\n",
                    step+1,channel+1);rc=21;goto leave_vga;
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
        a=timer_ticks();
        if(!vga_rgb3_direct_reset_template()) {rc=24;goto leave_vga;}
        b=timer_ticks();stripe_copy+=b-a;
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_stripe(
                (const u8 *const *)current_arg)) {rc=25;goto leave_vga;}
        b=timer_ticks();stripe_scatter+=b-a;
        stripe_hash=vga_screen_hash();
        if(stripe_hash!=asm_hash) {
            printf("FAIL: RGB stripe triplet %u asm %08lX stripe %08lX\n",
                step+1,asm_hash,stripe_hash);rc=26;goto leave_vga;
        }
        a=timer_ticks();
        if(!vga_show_prepared3_at(0,0,0)) {rc=27;goto leave_vga;}
        b=timer_ticks();stripe_present+=b-a;
        a=timer_ticks();
        if(!vga_apply_codewords3_direct(
                (const u8 *const *)current_arg)) {rc=28;goto leave_vga;}
        b=timer_ticks();production_raster+=b-a;
        if(vga_screen_hash()!=stripe_hash) {
            printf("FAIL: production stripe triplet %u\n",step+1);
            rc=29;goto leave_vga;
        }
        if(!vga_rgb3_direct_reset_template()) {rc=36;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_grouped(
                (const u8 *const *)current_arg)) {rc=37;goto leave_vga;}
        b=timer_ticks();grouped_full+=b-a;
        grouped_hash=vga_screen_hash();
        if(grouped_hash!=stripe_hash) {
            printf("FAIL: RGB grouped stripe triplet %u stripe %08lX grouped %08lX\n",
                step+1,stripe_hash,grouped_hash);rc=38;goto leave_vga;
        }
        if(!vga_rgb3_direct_reset_template()) {rc=43;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_phased(
                (const u8 *const *)current_arg)) {rc=44;goto leave_vga;}
        b=timer_ticks();phased_full+=b-a;
        phased_hash=vga_screen_hash();
        if(phased_hash!=stripe_hash) {
            printf("FAIL: RGB phased stripe triplet %u stripe %08lX phased %08lX\n",
                step+1,stripe_hash,phased_hash);rc=45;goto leave_vga;
        }
    }
    for(step=0;step<LOOPS;++step) {
        if(!vga_rgb3_direct_reset_template()) {rc=30;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_stripe_runs(
                (const u8 *const *)current_arg)) {rc=31;goto leave_vga;}
        b=timer_ticks();isolated_stripe+=b-a;
        if(!vga_rgb3_direct_reset_template()) {rc=32;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_stripe_fallback(
                (const u8 *const *)current_arg)) {rc=33;goto leave_vga;}
        b=timer_ticks();isolated_fallback+=b-a;
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_stripe_setup(
                (const u8 *const *)current_arg)) {rc=34;goto leave_vga;}
        b=timer_ticks();isolated_setup+=b-a;
        if(!vga_rgb3_direct_reset_template()) {rc=39;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_group_baseline(
                (const u8 *const *)current_arg)) {rc=40;goto leave_vga;}
        b=timer_ticks();group_baseline+=b-a;
        if(!vga_rgb3_direct_reset_template()) {rc=41;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_groups(
                (const u8 *const *)current_arg)) {rc=42;goto leave_vga;}
        b=timer_ticks();group_scatter+=b-a;
        if(!vga_rgb3_direct_reset_template()) {rc=46;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_phase_baseline(
                (const u8 *const *)current_arg)) {rc=47;goto leave_vga;}
        b=timer_ticks();phase_baseline+=b-a;
        if(!vga_rgb3_direct_reset_template()) {rc=48;goto leave_vga;}
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_phase_groups(
                (const u8 *const *)current_arg)) {rc=49;goto leave_vga;}
        b=timer_ticks();phase_scatter+=b-a;
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_phase_kernel(
                (const u8 *const *)current_arg)) {rc=50;goto leave_vga;}
        b=timer_ticks();phase_kernel+=b-a;
        a=timer_ticks();
        if(!vga_rgb3_direct_scatter_phase_edges(
                (const u8 *const *)current_arg)) {rc=51;goto leave_vga;}
        b=timer_ticks();phase_edges+=b-a;
    }
    /* Twelve channel derivations equal four complete RGB parity triplets,
     * the parity volume generated by /RE:3 for these twelve DATA triplets. */
    a=timer_ticks();
    for(step=0;step<LOOPS;++step)
        if(!qrcodegen_dosferDeriveXor3V40L(current[0],current[1],current[2],
                parity_header_xor,parity_codewords)) {rc=22;goto leave_vga;}
    b=timer_ticks();parity_derive=b-a;

    delta_total=delta_build+delta_pack+delta_ecc+delta_raster+delta_present;
    direct_total=direct_build+direct_encode+direct_copy+direct_scatter+direct_present;
    asm_total=direct_build+direct_encode+asm_copy+asm_scatter+asm_present;
    fused_total=direct_build+delta_pack+fused_encode+asm_copy+asm_scatter+asm_present;
    stripe_total=direct_build+delta_pack+fused_encode+stripe_copy+
        stripe_scatter+stripe_present;
    production_grouped_total=direct_build+delta_pack+fused_encode+
        production_raster+stripe_present;
    printf("RGB3 production delta x%u: frame %lu ms, pack %lu ms, 3ECC %lu ms, raster %lu ms, present %lu ms, total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,delta_build),timer_elapsed_ms(0,delta_pack),timer_elapsed_ms(0,delta_ecc),
        timer_elapsed_ms(0,delta_raster),timer_elapsed_ms(0,delta_present),
        timer_elapsed_ms(0,delta_total),timer_elapsed_ms(0,delta_total)/(u32)LOOPS);
    printf("RGB3 direct C baseline x%u: frame %lu ms, pack+3ECC+interleave %lu ms, raster %lu ms, present %lu ms, total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,direct_build),timer_elapsed_ms(0,direct_encode),
        timer_elapsed_ms(0,direct_copy+direct_scatter),timer_elapsed_ms(0,direct_present),
        timer_elapsed_ms(0,direct_total),timer_elapsed_ms(0,direct_total)/(u32)LOOPS);
    printf("RGB3 3x-encode direct ASM x%u: raster %lu ms, present %lu ms, total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,asm_copy+asm_scatter),timer_elapsed_ms(0,asm_present),timer_elapsed_ms(0,asm_total),
        timer_elapsed_ms(0,asm_total)/(u32)LOOPS);
    printf("RGB3 production fused ASM x%u: total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,fused_total),
        timer_elapsed_ms(0,fused_total)/(u32)LOOPS);
    printf("RGB3 stripe ASM x%u: %u runs cover %u codewords; template %lu ms, scatter %lu ms (%lu/triplet), present %lu ms, fused total %lu ms (%lu/triplet)\n",
        LOOPS,stripe_runs,stripe_covered,timer_elapsed_ms(0,stripe_copy),
        timer_elapsed_ms(0,stripe_scatter),
        timer_elapsed_ms(0,stripe_scatter)/(u32)LOOPS,
        timer_elapsed_ms(0,stripe_present),timer_elapsed_ms(0,stripe_total),
        timer_elapsed_ms(0,stripe_total)/(u32)LOOPS);
    printf("RGB3 integrated phased production x%u: template+scatter %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,production_raster),
        timer_elapsed_ms(0,production_raster)/(u32)LOOPS);
    printf("RGB3 complete phased production x%u: total %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,production_grouped_total),
        timer_elapsed_ms(0,production_grouped_total)/(u32)LOOPS);
    printf("RGB3 stripe isolation x%u: covered runs %lu ms, fallback %lu ms, run setup %lu ms\n",
        LOOPS,timer_elapsed_ms(0,isolated_stripe),
        timer_elapsed_ms(0,isolated_fallback),timer_elapsed_ms(0,isolated_setup));
    printf("RGB3 stripe geometries C0/30/0C/03: runs %u/%u/%u/%u, codewords %u/%u/%u/%u; other %u runs %u codewords\n",
        geometry_runs[0],geometry_runs[1],geometry_runs[2],geometry_runs[3],
        geometry_codewords[0],geometry_codewords[1],geometry_codewords[2],
        geometry_codewords[3],other_runs,other_codewords);
    printf("RGB3 four-run groups: %u groups, %u source codewords -> %u destination bytes\n",
        stripe_groups,group_source_codewords,group_destination_bytes);
    printf("RGB3 phase groups: %u groups, %u source codewords -> %u destination bytes plus %u edge contributions\n",
        phase_groups,phase_source_codewords,phase_destination_bytes,
        phase_edge_contributions);
    printf("RGB3 grouped isolation x%u: original runs %lu ms, grouped byte scatter %lu ms; complete grouped scatter %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,group_baseline),
        timer_elapsed_ms(0,group_scatter),timer_elapsed_ms(0,grouped_full),
        timer_elapsed_ms(0,grouped_full)/(u32)LOOPS);
    printf("RGB3 phase isolation x%u: original runs %lu ms, phase byte scatter+edges %lu ms; complete phased scatter %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,phase_baseline),
        timer_elapsed_ms(0,phase_scatter),timer_elapsed_ms(0,phased_full),
        timer_elapsed_ms(0,phased_full)/(u32)LOOPS);
    printf("RGB3 phase components x%u: shifted group kernel %lu ms, 48 edges %lu ms\n",
        LOOPS,timer_elapsed_ms(0,phase_kernel),
        timer_elapsed_ms(0,phase_edges));
    printf("RGB3 direct QR x%u: encode %lu ms = pack %lu + ECC %lu + interleave/overhead %lu ms\n",
        LOOPS,timer_elapsed_ms(0,direct_encode),timer_elapsed_ms(0,delta_pack),
        timer_elapsed_ms(0,delta_ecc),timer_elapsed_ms(0,direct_encode-delta_pack-delta_ecc));
    printf("RGB3 isolated ECC x%u: 3x quad %lu ms, fused quad %lu ms (%ld ms, %ld%%)\n",
        LOOPS,timer_elapsed_ms(0,delta_ecc),timer_elapsed_ms(0,fused_ecc),
        (long)timer_elapsed_ms(0,fused_ecc)-(long)timer_elapsed_ms(0,delta_ecc),
        (long)(timer_elapsed_ms(0,fused_ecc)*100UL/timer_elapsed_ms(0,delta_ecc))-100L);
    printf("RGB3 fused QR x%u: ECC+interleave %lu ms; with pack %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,fused_encode),
        timer_elapsed_ms(0,fused_encode+delta_pack),
        timer_elapsed_ms(0,fused_encode+delta_pack)/(u32)LOOPS);
    printf("RGB3 protocol x%u triplets: raw copy %lu ms, copy+CRC %lu ms, whiten+CRC %lu ms, header-only %lu ms\n",
        LOOPS,timer_elapsed_ms(0,protocol_copy),timer_elapsed_ms(0,protocol_crc),
        timer_elapsed_ms(0,protocol_whiten),timer_elapsed_ms(0,protocol_header));
    printf("RGB3 exact scatter x%u: template copy %lu ms; C %lu ms (%lu/triplet); ASM %lu ms (%lu/triplet)\n",
        LOOPS,timer_elapsed_ms(0,direct_copy),timer_elapsed_ms(0,direct_scatter),
        timer_elapsed_ms(0,direct_scatter)/(u32)LOOPS,
        timer_elapsed_ms(0,asm_scatter),timer_elapsed_ms(0,asm_scatter)/(u32)LOOPS);
    printf("RGB3 /RE:3 parity construction: %u channel XOR3+repair operations %lu ms (%lu ms/parity triplet)\n",
        LOOPS,timer_elapsed_ms(0,parity_derive),
        timer_elapsed_ms(0,parity_derive)/(u32)(LOOPS/3));
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
    if(parity_codewords)_ffree(parity_codewords);
    for(channel=0;channel<3;channel++) {
        if(current_raw[channel])_ffree(current_raw[channel]);
        if(workspace[channel])_ffree(workspace[channel]);
        if(fused_raw[channel])_ffree(fused_raw[channel]);
        if(fused_codewords[channel])_ffree(fused_codewords[channel]);
    }
    return rc;
}
