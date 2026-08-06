#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <malloc.h>
#include <string.h>
#include "vga.h"
#include "qrcodegen.h"
#include "timing.h"

#define VGA_BYTES_PER_LINE 40
#define VGA_HEIGHT 200
#define VGA_PAGE_BYTES 0x2000u
#define VGA_VISIBLE_BYTES (VGA_BYTES_PER_LINE * VGA_HEIGHT)
#define VGA_STATUS_ROW 192
#define VGA_STATUS_ROWS 8
#define QR_TOTAL (DOSFER_QR_SIZE + DOSFER_QR_QUIET * 2)
#define QR_X0 ((320 - QR_TOTAL) / 2)
#define QR_Y0 DOSFER_QR_QUIET
#define QR_DATA_BITS (DOSFER_QR_CODEWORDS * 8)

static u8 far *font8;
static int vga_active;
static int screen_invert=-1;
static int display_page;
static int write_page;
static u8 page_initialized[2];
static u8 page_invert[2];

/* Persistent shadow raster and QR delta state. */
static u8
#ifdef __WATCOMC__
    __near
#endif
    screen_320[VGA_VISIBLE_BYTES];
static u8
#ifdef __WATCOMC__
    __near
#endif
    previous_codewords[DOSFER_QR_CODEWORDS];
static u16 far *delta_offset;
static u8 far *delta_mask;
static u16 delta_bits;
static u16 delta_codewords;

#ifdef DOSFER_PROFILE
/* render, VGA upload/setup, retrace wait, page flip, status drawing */
u32 dosferVgaProfileTicks[5];
#endif

#ifdef __WATCOMC__
static u8 far *dosferFont8(void);
#pragma aux dosferFont8 = \
    "push bp" \
    "mov ax,1130h" \
    "mov bh,3" \
    "int 10h" \
    "mov dx,es" \
    "mov ax,bp" \
    "pop bp" \
    value [dx ax] modify [bx];

static void dosferCopyFull320(const u8 far *src,u8 far *dest);
#pragma aux dosferCopyFull320 = \
    "mov cx,2000" \
    "copy320_loop:" \
    "mov eax,fs:[si]" \
    "mov es:[di],eax" \
    "add si,4" \
    "add di,4" \
    "loop copy320_loop" \
    parm [fs si] [es di] modify [ax cx si di];

/* V40+quiet-zone occupies rows 0..184 and bytes 8..31.  Rows 185..191
 * stay at the page background; rows 192..199 hold the one-line status. */
static void dosferCopyPartial320(const u8 far *src,u8 far *dest);
#pragma aux dosferCopyPartial320 = \
    "mov cx,185" \
    "copy320_rows:" \
    "mov eax,fs:[si+8]"  "mov es:[di+8],eax" \
    "mov eax,fs:[si+12]" "mov es:[di+12],eax" \
    "mov eax,fs:[si+16]" "mov es:[di+16],eax" \
    "mov eax,fs:[si+20]" "mov es:[di+20],eax" \
    "mov eax,fs:[si+24]" "mov es:[di+24],eax" \
    "mov eax,fs:[si+28]" "mov es:[di+28],eax" \
    "add si,40" \
    "add di,40" \
    "loop copy320_rows" \
    "add si,280" \
    "add di,280" \
    "mov cx,80" \
    "copy320_status:" \
    "mov eax,fs:[si]" \
    "mov es:[di],eax" \
    "add si,4" \
    "add di,4" \
    "loop copy320_status" \
    parm [fs si] [es di] modify [ax cx si di];
#else
static u8 far *dosferFont8(void) { return 0; }
static void dosferCopyFull320(const u8 far *src,u8 far *dest) {
    _fmemcpy(dest,src,VGA_VISIBLE_BYTES);
}
static void dosferCopyPartial320(const u8 far *src,u8 far *dest) {
    int row;
    for(row=0;row<185;++row)
        _fmemcpy(dest+(u32)row*VGA_BYTES_PER_LINE+8,
                 src+(u32)row*VGA_BYTES_PER_LINE+8,24);
    _fmemcpy(dest+(u32)VGA_STATUS_ROW*VGA_BYTES_PER_LINE,
             src+(u32)VGA_STATUS_ROW*VGA_BYTES_PER_LINE,
             VGA_STATUS_ROWS*VGA_BYTES_PER_LINE);
}
#endif

