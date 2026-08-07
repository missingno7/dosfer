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
static int rgb3_active;
static int vga_dac_present;
static int screen_invert=-1;
static int write_page;
#ifdef DOSFER_DEVTOOLS
static int display_page;
#endif
static u16 crtc_port;
static u16 page_start[2];
static u32 last_flip_tick;
static u8 page_initialized[2];
static u8 page_invert[2];
static u32 status_generation;
static u32 page_status_generation[2];
static int status_active;

/* Persistent shadow raster and QR delta state. */
static u8
#ifdef __WATCOMC__
    __near
#endif
    screen_320[VGA_VISIBLE_BYTES],
    screen_green[VGA_VISIBLE_BYTES],
    screen_blue[VGA_VISIBLE_BYTES];
/* One 16-bit entry per QR codeword bit: low 13 bits are the 0..7999
 * shadow-raster byte offset, high 3 bits select the pixel bit in that byte.
 * This replaces the old 16-bit offset + separate 8-bit mask tables, saving
 * 29,648 bytes and one far-memory load for every changed module. */
static u16 far *delta_entry;
#ifdef DOSFER_DEVTOOLS
static u16 delta_bits;
#endif
static const u8 pixel_mask[8]={0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01};

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

/* V40+quiet-zone occupies rows 0..184 and bytes 8..31.  Keep the recurring
 * upload to this 24-byte strip; the status rows are copied only when their
 * contents actually change. */
static void dosferCopyQr320(const u8 far *src,u8 far *dest);
#pragma aux dosferCopyQr320 = \
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
    parm [fs si] [es di] modify [ax cx si di];

static void dosferCopyStatus320(const u8 far *src,u8 far *dest);
#pragma aux dosferCopyStatus320 = \
    "mov cx,80" \
    "copy320_status:" \
    "mov eax,fs:[si]" \
    "mov es:[di],eax" \
    "add si,4" \
    "add di,4" \
    "loop copy320_status" \
    parm [fs si] [es di] modify [ax cx si di];

static void dosferClearFull320(u8 far *dest);
#pragma aux dosferClearFull320 = \
    "xor eax,eax" \
    "mov cx,2000" \
    "clear320_loop:" \
    "mov es:[di],eax" \
    "add di,4" \
    "loop clear320_loop" \
    parm [es di] modify [ax cx di];
#else
static u8 far *dosferFont8(void) { return 0; }
static void dosferCopyFull320(const u8 far *src,u8 far *dest) {
    _fmemcpy(dest,src,VGA_VISIBLE_BYTES);
}
static void dosferCopyQr320(const u8 far *src,u8 far *dest) {
    int row;
    for(row=0;row<185;++row)
        _fmemcpy(dest+(u32)row*VGA_BYTES_PER_LINE+8,
                 src+(u32)row*VGA_BYTES_PER_LINE+8,24);
}
static void dosferCopyStatus320(const u8 far *src,u8 far *dest) {
    _fmemcpy(dest,src,VGA_STATUS_ROWS*VGA_BYTES_PER_LINE);
}
static void dosferClearFull320(u8 far *dest) {
    _fmemset(dest,0,VGA_VISIBLE_BYTES);
}
#endif

static void free_delta(void) {
    if(delta_entry)free(delta_entry);
    delta_entry=0;
#ifdef DOSFER_DEVTOOLS
    delta_bits=0;
#endif
}

static void bios_mode(u8 mode) {
    union REGS r;
    memset(&r,0,sizeof(r));
    r.h.ah=0;
    r.h.al=mode;
    int86(0x10,&r,&r);
}

static void bios_page(u8 page) {
    union REGS r;
    memset(&r,0,sizeof(r));
    r.h.ah=5;
    r.h.al=page;
    int86(0x10,&r,&r);
}

static u16 read_crtc_start(void) {
    u16 start;
    outp(crtc_port,0x0C);start=(u16)inp(crtc_port+1)<<8;
    outp(crtc_port,0x0D);start|=(u8)inp(crtc_port+1);
    return start;
}

static int bios_vga_present(void) {
    union REGS r;
    memset(&r,0,sizeof(r));
    r.x.ax=0x1A00;
    int86(0x10,&r,&r);
    return r.h.al==0x1A;
}

