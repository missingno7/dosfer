/* DOS/Open Watcom oracle for the RGB3 production direct renderer.
 *
 * Each production direct R/G/B triplet is compared first with the retained
 * delta oracle and then with an independently full-rendered triplet, both in
 * the RAM raster and in the actual VGA planes. The 13 iterations include the
 * reported frame-39 position. */
#include <stdio.h>
#include <dos.h>
#include <string.h>
#include <malloc.h>
#include "dosfer.h"
#include "qrcodegen.h"
#include "vga.h"

#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(40)
#define DATA_CODEWORDS 2956
#define CODEWORDS 3706

static u8 frame[2952];

static u8 far *align_far16(u8 far *p) {
    u32 paragraphs=((u32)FP_OFF(p)+15UL)>>4;
    return (u8 far *)MK_FP((u16)(FP_SEG(p)+paragraphs),0);
}

static void fill_frame(unsigned int sequence,unsigned int len) {
    unsigned long state=0x1F123BB5UL^(unsigned long)sequence*0x45D9F3BUL;
    unsigned int i;
    for(i=0;i<len;i++) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        frame[i]=(u8)state;
    }
}

int main(void) {
    u8 far *current[3]={0,0,0};
    u8 far *workspace[3]={0,0,0};
    u8 far *workspace_raw[3]={0,0,0};
    u8 far *fused_codewords[3]={0,0,0};
    u8 far *fused_codewords_raw[3]={0,0,0};
    u8 far *reference_codewords[3]={0,0,0};
    u8 far *reference_qr[3]={0,0,0};
    const u8 *data[3],*ecc[3],*qr[3];
    u8 *current_arg[3];
    unsigned int triplet,channel,i;
    u32 delta_hash,direct_hash,full_hash;
    int rc=0;

    for(channel=0;channel<3;channel++) {
        current[channel]=(u8 far *)_fmalloc(CODEWORDS);
        workspace_raw[channel]=(u8 far *)_fmalloc(QR_BUFFER+15);
        if(workspace_raw[channel])workspace[channel]=align_far16(workspace_raw[channel]);
        fused_codewords_raw[channel]=(u8 far *)_fmalloc(CODEWORDS+15);
        if(fused_codewords_raw[channel])
            fused_codewords[channel]=align_far16(fused_codewords_raw[channel]);
        reference_codewords[channel]=(u8 far *)_fmalloc(CODEWORDS);
        reference_qr[channel]=(u8 far *)_fmalloc(QR_BUFFER);
        if(!current[channel]||!workspace[channel]||!fused_codewords[channel]||
                !reference_codewords[channel]||!reference_qr[channel]) {
            puts("FAIL: could not allocate RGB VGA test buffers");
            rc=2;goto done;
        }
        fill_frame(channel,96+channel*31);
        if(!qrcodegen_dosferEncodeFrameV40L(frame,96+channel*31,
                current[channel],workspace[channel],qrcodegen_Mask_0,false)) {
            puts("FAIL: initial RGB QR encoding failed");rc=3;goto done;
        }
        qr[channel]=workspace[channel];current_arg[channel]=current[channel];
    }
    if(!vga_enter(VIDEO_320_60,1)) {puts("FAIL: VGA RGB mode unavailable");rc=4;goto done;}
    if(!vga_show_full_qr3_at(qr,current_arg,0,"RGB VGA TEST",0) ||
            !vga_display_matches()) {
        puts("FAIL: initial RGB VGA full render mismatch");rc=5;goto leave_vga;
    }

    for(triplet=0;triplet<13;triplet++) {
        for(channel=0;channel<3;channel++) {
            unsigned int len=(unsigned int)(80+((triplet*71+channel*43)%2800));
            fill_frame(triplet*3+channel+3,len);
            if(!qrcodegen_dosferPackFrameV40L(frame,len,workspace[channel])) {
                puts("FAIL: RGB delta pack failed");rc=6;goto leave_vga;
            }
            qrcodegen_dosferComputeEccBlocksV40L(workspace[channel],
                workspace[channel]+DATA_CODEWORDS);
            if(!qrcodegen_dosferEncodeFrameV40L(frame,len,
                    reference_codewords[channel],reference_qr[channel],
                    qrcodegen_Mask_0,false)) {
                puts("FAIL: RGB reference encode failed");rc=7;goto leave_vga;
            }
            data[channel]=workspace[channel];
            ecc[channel]=workspace[channel]+DATA_CODEWORDS;
            current_arg[channel]=current[channel];
        }
        if(!qrcodegen_dosferEncodePrepacked3V40L(workspace,fused_codewords)) {
            printf("FAIL: RGB triplet %u fused encode failed\n",triplet+1);
            rc=8;goto leave_vga;
        }
        for(channel=0;channel<3;channel++)for(i=0;i<CODEWORDS;i++)
            if(fused_codewords[channel][i]!=reference_codewords[channel][i]) {
                printf("FAIL: RGB triplet %u lane %u fused codeword %u %02X ref %02X\n",
                    triplet+1,channel+1,i,fused_codewords[channel][i],
                    reference_codewords[channel][i]);
                rc=9;goto leave_vga;
            }
        if(!vga_apply_v40l_delta3(data,ecc,current_arg)) {
            puts("FAIL: RGB delta application failed");rc=10;goto leave_vga;
        }
        for(channel=0;channel<3;channel++)for(i=0;i<CODEWORDS;i++)
            if(current[channel][i]!=reference_codewords[channel][i]) {
                printf("FAIL: RGB triplet %u lane %u codeword %u delta %02X ref %02X\n",
                    triplet+1,channel+1,i,current[channel][i],
                    reference_codewords[channel][i]);
                rc=11;goto leave_vga;
            }
        if(!vga_show_prepared3_at(0,"RGB VGA TEST",0) ||
                !vga_display_matches()) {
            printf("FAIL: RGB triplet %u delta VGA plane mismatch\n",triplet+1);
            rc=12;goto leave_vga;
        }
        delta_hash=vga_screen_hash();
        if(!vga_apply_codewords3_direct((const u8 *const *)fused_codewords)) {
            printf("FAIL: RGB triplet %u production direct render failed\n",triplet+1);
            rc=13;goto leave_vga;
        }
        direct_hash=vga_screen_hash();
        if(direct_hash!=delta_hash) {
            printf("FAIL: RGB triplet %u direct raster %08lX delta %08lX\n",
                triplet+1,direct_hash,delta_hash);
            rc=14;goto leave_vga;
        }
        if(!vga_show_prepared3_at(0,"RGB VGA TEST",0) ||
                !vga_display_matches()) {
            printf("FAIL: RGB triplet %u direct VGA plane mismatch\n",triplet+1);
            rc=15;goto leave_vga;
        }
        for(channel=0;channel<3;channel++) {
            qr[channel]=reference_qr[channel];
            current_arg[channel]=reference_codewords[channel];
        }
        if(!vga_show_full_qr3_at(qr,current_arg,0,"RGB VGA TEST",0) ||
                !vga_display_matches()) {
            printf("FAIL: RGB triplet %u full VGA plane mismatch\n",triplet+1);
            rc=16;goto leave_vga;
        }
        full_hash=vga_screen_hash();
        if(delta_hash!=full_hash) {
            printf("FAIL: RGB triplet %u delta raster %08lX full %08lX\n",
                triplet+1,delta_hash,full_hash);
            rc=17;goto leave_vga;
        }
    }
    puts("PASS: fused RGB3 codewords and direct R/G/B rasters match canonical paths through frame 39");

leave_vga:
    vga_leave();
done:
    for(channel=0;channel<3;channel++) {
        if(current[channel])_ffree(current[channel]);
        if(workspace_raw[channel])_ffree(workspace_raw[channel]);
        if(fused_codewords_raw[channel])_ffree(fused_codewords_raw[channel]);
        if(reference_codewords[channel])_ffree(reference_codewords[channel]);
        if(reference_qr[channel])_ffree(reference_qr[channel]);
    }
    return rc;
}