static void free_delta(void) {
    if(delta_offset)_ffree(delta_offset);
    if(delta_mask)_ffree(delta_mask);
    delta_offset=0;
    delta_mask=0;
    delta_bits=0;
    delta_codewords=0;
}

static void bios_mode(u8 mode) {
    union REGS r;
    memset(&r,0,sizeof(r));
    r.h.ah=0;
    r.h.al=mode;
    int86(0x10,&r,&r);
}

/* Mode 0Dh is normally ~70 Hz.  Keep its 320x200 planar memory layout and
 * double-scanned 400-line active image, but extend the vertical period to
 * 525 physical scanlines: ~59.94 Hz with the standard 25.175 MHz VGA clock. */
static void set_320x200_60hz(void) {
    u16 crtc=(inp(0x3CC)&1)?0x3D4:0x3B4;

    outp(crtc,0x11);outp(crtc+1,0x00); /* unlock CR00..CR07 */
    outp(crtc,0x06);outp(crtc+1,0x0B); /* vertical total = 525 */
    outp(crtc,0x07);outp(crtc+1,0x3E); /* overflow bits */
    outp(crtc,0x16);outp(crtc+1,0x0B); /* vertical blank end */
    outp(crtc,0x10);outp(crtc+1,0xD7); /* vertical retrace start */
    outp(crtc,0x11);outp(crtc+1,0x89); /* retrace end + lock */
}

static void setup_planar_write(void) {
    outp(0x3C4,2);outp(0x3C5,0x0F); /* write all four planes */
    outp(0x3CE,0);outp(0x3CF,0);
    outp(0x3CE,1);outp(0x3CF,0);
    outp(0x3CE,3);outp(0x3CF,0);
    outp(0x3CE,5);outp(0x3CF,0);
    outp(0x3CE,8);outp(0x3CF,0xFF);
}

int vga_enter(VideoMode mode) {
    free_delta();
    bios_mode(0x0D);
    if(mode==VIDEO_320_60)set_320x200_60hz();
    else if(mode!=VIDEO_320_70){bios_mode(3);return 0;}

    vga_active=1;
    display_page=0;
    write_page=1;
    screen_invert=-1;
    page_initialized[0]=page_initialized[1]=0;
    font8=dosferFont8();
    return 1;
}

void vga_leave(void) {
    free_delta();
    screen_invert=-1;
    if(vga_active){
        bios_mode(3);
        vga_active=0;
    }
}

void vga_wait_retrace(void) {
    while(inp(0x3DA)&8) ;
    while(!(inp(0x3DA)&8)) ;
}

static void status_text_320(const char *s) {
    int x,y;
    u8 far *glyph;
    u8 bg=screen_invert?0x00:0xFF;

    for(y=VGA_STATUS_ROW;y<VGA_HEIGHT;++y)
        memset(screen_320+y*VGA_BYTES_PER_LINE,bg,VGA_BYTES_PER_LINE);
    if(!font8||!s)return;

    for(x=0;s[x]&&x<40;++x){
        glyph=font8+(u16)(u8)s[x]*8;
        for(y=0;y<8;++y)
            screen_320[(VGA_STATUS_ROW+y)*VGA_BYTES_PER_LINE+x]=
                screen_invert?glyph[y]:(u8)~glyph[y];
    }
}

static int build_qr_image_320(const u8 *qr,int invert) {
    int my,mx,index,dark,px;
    u8 mask;

    if(!qr)return 0;
    memset(screen_320,invert?0x00:0xFF,sizeof(screen_320));
    screen_invert=invert;

    for(my=0;my<DOSFER_QR_SIZE;++my){
        index=my*DOSFER_QR_SIZE;
        for(mx=0;mx<DOSFER_QR_SIZE;++mx){
            dark=(qr[(index>>3)+1]>>(index&7))&1;
            ++index;
            if(!dark)continue;
            px=QR_X0+DOSFER_QR_QUIET+mx;
            mask=(u8)(0x80>>(px&7));
            if(invert)
                screen_320[(QR_Y0+my)*VGA_BYTES_PER_LINE+(px>>3)]|=mask;
            else
                screen_320[(QR_Y0+my)*VGA_BYTES_PER_LINE+(px>>3)]&=(u8)~mask;
        }
    }
    return 1;
}