static void set_map_mask(u8 mask) {
    outp(0x3C4,2);
    outp(0x3C5,mask);
}

/* Logical pixel indexes 0..7 are the three RGB QR bits.  Keep the EGA
 * secondary/intensity bits clear so an inactive channel emits no light from
 * that primary: 000 black, 001 blue, 010 green, ... 111 white.  VGA maps the
 * same eight Attribute Controller entries to full-intensity DAC primaries. */
static void setup_rgb3_palette(void) {
    static const u8 ega[8]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
    static const u8 rgb[8][3]={
        {0,0,0},{0,0,63},{0,63,0},{0,63,63},
        {63,0,0},{63,0,63},{63,63,0},{63,63,63}
    };
    int i;

    (void)inp(0x3DA);
    for(i=0;i<8;++i) {
        outp(0x3C0,i);
        outp(0x3C0,ega[i]);
    }
    outp(0x3C0,0x20); /* re-enable video */

    if(!vga_dac_present)return;
    for(i=0;i<8;++i) {
        outp(0x3C8,ega[i]);
        outp(0x3C9,rgb[i][0]);
        outp(0x3C9,rgb[i][1]);
        outp(0x3C9,rgb[i][2]);
    }
}

/* Mode 0Dh is normally ~70 Hz.  Keep its 320x200 planar memory layout and
 * double-scanned 400-line active image, but extend the vertical period to
 * 525 physical scanlines: ~59.94 Hz with the standard 25.175 MHz VGA clock. */
static void set_320x200_60hz(void) {
    outp(crtc_port,0x11);outp(crtc_port+1,0x00); /* unlock CR00..CR07 */
    outp(crtc_port,0x06);outp(crtc_port+1,0x0B); /* vertical total = 525 */
    outp(crtc_port,0x07);outp(crtc_port+1,0x3E); /* overflow bits */
    outp(crtc_port,0x16);outp(crtc_port+1,0x0B); /* vertical blank end */
    outp(crtc_port,0x10);outp(crtc_port+1,0xD7); /* vertical retrace start */
    outp(crtc_port,0x11);outp(crtc_port+1,0x89); /* retrace end + lock */
}

static void setup_planar_write(void) {
    set_map_mask(0x0F); /* legacy BW writes all four planes */
    outp(0x3CE,0);outp(0x3CF,0);
    outp(0x3CE,1);outp(0x3CF,0);
    outp(0x3CE,3);outp(0x3CF,0);
    outp(0x3CE,5);outp(0x3CF,0);
    outp(0x3CE,8);outp(0x3CF,0xFF);
}

int vga_enter(VideoMode mode,int rgb3) {
    free_delta();
    vga_dac_present=bios_vga_present();
    bios_mode(0x0D);
    crtc_port=(inp(0x3CC)&1)?0x3D4:0x3B4;
    /* The custom 59.94-Hz timing uses VGA CRTC semantics.  A real EGA keeps
     * its native Mode 0Dh timing instead of being programmed with VGA values. */
    if(mode==VIDEO_320_60&&vga_dac_present)set_320x200_60hz();
    else if(mode!=VIDEO_320_60&&mode!=VIDEO_320_70){bios_mode(3);return 0;}

    /* Ask the BIOS for the two page starts once, then use the measured CRTC
     * values directly during streaming. This preserves adapter-specific page
     * semantics without paying INT 10h overhead on every QR transition. */
    bios_page(0);page_start[0]=read_crtc_start();
    bios_page(1);page_start[1]=read_crtc_start();
    bios_page(0);
    if(page_start[0]==page_start[1]){bios_mode(3);return 0;}

    vga_active=1;
    rgb3_active=rgb3!=0;
#ifdef DOSFER_DEVTOOLS
    display_page=0;
#endif
    write_page=1;
    last_flip_tick=0;
    screen_invert=-1;
    page_initialized[0]=page_initialized[1]=0;
    status_generation=1;
    page_status_generation[0]=page_status_generation[1]=0;
    status_active=0;
    font8=dosferFont8();
    /* The release backend never changes write mode or map mask while the
     * graphics mode is active, so program the planar-write state once rather
     * than repeating the same VGA port writes for every QR upload. */
    setup_planar_write();
    if(rgb3_active)setup_rgb3_palette();
    return 1;
}