static void copy_320_flip(void) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);
    union REGS r;
    u16 base=(u16)(write_page<<13);
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks(),profile_now;
#endif

    setup_planar_write();
    if(!page_initialized[write_page]||page_invert[write_page]!=(u8)screen_invert)
        dosferCopyFull320(screen_320,vram+base);
    else
        dosferCopyPartial320(screen_320,vram+base);
    page_initialized[write_page]=1;
    page_invert[write_page]=(u8)screen_invert;

#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[1]+=profile_now-profile_start;
    profile_start=profile_now;
#endif
    vga_wait_retrace();
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[2]+=profile_now-profile_start;
    profile_start=profile_now;
#endif

    memset(&r,0,sizeof(r));
    r.h.ah=5;
    r.h.al=(u8)write_page;
    int86(0x10,&r,&r);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[3]+=profile_now-profile_start;
#endif
    display_page=write_page;
    write_page^=1;
}

static int prepare_delta(const u8 *codewords,u16 codeword_len) {
    const u16 *bytes=qrcodegen_dosferPlacementBytes();
    const u8 *masks=qrcodegen_dosferPlacementMasks();
    int bits=qrcodegen_dosferPlacementBits();
    int i,bit,x,y,px;
    u16 linear;
    u8 m;

    if(!bytes||!masks||!codewords||bits<=0||bits!=(int)codeword_len*8)return 0;
    if(bits>QR_DATA_BITS||codeword_len>DOSFER_QR_CODEWORDS)return 0;

    free_delta();
    delta_offset=(u16 far *)_fmalloc((u32)bits*sizeof(u16));
    delta_mask=(u8 far *)_fmalloc((u32)bits);
    if(!delta_offset||!delta_mask){free_delta();return 0;}

    for(i=0;i<bits;++i){
        m=masks[i];
        bit=0;
        while(((u8)1<<bit)!=m)++bit;
        linear=(u16)(((bytes[i]-1)<<3)+bit);
        y=linear/DOSFER_QR_SIZE;
        x=linear-y*DOSFER_QR_SIZE;
        px=QR_X0+DOSFER_QR_QUIET+x;
        delta_offset[i]=(u16)((QR_Y0+y)*VGA_BYTES_PER_LINE+(px>>3));
        delta_mask[i]=(u8)(0x80>>(px&7));
    }

    _fmemcpy(previous_codewords,codewords,codeword_len);
    delta_bits=(u16)bits;
    delta_codewords=codeword_len;
    qrcodegen_dosferReleaseMatrixCache();
    return 1;
}

static void update_delta(const u8 *codewords) {
    const u16 far *offset=delta_offset;
    const u8 far *masks=delta_mask;
    u16 i,off;
    u8 changed;

    for(i=0;i+1<delta_codewords;i+=2,offset+=16,masks+=16){
        changed=(u8)(codewords[i]^previous_codewords[i]);
        previous_codewords[i]=codewords[i];
#define TOGGLE(k,b) if(changed&(b)){off=offset[k];screen_320[off]^=masks[k];}
        TOGGLE(0,0x80) TOGGLE(1,0x40) TOGGLE(2,0x20) TOGGLE(3,0x10)
        TOGGLE(4,0x08) TOGGLE(5,0x04) TOGGLE(6,0x02) TOGGLE(7,0x01)
        changed=(u8)(codewords[i+1]^previous_codewords[i+1]);
        previous_codewords[i+1]=codewords[i+1];
        TOGGLE(8,0x80) TOGGLE(9,0x40) TOGGLE(10,0x20) TOGGLE(11,0x10)
        TOGGLE(12,0x08) TOGGLE(13,0x04) TOGGLE(14,0x02) TOGGLE(15,0x01)
#undef TOGGLE
    }
    if(i<delta_codewords){
        changed=(u8)(codewords[i]^previous_codewords[i]);
        previous_codewords[i]=codewords[i];
#define TOGGLE_TAIL(k,b) if(changed&(b)){off=offset[k];screen_320[off]^=masks[k];}
        TOGGLE_TAIL(0,0x80) TOGGLE_TAIL(1,0x40) TOGGLE_TAIL(2,0x20) TOGGLE_TAIL(3,0x10)
        TOGGLE_TAIL(4,0x08) TOGGLE_TAIL(5,0x04) TOGGLE_TAIL(6,0x02) TOGGLE_TAIL(7,0x01)
#undef TOGGLE_TAIL
    }
}