void vga_leave(void) {
    free_delta();
    screen_invert=-1;
    rgb3_active=0;
    if(vga_active){
        bios_mode(3);
        vga_active=0;
    }
}

#ifdef DOSFER_DEVTOOLS
static void wait_next_retrace(void) {
    while(inp(0x3DA)&8) ;
    while(!(inp(0x3DA)&8)) ;
}

u32 vga_measure_refresh_hz100(u16 samples) {
    u16 i;
    u32 start,elapsed;
    if(!vga_active)return 0;
    if(samples<2)samples=2;
    wait_next_retrace();
    start=timer_ticks();
    for(i=0;i<samples;++i)wait_next_retrace();
    elapsed=timer_elapsed_ms(start,timer_ticks());
    return elapsed?((u32)samples*100000UL)/elapsed:0;
}
#endif

/* Wait for the first vertical-retrace interval that is not earlier than the
 * requested PIT deadline.  Polling from before the target avoids the old
 * HOLD:50 + "one more retrace" penalty; at 59.94 Hz, 50 ms now naturally
 * lands on the third refresh (~50.05 ms). */
static void wait_retrace_at_or_after(u32 earliest_tick) {
    for(;;) {
        if(inp(0x3DA)&8) {
            do {
                if(!earliest_tick||(long)(timer_ticks()-earliest_tick)>=0)return;
            } while(inp(0x3DA)&8);
        }
        while(!(inp(0x3DA)&8)) ;
        if(!earliest_tick||(long)(timer_ticks()-earliest_tick)>=0)return;
    }
}

/* Page-start values are learned from BIOS AH=05h during initialization.
 * Streaming then writes those exact CRTC values directly, avoiding BIOS
 * overhead while retaining the adapter's own Mode 0Dh page convention. */
static void select_display_page(unsigned page) {
    u16 start=page_start[page&1];
    outp(crtc_port,0x0C);outp(crtc_port+1,(u8)(start>>8));
    outp(crtc_port,0x0D);outp(crtc_port+1,(u8)start);
}

u32 vga_last_flip_tick(void) { return last_flip_tick; }

static u8 *rgb_screen(int channel) {
    if(channel==VGA_RGB_GREEN)return screen_green;
    if(channel==VGA_RGB_BLUE)return screen_blue;
    return screen_320;
}

static void status_text_buffer(u8 *screen,const char *s) {
    int x,y;
    u8 far *glyph;
    u8 bg=screen_invert?0x00:0xFF;

    for(y=VGA_STATUS_ROW;y<VGA_HEIGHT;++y)
        memset(screen+y*VGA_BYTES_PER_LINE,bg,VGA_BYTES_PER_LINE);
    if(!font8||!s)return;
    for(x=0;s[x]&&x<40;++x) {
        glyph=font8+(u16)(u8)s[x]*8;
        for(y=0;y<8;++y)
            screen[(VGA_STATUS_ROW+y)*VGA_BYTES_PER_LINE+x]=
                screen_invert?glyph[y]:(u8)~glyph[y];
    }
}

static void status_text_320(const char *s) {
    if(!s&&!status_active)return;
    status_text_buffer(screen_320,s);
    status_active=s!=0;
    ++status_generation;
}

static void status_text_rgb3(const char *s) {
    int channel;
    if(!s&&!status_active)return;
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
        status_text_buffer(rgb_screen(channel),s);
    status_active=s!=0;
    ++status_generation;
}

static int build_qr_image_buffer(u8 *screen,const u8 *qr,int invert) {
    int my,mx,index,dark,px;
    u8 mask;

    if(!screen||!qr)return 0;
    memset(screen,invert?0x00:0xFF,VGA_VISIBLE_BYTES);
    for(my=0;my<DOSFER_QR_SIZE;++my) {
        index=my*DOSFER_QR_SIZE;
        for(mx=0;mx<DOSFER_QR_SIZE;++mx) {
            dark=(qr[(index>>3)+1]>>(index&7))&1;
            ++index;
            if(!dark)continue;
            px=QR_X0+DOSFER_QR_QUIET+mx;
            mask=(u8)(0x80>>(px&7));
            if(invert)
                screen[(QR_Y0+my)*VGA_BYTES_PER_LINE+(px>>3)]|=mask;
            else
                screen[(QR_Y0+my)*VGA_BYTES_PER_LINE+(px>>3)]&=(u8)~mask;
        }
    }
    return 1;
}

static int build_qr_image_320(const u8 *qr,int invert) {
    screen_invert=invert;
    return build_qr_image_buffer(screen_320,qr,invert);
}

static int build_qr_image_rgb3(const u8 *const qr[VGA_RGB_CHANNELS],int invert) {
    int channel;
    screen_invert=invert;
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
        if(!build_qr_image_buffer(rgb_screen(channel),qr[channel],invert))return 0;
    return 1;
}

static void finish_page_flip(u32 earliest_tick) {
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks(),profile_now;
#endif
    wait_retrace_at_or_after(earliest_tick);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[2]+=profile_now-profile_start;
    profile_start=profile_now;
#endif
    select_display_page((unsigned)write_page);
    last_flip_tick=timer_ticks();
#ifdef DOSFER_PROFILE
    profile_now=last_flip_tick;dosferVgaProfileTicks[3]+=profile_now-profile_start;
#endif
#ifdef DOSFER_DEVTOOLS
    display_page=write_page;
#endif
    write_page^=1;
}

static void copy_320_flip(u32 earliest_tick) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);
    u16 base=(u16)(write_page<<13);
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks(),profile_now;
#endif

    set_map_mask(0x0F);
    if(!page_initialized[write_page]||page_invert[write_page]!=(u8)screen_invert) {
        dosferCopyFull320(screen_320,vram+base);
        page_status_generation[write_page]=status_generation;
    } else {
        dosferCopyQr320(screen_320,vram+base);
        if(page_status_generation[write_page]!=status_generation) {
            dosferCopyStatus320(screen_320+(u32)VGA_STATUS_ROW*VGA_BYTES_PER_LINE,
                vram+base+(u32)VGA_STATUS_ROW*VGA_BYTES_PER_LINE);
            page_status_generation[write_page]=status_generation;
        }
    }
    page_initialized[write_page]=1;
    page_invert[write_page]=(u8)screen_invert;
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[1]+=profile_now-profile_start;
#endif
    finish_page_flip(earliest_tick);
}

static void copy_rgb3_flip(u32 earliest_tick) {
    static const u8 plane_mask[VGA_RGB_CHANNELS]={0x04,0x02,0x01};
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);
    u16 base=(u16)(write_page<<13);
    int channel;
    int reset=!page_initialized[write_page]||
              page_invert[write_page]!=(u8)screen_invert;
    int status_changed=page_status_generation[write_page]!=status_generation;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks(),profile_now;
#endif

    if(reset) {
        set_map_mask(0x08); /* fourth plane must never leak into RGB indexes */
        dosferClearFull320(vram+base);
    }
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
        u8 *screen=rgb_screen(channel);
        set_map_mask(plane_mask[channel]);
        /* The first RGB image on each VGA page used to take the full-copy
           path. On real hardware/DOSBox that path can leave two colour
           planes stale. Initialise the page explicitly, then use the exact
           QR/status copy routine that all later RGB images use. */
        if(reset)_fmemset(vram+base,screen_invert?0x00:0xFF,VGA_VISIBLE_BYTES);
        dosferCopyQr320(screen,vram+base);
        if(reset||status_changed)
            dosferCopyStatus320(screen+(u32)VGA_STATUS_ROW*VGA_BYTES_PER_LINE,
                vram+base+(u32)VGA_STATUS_ROW*VGA_BYTES_PER_LINE);
    }
    page_initialized[write_page]=1;
    page_invert[write_page]=(u8)screen_invert;
    page_status_generation[write_page]=status_generation;
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[1]+=profile_now-profile_start;
#endif
    finish_page_flip(earliest_tick);
}