int vga_delta_ready(void) {
    return delta_offset!=0;
}

void vga_delta_stats(u16 *bits) {
    if(bits)*bits=delta_bits;
}

u32 vga_screen_hash(void) {
    u32 i,h=2166136261UL;
    /* Ignore status rows so full redraw and streaming hashes are comparable. */
    for(i=0;i<7680;++i){h^=screen_320[i];h*=16777619UL;}
    return h;
}

int vga_display_matches(void) {
    u8 far *vram;
    u16 i;
    outp(0x3CE,4);outp(0x3CF,0); /* all four display planes are identical */
    vram=(u8 far *)MK_FP(0xA000,(u16)(display_page<<13));
    for(i=0;i<VGA_VISIBLE_BYTES;++i)
        if(vram[i]!=screen_320[i])return 0;
    return 1;
}

int vga_show_qr_stream(const u8 *qr,const u8 *codewords,u16 codeword_len,
                       int invert,const char *status,int delta_only) {
#ifdef DOSFER_PROFILE
    u32 profile_start,profile_now;
#endif
    if(!vga_active||!qr||!codewords)return 0;
#ifdef DOSFER_PROFILE
    profile_start=timer_ticks();
#endif
    if(delta_only){
        if(!delta_offset||delta_codewords!=codeword_len||screen_invert!=invert)return 0;
        update_delta(codewords);
    } else {
        if(!build_qr_image_320(qr,invert))return 0;
        /* Delta acceleration is optional. A low-memory machine must still
         * be able to transmit using canonical full redraws. */
        prepare_delta(codewords,codeword_len);
    }
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[0]+=profile_now-profile_start;
    profile_start=profile_now;
#endif
    status_text_320(status);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[4]+=profile_now-profile_start;
#endif
    copy_320_flip();
    return 1;
}

int vga_show_qr(const u8 *qr,int invert,const char *status) {
    if(!vga_active||!build_qr_image_320(qr,invert))return 0;
    status_text_320(status);
    copy_320_flip();
    return 1;
}

void vga_benchmark_qr(const u8 *qr,int loops,u32 *build_ms,u32 *copy_ms,u32 *text_ms) {
    int i;
    u32 a,b;
    a=timer_ticks();for(i=0;i<loops;++i)build_qr_image_320(qr,0);b=timer_ticks();
    *build_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)copy_320_flip();b=timer_ticks();
    *copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)status_text_320("DOSfer V40 320x200 benchmark");b=timer_ticks();
    *text_ms=timer_elapsed_ms(a,b);
}

void vga_benchmark_delta(const u8 *codewords,u16 codeword_len,int loops,
                         u32 *update_ms,u32 *copy_ms,u32 *text_ms) {
    u8 far *alternate=(u8 far *)_fmalloc(codeword_len);
    int i;
    u32 a,b,state=0x6A67C69DUL;

    if(!alternate||!delta_offset){
        *update_ms=*copy_ms=*text_ms=0;
        if(alternate)_ffree(alternate);
        return;
    }
    for(i=0;i<codeword_len;++i){
        state^=state<<13;state^=state>>17;state^=state<<5;
        alternate[i]=(u8)(codewords[i]^(u8)state);
    }
    a=timer_ticks();for(i=0;i<loops;++i)update_delta((i&1)?codewords:alternate);b=timer_ticks();
    *update_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)copy_320_flip();b=timer_ticks();
    *copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)status_text_320("DOSfer V40 delta benchmark");b=timer_ticks();
    *text_ms=timer_elapsed_ms(a,b);
    _ffree(alternate);
}

void speaker_beep(void) {
    u16 div=1193180UL/880;
    u8 old=inp(0x61);
    outp(0x43,0xB6);
    outp(0x42,(u8)div);
    outp(0x42,(u8)(div>>8));
    outp(0x61,old|3);
    delay(80);
    outp(0x61,old);
}