static int prepare_delta(void) {
    u16 *modules;
    int bits=qrcodegen_dosferPlacementBits();
    int i,x,y,px;
    u16 linear;

    if(bits!=QR_DATA_BITS)return 0;

    /* Take ownership of the fixed V40 placement table and convert it in
     * place.  This avoids allocating a second 59,296-byte map while the
     * canonical matrix cache is still alive. */
    modules=qrcodegen_dosferTakePlacementModules();
    if(!modules)return 0;

    free_delta();
    delta_entry=modules;
    for(i=0;i<bits;++i){
        linear=delta_entry[i];
        y=linear/DOSFER_QR_SIZE;
        x=linear-y*DOSFER_QR_SIZE;
        px=QR_X0+DOSFER_QR_QUIET+x;
        delta_entry[i]=(u16)(((QR_Y0+y)*VGA_BYTES_PER_LINE+(px>>3))|
            ((u16)(px&7)<<13));
    }
#ifdef DOSFER_DEVTOOLS
    delta_bits=(u16)bits;
#endif
    qrcodegen_dosferReleaseMatrixCache();
    return 1;
}

/* Toggle one changed codeword through the sequential packed placement map. */
#define APPLY_CHANGED_BYTE(changed,map) do { \
    u16 entry__; \
    if((changed)&0x80){entry__=(map)[0];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
    if((changed)&0x40){entry__=(map)[1];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
    if((changed)&0x20){entry__=(map)[2];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
    if((changed)&0x10){entry__=(map)[3];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
    if((changed)&0x08){entry__=(map)[4];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
    if((changed)&0x04){entry__=(map)[5];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
    if((changed)&0x02){entry__=(map)[6];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
    if((changed)&0x01){entry__=(map)[7];screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13];} \
} while(0)

int vga_apply_codeword_delta(const u8 *next_codewords,u8 *current_codewords) {
    const u16 far *map=delta_entry;
    const u8 far *next=next_codewords;
    u8 far *current=current_codewords;
    u16 i;
    u8 changed;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!delta_entry||!next_codewords||!current_codewords)return 0;
    for(i=0;i<DOSFER_QR_CODEWORDS;++i,map+=8) {
        changed=(u8)(*next^*current);
        *current++=*next++;
        if(changed)APPLY_CHANGED_BYTE(changed,map);
    }
#ifdef DOSFER_PROFILE
    dosferVgaProfileTicks[0]+=timer_ticks()-profile_start;
#endif
    return 1;
}

/* Emit V40-L codewords in final interleaved order while updating the caller's
 * single persistent current-codeword stream and the RAM shadow raster.  The
 * 2956 data bytes and 750 block-major ECC bytes are never materialized as a
 * second 3706-byte output array. */
int vga_apply_v40l_delta(const u8 *data_codewords,const u8 *ecc_blocks,
                         u8 *current_codewords) {
    static const u16 block_offset[25]={
        0,118,236,354,472,590,708,826,944,1062,
        1180,1298,1416,1534,1652,1770,1888,2006,2124,
        2242,2361,2480,2599,2718,2837
    };
    const u16 far *map=delta_entry;
    u8 far *current=current_codewords;
    u16 cw=0;
    int row,block;
    u8 value,changed;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!delta_entry||!data_codewords||!ecc_blocks||!current_codewords)return 0;

#define EMIT_VALUE(v) do { \
        value=(u8)(v); \
        changed=(u8)(value^*current); \
        *current++=value; \
        if(changed)APPLY_CHANGED_BYTE(changed,map); \
        ++cw;map+=8; \
    } while(0)

    /* First 118 data columns contain one byte from every block. */
    for(row=0;row<118;++row)
        for(block=0;block<25;++block)
            EMIT_VALUE(data_codewords[block_offset[block]+row]);

    /* Six long blocks contribute one extra data byte. */
    for(block=19;block<25;++block)
        EMIT_VALUE(data_codewords[block_offset[block]+118]);

    /* Thirty ECC columns contain one byte from every block. */
    for(row=0;row<30;++row)
        for(block=0;block<25;++block)
            EMIT_VALUE(ecc_blocks[block*30+row]);

#undef EMIT_VALUE
#ifdef DOSFER_PROFILE
    dosferVgaProfileTicks[0]+=timer_ticks()-profile_start;
#endif
    return cw==DOSFER_QR_CODEWORDS;
}


#define APPLY_RGB_MODULE(bit_index,bit_mask) do { \
    if((changed_r|changed_g|changed_b)&(bit_mask)) { \
        u16 entry__=(map)[bit_index]; \
        u16 offset__=(u16)(entry__&0x1FFF); \
        u8 pixel__=pixel_mask[entry__>>13]; \
        if(changed_r&(bit_mask))screen_320[offset__]^=pixel__; \
        if(changed_g&(bit_mask))screen_green[offset__]^=pixel__; \
        if(changed_b&(bit_mask))screen_blue[offset__]^=pixel__; \
    } \
} while(0)

#define APPLY_CHANGED_BYTE3(map) do { \
    APPLY_RGB_MODULE(0,0x80); APPLY_RGB_MODULE(1,0x40); \
    APPLY_RGB_MODULE(2,0x20); APPLY_RGB_MODULE(3,0x10); \
    APPLY_RGB_MODULE(4,0x08); APPLY_RGB_MODULE(5,0x04); \
    APPLY_RGB_MODULE(6,0x02); APPLY_RGB_MODULE(7,0x01); \
} while(0)

int vga_apply_codeword_delta3(
        const u8 *const next_codewords[VGA_RGB_CHANNELS],
        u8 *const current_codewords[VGA_RGB_CHANNELS]) {
    const u16 far *map=delta_entry;
    const u8 far *next_r,*next_g,*next_b;
    u8 far *current_r,*current_g,*current_b;
    u16 i;
    u8 changed_r,changed_g,changed_b;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!rgb3_active||!delta_entry||!next_codewords||!current_codewords||
       !next_codewords[VGA_RGB_RED]||!next_codewords[VGA_RGB_GREEN]||
       !next_codewords[VGA_RGB_BLUE]||!current_codewords[VGA_RGB_RED]||
       !current_codewords[VGA_RGB_GREEN]||!current_codewords[VGA_RGB_BLUE])return 0;
    next_r=next_codewords[VGA_RGB_RED];
    next_g=next_codewords[VGA_RGB_GREEN];
    next_b=next_codewords[VGA_RGB_BLUE];
    current_r=current_codewords[VGA_RGB_RED];
    current_g=current_codewords[VGA_RGB_GREEN];
    current_b=current_codewords[VGA_RGB_BLUE];
    for(i=0;i<DOSFER_QR_CODEWORDS;++i,map+=8) {
        changed_r=(u8)(*next_r^*current_r);
        changed_g=(u8)(*next_g^*current_g);
        changed_b=(u8)(*next_b^*current_b);
        *current_r++=*next_r++;
        *current_g++=*next_g++;
        *current_b++=*next_b++;
        if(changed_r|changed_g|changed_b)APPLY_CHANGED_BYTE3(map);
    }
#ifdef DOSFER_PROFILE
    dosferVgaProfileTicks[0]+=timer_ticks()-profile_start;
#endif
    return 1;
}

int vga_apply_v40l_delta3(
        const u8 *const data_codewords[VGA_RGB_CHANNELS],
        const u8 *const ecc_blocks[VGA_RGB_CHANNELS],
        u8 *const current_codewords[VGA_RGB_CHANNELS]) {
    static const u16 block_offset[25]={
        0,118,236,354,472,590,708,826,944,1062,
        1180,1298,1416,1534,1652,1770,1888,2006,2124,
        2242,2361,2480,2599,2718,2837
    };
    const u16 far *map=delta_entry;
    u8 far *current_r,*current_g,*current_b;
    u16 cw=0;
    int row,block;
    u8 value_r,value_g,value_b,changed_r,changed_g,changed_b;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!rgb3_active||!delta_entry||!data_codewords||!ecc_blocks||
       !current_codewords)return 0;
    for(block=0;block<VGA_RGB_CHANNELS;++block)
        if(!data_codewords[block]||!ecc_blocks[block]||
           !current_codewords[block])return 0;
    current_r=current_codewords[VGA_RGB_RED];
    current_g=current_codewords[VGA_RGB_GREEN];
    current_b=current_codewords[VGA_RGB_BLUE];

#define EMIT_VALUE3(vr,vg,vb) do { \
        value_r=(u8)(vr); value_g=(u8)(vg); value_b=(u8)(vb); \
        changed_r=(u8)(value_r^*current_r); \
        changed_g=(u8)(value_g^*current_g); \
        changed_b=(u8)(value_b^*current_b); \
        *current_r++=value_r; *current_g++=value_g; *current_b++=value_b; \
        if(changed_r|changed_g|changed_b)APPLY_CHANGED_BYTE3(map); \
        ++cw; map+=8; \
    } while(0)

    for(row=0;row<118;++row)
        for(block=0;block<25;++block)
            EMIT_VALUE3(
                data_codewords[VGA_RGB_RED][block_offset[block]+row],
                data_codewords[VGA_RGB_GREEN][block_offset[block]+row],
                data_codewords[VGA_RGB_BLUE][block_offset[block]+row]);
    for(block=19;block<25;++block)
        EMIT_VALUE3(
            data_codewords[VGA_RGB_RED][block_offset[block]+118],
            data_codewords[VGA_RGB_GREEN][block_offset[block]+118],
            data_codewords[VGA_RGB_BLUE][block_offset[block]+118]);
    for(row=0;row<30;++row)
        for(block=0;block<25;++block)
            EMIT_VALUE3(
                ecc_blocks[VGA_RGB_RED][block*30+row],
                ecc_blocks[VGA_RGB_GREEN][block*30+row],
                ecc_blocks[VGA_RGB_BLUE][block*30+row]);

#undef EMIT_VALUE3
#ifdef DOSFER_PROFILE
    dosferVgaProfileTicks[0]+=timer_ticks()-profile_start;
#endif
    return cw==DOSFER_QR_CODEWORDS;
}

#undef APPLY_CHANGED_BYTE3
#undef APPLY_RGB_MODULE
#undef APPLY_CHANGED_BYTE

int vga_delta_ready(void) {
    return delta_entry!=0;
}

int vga_rgb3_active(void) {
    return rgb3_active;
}

#ifdef DOSFER_DEVTOOLS
void vga_delta_stats(u16 *bits) {
    if(bits)*bits=delta_bits;
}

u32 vga_screen_hash(void) {
    u32 i,h=2166136261UL;
    int channel,channels=rgb3_active?VGA_RGB_CHANNELS:1;
    /* Ignore status rows so full redraw and streaming hashes are comparable. */
    for(channel=0;channel<channels;++channel) {
        u8 *screen=rgb_screen(channel);
        for(i=0;i<7680;++i){h^=screen[i];h*=16777619UL;}
    }
    return h;
}

int vga_display_matches(void) {
    static const u8 read_plane[VGA_RGB_CHANNELS]={2,1,0};
    u8 far *vram=(u8 far *)MK_FP(0xA000,(u16)(display_page<<13));
    u16 i;
    int channel;
    if(!rgb3_active) {
        outp(0x3CE,4);outp(0x3CF,0);
        for(i=0;i<VGA_VISIBLE_BYTES;++i)
            if(vram[i]!=screen_320[i])return 0;
        return 1;
    }
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
        u8 *screen=rgb_screen(channel);
        outp(0x3CE,4);outp(0x3CF,read_plane[channel]);
        for(i=0;i<VGA_VISIBLE_BYTES;++i)
            if(vram[i]!=screen[i])return 0;
    }
    outp(0x3CE,4);outp(0x3CF,3);
    for(i=0;i<VGA_VISIBLE_BYTES;++i)if(vram[i])return 0;
    return 1;
}
#endif

int vga_show_full_qr_at(const u8 *qr,const u8 *codewords,
                        int invert,const char *status,u32 earliest_tick) {
#ifdef DOSFER_PROFILE
    u32 profile_start,profile_now;
#endif
    if(!vga_active||!qr||!codewords)return 0;
#ifdef DOSFER_PROFILE
    profile_start=timer_ticks();
#endif
    if(!build_qr_image_320(qr,invert))return 0;
    /* Delta acceleration is optional. A low-memory machine must still be
     * able to transmit using canonical full redraws. */
    prepare_delta();
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[0]+=profile_now-profile_start;
    profile_start=profile_now;
#endif
    status_text_320(status);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[4]+=profile_now-profile_start;
#endif
    copy_320_flip(earliest_tick);
    return 1;
}

int vga_show_prepared_at(int invert,const char *status,u32 earliest_tick) {
#ifdef DOSFER_PROFILE
    u32 profile_start,profile_now;
#endif
    if(!vga_active||!delta_entry||screen_invert!=invert)return 0;
#ifdef DOSFER_PROFILE
    profile_start=timer_ticks();
#endif
    status_text_320(status);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[4]+=profile_now-profile_start;
#endif
    copy_320_flip(earliest_tick);
    return 1;
}

int vga_show_full_qr3_at(const u8 *const qr[VGA_RGB_CHANNELS],
                         u8 *const codewords[VGA_RGB_CHANNELS],
                         int invert,const char *status,u32 earliest_tick) {
#ifdef DOSFER_PROFILE
    u32 profile_start,profile_now;
#endif
    int channel;
    if(!vga_active||!rgb3_active||!qr||!codewords)return 0;
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
        if(!qr[channel]||!codewords[channel])return 0;
#ifdef DOSFER_PROFILE
    profile_start=timer_ticks();
#endif
    if(!build_qr_image_rgb3(qr,invert))return 0;
    prepare_delta();
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[0]+=profile_now-profile_start;
    profile_start=profile_now;
#endif
    status_text_rgb3(status);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[4]+=profile_now-profile_start;
#endif
    copy_rgb3_flip(earliest_tick);
    return 1;
}

int vga_show_prepared3_at(int invert,const char *status,u32 earliest_tick) {
#ifdef DOSFER_PROFILE
    u32 profile_start,profile_now;
#endif
    if(!vga_active||!rgb3_active||!delta_entry||screen_invert!=invert)return 0;
#ifdef DOSFER_PROFILE
    profile_start=timer_ticks();
#endif
    status_text_rgb3(status);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[4]+=profile_now-profile_start;
#endif
    copy_rgb3_flip(earliest_tick);
    return 1;
}

#ifdef DOSFER_DEVTOOLS
void vga_benchmark_qr(const u8 *qr,int loops,u32 *build_ms,u32 *copy_ms,u32 *text_ms) {
    int i;
    u32 a,b;
    a=timer_ticks();for(i=0;i<loops;++i)build_qr_image_320(qr,0);b=timer_ticks();
    *build_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)copy_320_flip(0);b=timer_ticks();
    *copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)status_text_320("DOSfer V40 320x200 benchmark");b=timer_ticks();
    *text_ms=timer_elapsed_ms(a,b);
}

void vga_benchmark_delta(const u8 *codewords,int loops,
                         u32 *update_ms,u32 *copy_ms,u32 *text_ms) {
    u8 far *alternate=(u8 far *)_fmalloc(DOSFER_QR_CODEWORDS);
    u8 far *current=(u8 far *)_fmalloc(DOSFER_QR_CODEWORDS);
    int i;
    u32 a,b,state=0x6A67C69DUL;

    if(!alternate||!current||!delta_entry){
        *update_ms=*copy_ms=*text_ms=0;
        if(alternate)_ffree(alternate);
        if(current)_ffree(current);
        return;
    }
    _fmemcpy(current,codewords,DOSFER_QR_CODEWORDS);
    for(i=0;i<DOSFER_QR_CODEWORDS;++i){
        state^=state<<13;state^=state>>17;state^=state<<5;
        alternate[i]=(u8)(codewords[i]^(u8)state);
    }
    a=timer_ticks();
    for(i=0;i<loops;++i)
        vga_apply_codeword_delta((i&1)?codewords:alternate,current);
    b=timer_ticks();
    *update_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)copy_320_flip(0);b=timer_ticks();
    *copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)status_text_320("DOSfer V40 delta benchmark");b=timer_ticks();
    *text_ms=timer_elapsed_ms(a,b);
    _ffree(alternate);
    _ffree(current);
}

#endif

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
