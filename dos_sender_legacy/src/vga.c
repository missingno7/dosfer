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
static u16 far *placement_entry;
/* Fixed V40-L function-module raster shared by the BW and RGB3 direct
 * renderers. All three RGB planes have the same function patterns. */
static u8 far *direct_template;
static const u8 far *direct_asm_red,*direct_asm_green,*direct_asm_blue;
static const u8 far *direct_asm_bw;
static const u16 far *direct_asm_map;
#define VGA_STRIPE_MIN_RUN 4
#define VGA_STRIPE_MAX_RUNS 256
typedef struct {
    u16 first,count,dest;
    signed char direction;
    u8 pair_mask[4];
    u8 group_flags;
} VgaStripeRun;
static VgaStripeRun far *stripe_runs;
static u16 stripe_run_count,stripe_codewords_covered;
static u16 stripe_asm_step;
static const u8 far *stripe_asm_pair_mask;
#define VGA_STRIPE_MAX_GROUPS 8
#define VGA_STRIPE_GROUP_EXACT 1
#define VGA_STRIPE_GROUP_PHASE 2
#define VGA_STRIPE_GROUP_TRIPLE 4
typedef struct {
    u16 first[4];
    u16 count,dest;
} VgaStripeGroup;
static VgaStripeGroup stripe_groups[VGA_STRIPE_MAX_GROUPS];
static u16 stripe_group_count,stripe_group_codewords;
static u8 stripe_pair_swap[256],stripe_bit_reverse[256];
static const u8 far *stripe_group_source;
static u16 stripe_group_src0,stripe_group_src1,stripe_group_src2,
           stripe_group_src3,stripe_group_dest,stripe_group_count_asm;
static u8 far *stripe_group_lut_raw;
static u32 far *stripe_group_lut;
#define VGA_STRIPE_MAX_PHASE_GROUPS 8
#define VGA_STRIPE_MAX_EDGES 64
typedef struct {
    u16 first[4];
    u16 count,dest;
    u8 phase;
} VgaStripePhaseGroup;
typedef struct {
    u16 codeword,dest;
    u8 shift;
    u8 pair_mask[4];
    u8 pad;
} VgaStripeEdge;
static VgaStripePhaseGroup stripe_phase_groups[VGA_STRIPE_MAX_PHASE_GROUPS];
static VgaStripeEdge stripe_phase_edges[VGA_STRIPE_MAX_EDGES];
static u16 stripe_phase_group_count,stripe_phase_codewords,
           stripe_phase_destination_bytes,stripe_phase_edge_count;
static u8 stripe_phase_type;
static u16 stripe_edge_source_offset;
#define VGA_STRIPE_MAX_TRIPLES 4
typedef struct {
    u16 first[3];
    u16 count,dest;
} VgaStripeTriple;
static VgaStripeTriple stripe_triples[VGA_STRIPE_MAX_TRIPLES];
static u16 stripe_triple_count,stripe_triple_codewords;
static const u16 far *direct_range_map;
static u16 direct_range_count,direct_range_source_offset;
#ifdef DOSFER_DEVTOOLS
static u16 delta_bits;
#endif
#ifdef DOSFER_MAP_STATS
static u32 rgb_map_loads,rgb_red_xors,rgb_green_xors,rgb_blue_xors;
#define RGB_STAT_MAP() (++rgb_map_loads)
#define RGB_STAT_RED() (++rgb_red_xors)
#define RGB_STAT_GREEN() (++rgb_green_xors)
#define RGB_STAT_BLUE() (++rgb_blue_xors)
#else
#define RGB_STAT_MAP() ((void)0)
#define RGB_STAT_RED() ((void)0)
#define RGB_STAT_GREEN() ((void)0)
#define RGB_STAT_BLUE() ((void)0)
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

static void free_stream_renderer(void) {
    if(placement_entry)free(placement_entry);
    if(direct_template)_ffree(direct_template);
    if(stripe_runs)_ffree(stripe_runs);
    if(stripe_group_lut_raw)_ffree(stripe_group_lut_raw);
    stripe_group_lut_raw=0;
    stripe_group_lut=0;
    placement_entry=0;
    direct_template=0;
    stripe_runs=0;
    stripe_run_count=stripe_codewords_covered=0;
    stripe_group_count=stripe_group_codewords=0;
    stripe_phase_group_count=stripe_phase_codewords=0;
    stripe_phase_destination_bytes=stripe_phase_edge_count=0;
    stripe_triple_count=stripe_triple_codewords=0;
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
    free_stream_renderer();
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
    free_stream_renderer();
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

/* Recognize uninterrupted same-byte two-column stripes directly from the
 * production framebuffer map. A run descriptor replaces all 8*count module
 * entries in the stripe kernel; crossings remain on the exact
 * placement-map fallback. */
static int stripe_codeword_shape(u16 codeword,signed char *direction,
        u16 *dest,u8 *right_mask,u8 *left_mask) {
    const u16 far *entry=placement_entry+(u32)codeword*8UL;
    u16 first=(u16)(entry[0]&0x1FFF),row;
    int step;

    if((entry[1]&0x1FFF)!=first)return 0;
    step=(int)(entry[2]&0x1FFF)-(int)first;
    if(step!=VGA_BYTES_PER_LINE&&step!=-VGA_BYTES_PER_LINE)return 0;
    *direction=(signed char)(step>0?1:-1);
    *right_mask=pixel_mask[entry[0]>>13];
    *left_mask=pixel_mask[entry[1]>>13];
    for(row=0;row<4;++row) {
        u16 expected=(u16)(first+(int)row*step);
        if((entry[row*2]&0x1FFF)!=expected||
           (entry[row*2+1]&0x1FFF)!=expected||
           pixel_mask[entry[row*2]>>13]!=*right_mask||
           pixel_mask[entry[row*2+1]>>13]!=*left_mask)return 0;
    }
    *dest=first;
    return 1;
}

static void prepare_stripe_runs(void) {
    u16 first=0,count,next_dest,dest;
    u8 right_mask,left_mask,next_right,next_left;
    signed char direction,next_direction;

    stripe_runs=(VgaStripeRun far *)_fmalloc(
        VGA_STRIPE_MAX_RUNS*sizeof(VgaStripeRun));
    if(!stripe_runs)return;
    while(first<DOSFER_QR_CODEWORDS) {
        if(!stripe_codeword_shape(first,&direction,&dest,
                &right_mask,&left_mask)) {++first;continue;}
        count=1;
        while(first+count<DOSFER_QR_CODEWORDS&&
              stripe_codeword_shape((u16)(first+count),&next_direction,
                  &next_dest,&next_right,&next_left)&&
              next_direction==direction&&next_right==right_mask&&
              next_left==left_mask&&
              next_dest==(u16)(dest+(int)count*(int)direction*160))++count;
        if(count>=VGA_STRIPE_MIN_RUN) {
            VgaStripeRun far *run;
            if(stripe_run_count>=VGA_STRIPE_MAX_RUNS) {
                _ffree(stripe_runs);stripe_runs=0;
                stripe_run_count=stripe_codewords_covered=0;return;
            }
            run=&stripe_runs[stripe_run_count++];
            run->first=first;run->count=count;run->dest=dest;
            run->direction=direction;
            run->pair_mask[0]=0;
            run->pair_mask[1]=left_mask;
            run->pair_mask[2]=right_mask;
            run->pair_mask[3]=(u8)(right_mask|left_mask);
            run->group_flags=0;
            stripe_codewords_covered=(u16)(stripe_codewords_covered+count);
        }
        first=(u16)(first+count);
    }
}

static int stripe_group_slot(u8 mask) {
    if(mask==0x03)return 0;
    if(mask==0x0C)return 1;
    if(mask==0x30)return 2;
    if(mask==0xC0)return 3;
    return -1;
}

static u16 stripe_run_top(const VgaStripeRun far *run) {
    if(run->direction>0)return run->dest;
    return (u16)(run->dest-(u16)(run->count*4u-1u)*VGA_BYTES_PER_LINE);
}

/* Find complete framebuffer-byte regions formed by the four regular
 * two-column masks. The alternating directions are the fixed V40 zigzag:
 * 03/30 run upward in placement order, 0C/C0 downward. */
static void prepare_stripe_groups(void) {
    u16 i,j,top;
    int slot;
    u8 value;

    stripe_group_count=stripe_group_codewords=0;
    for(i=0;i<stripe_run_count;++i)stripe_runs[i].group_flags=0;
    for(i=0;i<256;++i) {
        stripe_pair_swap[i]=(u8)(((i&0xAAu)>>1)|((i&0x55u)<<1));
        value=(u8)i;
        value=(u8)((value>>4)|(value<<4));
        value=(u8)(((value&0xCCu)>>2)|((value&0x33u)<<2));
        stripe_bit_reverse[i]=(u8)(((value&0xAAu)>>1)|((value&0x55u)<<1));
    }
    for(i=0;i<stripe_run_count;++i) {
        VgaStripeRun far *base=&stripe_runs[i];
        int found[4]={-1,-1,-1,-1};
        if((base->group_flags&VGA_STRIPE_GROUP_EXACT)||
           stripe_group_slot(base->pair_mask[3])!=0)
            continue;
        top=stripe_run_top(base);
        for(j=i;j<stripe_run_count;++j) {
            VgaStripeRun far *candidate=&stripe_runs[j];
            if((candidate->group_flags&VGA_STRIPE_GROUP_EXACT)||
               candidate->count!=base->count||
               stripe_run_top(candidate)!=top)continue;
            slot=stripe_group_slot(candidate->pair_mask[3]);
            if(slot>=0&&found[slot]<0)found[slot]=j;
        }
        if(found[0]>=0&&found[1]>=0&&found[2]>=0&&found[3]>=0&&
           stripe_runs[found[0]].direction<0&&
           stripe_runs[found[1]].direction>0&&
           stripe_runs[found[2]].direction<0&&
           stripe_runs[found[3]].direction>0) {
            VgaStripeGroup *group;
            if(stripe_group_count>=VGA_STRIPE_MAX_GROUPS)break;
            group=&stripe_groups[stripe_group_count++];
            for(slot=0;slot<4;++slot) {
                group->first[slot]=stripe_runs[found[slot]].first;
                stripe_runs[found[slot]].group_flags|=VGA_STRIPE_GROUP_EXACT;
            }
            group->count=base->count;group->dest=top;
            stripe_group_codewords=(u16)(stripe_group_codewords+
                base->count*4u);
        }
    }
}

/* Each exact group combines four two-bit lanes into four framebuffer bytes.
 * Precompute one independent 32-bit contribution per raw lane byte; XORing
 * the four entries is exactly the existing normalize-and-transpose kernel. */
static u32 stripe_group_normalized_contribution(u16 lane,u8 normalized) {
    u32 value,swap;
    value=(u32)normalized<<(lane*8u);
    swap=((value>>6)^value)&0x00CC00CCUL;
    value^=swap^(swap<<6);
    swap=((value>>12)^value)&0x0000F0F0UL;
    return value^swap^(swap<<12);
}

static u32 stripe_group_lane_contribution(u16 lane,u8 raw) {
    u8 normalized=(lane&1u)?stripe_pair_swap[raw]:stripe_bit_reverse[raw];
    return stripe_group_normalized_contribution(lane,normalized);
}

static void prepare_stripe_group_lut(void) {
    u16 lane,value;
    u32 paragraphs;
    stripe_group_lut_raw=(u8 far *)_fmalloc(4096u+15u);
    if(!stripe_group_lut_raw)return;
    paragraphs=((u32)FP_OFF(stripe_group_lut_raw)+15UL)>>4;
    stripe_group_lut=(u32 far *)MK_FP(
        (u16)(FP_SEG(stripe_group_lut_raw)+paragraphs),0);
    for(lane=0;lane<4;++lane)
        for(value=0;value<256;++value)
            stripe_group_lut[(u16)(lane*256u+value)]=
                stripe_group_lane_contribution(lane,(u8)value);
}

static u16 stripe_run_bottom(const VgaStripeRun far *run) {
    return (u16)(stripe_run_top(run)+
        (u16)(run->count*4u-1u)*VGA_BYTES_PER_LINE);
}

/* Find the adjacent complete-byte regions whose four mask lanes have the
 * same 164-row overlap but are displaced by two rows.  Keeping this derived
 * from the placement stream makes the experiment self-checking and avoids
 * embedding V40 codeword indices in the renderer. */
static void prepare_stripe_phase_groups(void) {
    u16 i,j,k,top0,bottom0,top1,bottom1,overlap_top,overlap_bottom;
    int slot;

    stripe_phase_group_count=stripe_phase_codewords=0;
    stripe_phase_destination_bytes=stripe_phase_edge_count=0;
    for(i=0;i<stripe_run_count;++i)
        stripe_runs[i].group_flags&=(u8)~VGA_STRIPE_GROUP_PHASE;
    if(sizeof(VgaStripeEdge)!=10)return; /* ASM recipe stride contract. */
    for(i=0;i<stripe_run_count;++i) {
        VgaStripeRun far *run0=&stripe_runs[i];
        int reverse[2]={-1,-1},forward[2]={-1,-1};
        VgaStripePhaseGroup *group;
        u8 phase;

        if(run0->group_flags||
           stripe_group_slot(run0->pair_mask[3])!=0||
           run0->direction>=0)continue;
        reverse[0]=i;
        top0=stripe_run_top(run0);bottom0=stripe_run_bottom(run0);
        for(j=0;j<stripe_run_count;++j) {
            VgaStripeRun far *candidate=&stripe_runs[j];
            if(candidate->group_flags)continue;
            slot=stripe_group_slot(candidate->pair_mask[3]);
            if(slot==2&&candidate->direction<0&&
               candidate->count==run0->count&&
               stripe_run_top(candidate)==top0&&
               stripe_run_bottom(candidate)==bottom0) {
                reverse[1]=j;break;
            }
        }
        if(reverse[1]<0)continue;
        for(j=0;j<stripe_run_count;++j) {
            VgaStripeRun far *candidate=&stripe_runs[j];
            if(candidate->group_flags||
               candidate->direction<=0||
               stripe_group_slot(candidate->pair_mask[3])!=1)continue;
            top1=stripe_run_top(candidate);bottom1=stripe_run_bottom(candidate);
            if(!((top0+2u*VGA_BYTES_PER_LINE==top1&&
                  bottom0==bottom1+2u*VGA_BYTES_PER_LINE)||
                 (top1+2u*VGA_BYTES_PER_LINE==top0&&
                  bottom1==bottom0+2u*VGA_BYTES_PER_LINE)))continue;
            forward[0]=j;
            for(k=0;k<stripe_run_count;++k) {
                VgaStripeRun far *mate=&stripe_runs[k];
                if(mate->group_flags||
                   mate->direction<=0||
                   stripe_group_slot(mate->pair_mask[3])!=3)continue;
                if(mate->count==candidate->count&&
                   stripe_run_top(mate)==top1&&
                   stripe_run_bottom(mate)==bottom1) {
                    forward[1]=k;break;
                }
            }
            if(forward[1]>=0)break;
            forward[0]=-1;
        }
        if(forward[0]<0||forward[1]<0)continue;
        overlap_top=top0>top1?top0:top1;
        overlap_bottom=bottom0<bottom1?bottom0:bottom1;
        if(overlap_bottom<overlap_top||
           (u16)((overlap_bottom-overlap_top)/VGA_BYTES_PER_LINE+1u)!=164u)
            continue;
        phase=(u8)(top0<top1?1:2);
        if(stripe_phase_group_count>=VGA_STRIPE_MAX_PHASE_GROUPS)break;
        group=&stripe_phase_groups[stripe_phase_group_count++];
        group->first[0]=stripe_runs[reverse[0]].first;
        group->first[1]=stripe_runs[forward[0]].first;
        group->first[2]=stripe_runs[reverse[1]].first;
        group->first[3]=stripe_runs[forward[1]].first;
        group->count=41;group->dest=overlap_top;group->phase=phase;
        stripe_runs[reverse[0]].group_flags|=VGA_STRIPE_GROUP_PHASE;
        stripe_runs[reverse[1]].group_flags|=VGA_STRIPE_GROUP_PHASE;
        stripe_runs[forward[0]].group_flags|=VGA_STRIPE_GROUP_PHASE;
        stripe_runs[forward[1]].group_flags|=VGA_STRIPE_GROUP_PHASE;
        stripe_phase_codewords=(u16)(stripe_phase_codewords+
            stripe_runs[reverse[0]].count+stripe_runs[reverse[1]].count+
            stripe_runs[forward[0]].count+stripe_runs[forward[1]].count);
        stripe_phase_destination_bytes=(u16)(
            stripe_phase_destination_bytes+164u);

        /* Record only the rows outside the common interval. They are exact
         * original run contributions and are applied after the phase kernel. */
        for(slot=0;slot<4;++slot) {
            u16 run_index=(u16)(slot==0?reverse[0]:slot==1?forward[0]:
                                slot==2?reverse[1]:forward[1]);
            VgaStripeRun far *run=&stripe_runs[run_index];
            u16 codeword,pair,dest;
            for(codeword=0;codeword<run->count;++codeword)
                for(pair=0;pair<4;++pair) {
                    dest=(u16)(run->dest+(int)run->direction*
                        (int)(codeword*4u+pair)*VGA_BYTES_PER_LINE);
                    if(dest>=overlap_top&&dest<=overlap_bottom)continue;
                    if(stripe_phase_edge_count>=VGA_STRIPE_MAX_EDGES) {
                        stripe_phase_group_count=stripe_phase_codewords=0;
                        stripe_phase_destination_bytes=stripe_phase_edge_count=0;
                        for(k=0;k<stripe_run_count;++k)
                            stripe_runs[k].group_flags&=
                                (u8)~VGA_STRIPE_GROUP_PHASE;
                        return;
                    }
                    stripe_phase_edges[stripe_phase_edge_count].codeword=
                        (u16)(run->first+codeword);
                    stripe_phase_edges[stripe_phase_edge_count].dest=dest;
                    stripe_phase_edges[stripe_phase_edge_count].shift=
                        (u8)(6u-pair*2u);
                    memcpy(stripe_phase_edges[stripe_phase_edge_count].pair_mask,
                        run->pair_mask,4);
                    stripe_phase_edges[stripe_phase_edge_count].pad=0;
                    ++stripe_phase_edge_count;
                }
        }
    }
}

/* Three long lanes (0C/30/C0) remain after the complete and phase-shifted
 * four-lane groups.  They share one exact 168-row destination interval; the
 * interrupted 03 lane can continue through the normal run path. */
static void prepare_stripe_triples(void) {
    u16 i,j,top;
    int slot;
    stripe_triple_count=stripe_triple_codewords=0;
    for(i=0;i<stripe_run_count;++i)
        stripe_runs[i].group_flags&=(u8)~VGA_STRIPE_GROUP_TRIPLE;
    for(i=0;i<stripe_run_count;++i) {
        VgaStripeRun far *base=&stripe_runs[i];
        int found[3]={-1,-1,-1};
        if(base->group_flags||stripe_group_slot(base->pair_mask[3])!=1||
           base->direction<=0)continue;
        found[0]=i;top=stripe_run_top(base);
        for(j=0;j<stripe_run_count;++j) {
            VgaStripeRun far *candidate=&stripe_runs[j];
            if(candidate->group_flags||candidate->count!=base->count||
               stripe_run_top(candidate)!=top)continue;
            slot=stripe_group_slot(candidate->pair_mask[3]);
            if(slot==2&&candidate->direction<0)found[1]=j;
            if(slot==3&&candidate->direction>0)found[2]=j;
        }
        if(found[1]>=0&&found[2]>=0) {
            VgaStripeTriple *group;
            if(stripe_triple_count>=VGA_STRIPE_MAX_TRIPLES)break;
            group=&stripe_triples[stripe_triple_count++];
            for(slot=0;slot<3;++slot) {
                group->first[slot]=stripe_runs[found[slot]].first;
                stripe_runs[found[slot]].group_flags|=
                    VGA_STRIPE_GROUP_TRIPLE;
            }
            group->count=base->count;group->dest=top;
            stripe_triple_codewords=(u16)(stripe_triple_codewords+
                base->count*3u);
        }
    }
}


static int prepare_placement_map(void) {
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

    free_stream_renderer();
    placement_entry=modules;
    for(i=0;i<bits;++i){
        linear=placement_entry[i];
        y=linear/DOSFER_QR_SIZE;
        x=linear-y*DOSFER_QR_SIZE;
        px=QR_X0+DOSFER_QR_QUIET+x;
        placement_entry[i]=(u16)(((QR_Y0+y)*VGA_BYTES_PER_LINE+(px>>3))|
            ((u16)(px&7)<<13));
    }
    prepare_stripe_runs();
    prepare_stripe_groups();
    prepare_stripe_group_lut();
    prepare_stripe_phase_groups();
    prepare_stripe_triples();
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
    const u16 far *map=placement_entry;
    const u8 far *next=next_codewords;
    u8 far *current=current_codewords;
    u16 i;
    u8 changed;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!placement_entry||!next_codewords||!current_codewords)return 0;
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
    const u16 far *map=placement_entry;
    u8 far *current=current_codewords;
    u16 cw=0;
    int row,block;
    u8 value,changed;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!placement_entry||!data_codewords||!ecc_blocks||!current_codewords)return 0;

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
    if(changed_any&(bit_mask)) { \
        u16 entry__=(map)[bit_index]; \
        u16 offset__=(u16)(entry__&0x1FFF); \
        u8 pixel__=pixel_mask[entry__>>13]; \
        RGB_STAT_MAP(); \
        if(changed_r&(bit_mask)){screen_320[offset__]^=pixel__;RGB_STAT_RED();} \
        if(changed_g&(bit_mask)){screen_green[offset__]^=pixel__;RGB_STAT_GREEN();} \
        if(changed_b&(bit_mask)){screen_blue[offset__]^=pixel__;RGB_STAT_BLUE();} \
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
    const u16 far *map=placement_entry;
    const u8 far *next_r,*next_g,*next_b;
    u8 far *current_r,*current_g,*current_b;
    u16 i;
    u8 changed_r,changed_g,changed_b,changed_any;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!rgb3_active||!placement_entry||!next_codewords||!current_codewords||
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
        changed_any=(u8)(changed_r|changed_g|changed_b);
        if(changed_any)APPLY_CHANGED_BYTE3(map);
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
    const u16 far *map=placement_entry;
    u8 far *current_r,*current_g,*current_b;
    u16 cw=0;
    int row,block;
    u8 value_r,value_g,value_b,changed_r,changed_g,changed_b,changed_any;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif

    if(!rgb3_active||!placement_entry||!data_codewords||!ecc_blocks||
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
        changed_any=(u8)(changed_r|changed_g|changed_b); \
        if(changed_any)APPLY_CHANGED_BYTE3(map); \
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
#undef RGB_STAT_BLUE
#undef RGB_STAT_GREEN
#undef RGB_STAT_RED
#undef RGB_STAT_MAP

int vga_delta_ready(void) {
    return placement_entry!=0;
}

int vga_rgb3_active(void) {
    return rgb3_active;
}

/* Dense monochrome renderer matching the RGB3 direct algorithm: start from
 * the fixed function raster and toggle every set codeword module through the
 * sequential placement map. */
static int direct_scatter_bw(const u8 *codewords) {
    const u16 far *map=placement_entry;
    const u8 far *source=codewords;
    u16 i;
    u8 value;

    if(rgb3_active||!placement_entry||!codewords)return 0;
#define DIRECT_BW_MODULE(k,b) do { \
    if(value&(b)) { \
        u16 entry__=(map)[k]; \
        screen_320[entry__&0x1FFF]^=pixel_mask[entry__>>13]; \
    } \
} while(0)
    for(i=0;i<DOSFER_QR_CODEWORDS;++i,map+=8) {
        value=*source++;
        DIRECT_BW_MODULE(0,0x80); DIRECT_BW_MODULE(1,0x40);
        DIRECT_BW_MODULE(2,0x20); DIRECT_BW_MODULE(3,0x10);
        DIRECT_BW_MODULE(4,0x08); DIRECT_BW_MODULE(5,0x04);
        DIRECT_BW_MODULE(6,0x02); DIRECT_BW_MODULE(7,0x01);
    }
#undef DIRECT_BW_MODULE
    return 1;
}

static int direct_scatter_bw_optimized(const u8 *codewords);

static int ensure_direct_template(void) {
    if(!direct_template)direct_template=(u8 far *)_fmalloc(VGA_VISIBLE_BYTES);
    return direct_template!=0;
}

static int capture_bw_template(const u8 *codewords) {
    if(!ensure_direct_template())return 0;
    if(!direct_scatter_bw_optimized(codewords))return 0;
    _fmemcpy(direct_template,screen_320,VGA_VISIBLE_BYTES);
    return direct_scatter_bw_optimized(codewords);
}

int vga_direct_ready(void) {
    return placement_entry&&direct_template;
}

int vga_apply_codewords_direct(const u8 *codewords) {
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif
    if(rgb3_active||!vga_direct_ready())return 0;
    dosferCopyQr320(direct_template,screen_320);
    if(!direct_scatter_bw_optimized(codewords))return 0;
#ifdef DOSFER_PROFILE
    dosferVgaProfileTicks[0]+=timer_ticks()-profile_start;
#endif
    return 1;
}

/* Dense production renderer. It starts from a function-module raster template
 * and directly scatters all three interleaved V40-L streams through the
 * existing placement map. It deliberately has no previous-codeword state or
 * delta comparison. */
static int direct_scatter_rgb3(const u8 *const codewords[VGA_RGB_CHANNELS]) {
    const u16 far *map=placement_entry;
    const u8 far *red,*green,*blue;
    u16 i;
    u8 value_r,value_g,value_b,value_any;

    if(!rgb3_active||!placement_entry||!codewords||!codewords[VGA_RGB_RED]||
            !codewords[VGA_RGB_GREEN]||!codewords[VGA_RGB_BLUE])return 0;
    red=codewords[VGA_RGB_RED];green=codewords[VGA_RGB_GREEN];
    blue=codewords[VGA_RGB_BLUE];
#define DIRECT_MODULE(k,b) do { \
    if(value_any&(b)) { \
        u16 entry__=(map)[k]; \
        u16 offset__=(u16)(entry__&0x1FFF); \
        u8 pixel__=pixel_mask[entry__>>13]; \
        if(value_r&(b))screen_320[offset__]^=pixel__; \
        if(value_g&(b))screen_green[offset__]^=pixel__; \
        if(value_b&(b))screen_blue[offset__]^=pixel__; \
    } \
} while(0)
    for(i=0;i<DOSFER_QR_CODEWORDS;++i,map+=8) {
        value_r=*red++;value_g=*green++;value_b=*blue++;
        value_any=(u8)(value_r|value_g|value_b);
        if(value_any) {
            DIRECT_MODULE(0,0x80); DIRECT_MODULE(1,0x40);
            DIRECT_MODULE(2,0x20); DIRECT_MODULE(3,0x10);
            DIRECT_MODULE(4,0x08); DIRECT_MODULE(5,0x04);
            DIRECT_MODULE(6,0x02); DIRECT_MODULE(7,0x01);
        }
    }
#undef DIRECT_MODULE
    return 1;
}

static void direct_scatter_rgb3_range(
        const u8 *const codewords[VGA_RGB_CHANNELS],u16 first,u16 count) {
    const u16 far *map=placement_entry+(u32)first*8UL;
    const u8 far *red=codewords[VGA_RGB_RED]+first;
    const u8 far *green=codewords[VGA_RGB_GREEN]+first;
    const u8 far *blue=codewords[VGA_RGB_BLUE]+first;
    u16 i;
    u8 value_r,value_g,value_b,value_any;

#define DIRECT_RANGE_MODULE(k,b) do { \
    if(value_any&(b)) { \
        u16 entry__=(map)[k]; \
        u16 offset__=(u16)(entry__&0x1FFF); \
        u8 pixel__=pixel_mask[entry__>>13]; \
        if(value_r&(b))screen_320[offset__]^=pixel__; \
        if(value_g&(b))screen_green[offset__]^=pixel__; \
        if(value_b&(b))screen_blue[offset__]^=pixel__; \
    } \
} while(0)
    for(i=0;i<count;++i,map+=8) {
        value_r=*red++;value_g=*green++;value_b=*blue++;
        value_any=(u8)(value_r|value_g|value_b);
        if(value_any) {
            DIRECT_RANGE_MODULE(0,0x80);DIRECT_RANGE_MODULE(1,0x40);
            DIRECT_RANGE_MODULE(2,0x20);DIRECT_RANGE_MODULE(3,0x10);
            DIRECT_RANGE_MODULE(4,0x08);DIRECT_RANGE_MODULE(5,0x04);
            DIRECT_RANGE_MODULE(6,0x02);DIRECT_RANGE_MODULE(7,0x01);
        }
    }
#undef DIRECT_RANGE_MODULE
}

static int direct_scatter_rgb3_optimized(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
static int direct_scatter_rgb3_stripe_optimized(
        const u8 *const codewords[VGA_RGB_CHANNELS]);

static int capture_rgb3_template(const u8 *const codewords[VGA_RGB_CHANNELS]) {
    int ok;
    if(!ensure_direct_template())return 0;
    /* The current shadows contain a complete QR. Toggling its data modules
     * out leaves the mask-specific fixed V40-L function raster. */
    if(!direct_scatter_rgb3_stripe_optimized(codewords))return 0;
    _fmemcpy(direct_template,screen_320,VGA_VISIBLE_BYTES);
#ifdef DOSFER_DEVTOOLS
    ok=!memcmp(screen_320,screen_green,VGA_VISIBLE_BYTES)&&
       !memcmp(screen_320,screen_blue,VGA_VISIBLE_BYTES);
#else
    ok=1;
#endif
    /* Restore the complete QR even when the diagnostic equality check fails. */
    if(!direct_scatter_rgb3_stripe_optimized(codewords))return 0;
    return ok;
}

int vga_apply_codewords3_direct(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks();
#endif
    if(!rgb3_active||!vga_direct_ready())return 0;
    dosferCopyQr320(direct_template,screen_320);
    dosferCopyQr320(direct_template,screen_green);
    dosferCopyQr320(direct_template,screen_blue);
    if(!direct_scatter_rgb3_stripe_optimized(codewords))return 0;
#ifdef DOSFER_PROFILE
    dosferVgaProfileTicks[0]+=timer_ticks()-profile_start;
#endif
    return 1;
}

#ifdef DOSFER_DIRECT_BENCH
int vga_rgb3_direct_reset_template(void) {
    if(!rgb3_active||!vga_direct_ready())return 0;
    dosferCopyQr320(direct_template,screen_320);
    dosferCopyQr320(direct_template,screen_green);
    dosferCopyQr320(direct_template,screen_blue);
    return 1;
}

int vga_rgb3_direct_scatter(const u8 *const codewords[VGA_RGB_CHANNELS]) {
    return direct_scatter_rgb3(codewords);
}

int vga_direct_reset_template(void) {
    if(rgb3_active||!vga_direct_ready())return 0;
    dosferCopyQr320(direct_template,screen_320);
    return 1;
}

int vga_direct_scatter(const u8 *codewords) {
    return direct_scatter_bw(codewords);
}

int vga_direct_scatter_asm(const u8 *codewords) {
    return direct_scatter_bw_optimized(codewords);
}

#endif

#ifdef __WATCOMC__
/* Exact BW codeword-centric scatter. FS streams the codewords, DS streams
 * the placement map, and SS explicitly addresses near renderer state. */
static void directScatterBw386(void);
#pragma aux directScatterBw386 = \
    "push ax" "push bx" "push cx" "push dx" \
    "push si" "push di" "push bp" \
    "push ds" "push fs" \
    "mov di,word ptr ss:direct_asm_bw" \
    "mov ax,word ptr ss:direct_asm_bw+2" "mov fs,ax" \
    "mov si,word ptr ss:direct_asm_map" \
    "mov ax,word ptr ss:direct_asm_map+2" "mov ds,ax" \
    "mov cx,1853" \
    "direct_bw_loop:" \
    "mov al,fs:[di]" "inc di" \
    "test al,80h" "jz direct_bw_m1" \
    "movzx ebp,word ptr [si]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_m1:" "test al,40h" "jz direct_bw_m2" \
    "movzx ebp,word ptr [si+2]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_m2:" "test al,20h" "jz direct_bw_m3" \
    "movzx ebp,word ptr [si+4]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_m3:" "test al,10h" "jz direct_bw_m4" \
    "movzx ebp,word ptr [si+6]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_m4:" "test al,08h" "jz direct_bw_m5" \
    "movzx ebp,word ptr [si+8]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_m5:" "test al,04h" "jz direct_bw_m6" \
    "movzx ebp,word ptr [si+10]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_m6:" "test al,02h" "jz direct_bw_m7" \
    "movzx ebp,word ptr [si+12]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_m7:" "test al,01h" "jz direct_bw_done" \
    "movzx ebp,word ptr [si+14]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw_done:" "add si,16" \
    "mov al,fs:[di]" "inc di" \
    "test al,80h" "jz direct_bw2_m1" \
    "movzx ebp,word ptr [si]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_m1:" "test al,40h" "jz direct_bw2_m2" \
    "movzx ebp,word ptr [si+2]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_m2:" "test al,20h" "jz direct_bw2_m3" \
    "movzx ebp,word ptr [si+4]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_m3:" "test al,10h" "jz direct_bw2_m4" \
    "movzx ebp,word ptr [si+6]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_m4:" "test al,08h" "jz direct_bw2_m5" \
    "movzx ebp,word ptr [si+8]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_m5:" "test al,04h" "jz direct_bw2_m6" \
    "movzx ebp,word ptr [si+10]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_m6:" "test al,02h" "jz direct_bw2_m7" \
    "movzx ebp,word ptr [si+12]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_m7:" "test al,01h" "jz direct_bw2_done" \
    "movzx ebp,word ptr [si+14]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_bw2_done:" "add si,16" \
    "dec cx" "jnz direct_bw_loop" \
    "pop fs" "pop ds" \
    "pop bp" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp fs];

/* Exact codeword-centric scatter, with all eight source bits and two
 * codewords per outer iteration unrolled. DS streams the packed placement
 * map; FS/GS/ES stream R/G/B once per codeword; SS explicitly addresses the
 * three near shadow rasters and pixel-mask LUT. */
static void directScatter386(void);
#pragma aux directScatter386 = \
    "push ax" "push bx" "push cx" "push dx" \
    "push si" "push di" "push bp" \
    "push ds" "push es" "push fs" "push gs" \
    "mov di,word ptr ss:direct_asm_red" \
    "mov ax,word ptr ss:direct_asm_red+2" "mov fs,ax" \
    "mov bp,word ptr ss:direct_asm_green" \
    "mov ax,word ptr ss:direct_asm_green+2" "mov gs,ax" \
    "mov bx,word ptr ss:direct_asm_blue" \
    "mov ax,word ptr ss:direct_asm_blue+2" "mov es,ax" \
    "mov si,word ptr ss:direct_asm_map" \
    "mov ax,word ptr ss:direct_asm_map+2" "mov ds,ax" \
    "mov cx,1853" \
    "direct_asm1_loop:" \
    "mov al,fs:[di]" "inc di" \
    "mov ah,gs:[bp]" "inc bp" \
    "mov dh,es:[bx]" "inc bx" \
    "push bp" "push bx" \
    "movzx ebp,word ptr [si]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,80h" "jz direct_asm1_m0_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m0_g:" "test ah,80h" "jz direct_asm1_m0_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m0_b:" "test dh,80h" "jz direct_asm1_m0_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m0_done:" \
    "movzx ebp,word ptr [si+2]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,40h" "jz direct_asm1_m1_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m1_g:" "test ah,40h" "jz direct_asm1_m1_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m1_b:" "test dh,40h" "jz direct_asm1_m1_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m1_done:" \
    "movzx ebp,word ptr [si+4]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,20h" "jz direct_asm1_m2_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m2_g:" "test ah,20h" "jz direct_asm1_m2_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m2_b:" "test dh,20h" "jz direct_asm1_m2_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m2_done:" \
    "movzx ebp,word ptr [si+6]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,10h" "jz direct_asm1_m3_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m3_g:" "test ah,10h" "jz direct_asm1_m3_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m3_b:" "test dh,10h" "jz direct_asm1_m3_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m3_done:" \
    "movzx ebp,word ptr [si+8]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,08h" "jz direct_asm1_m4_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m4_g:" "test ah,08h" "jz direct_asm1_m4_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m4_b:" "test dh,08h" "jz direct_asm1_m4_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m4_done:" \
    "movzx ebp,word ptr [si+10]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,04h" "jz direct_asm1_m5_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m5_g:" "test ah,04h" "jz direct_asm1_m5_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m5_b:" "test dh,04h" "jz direct_asm1_m5_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m5_done:" \
    "movzx ebp,word ptr [si+12]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,02h" "jz direct_asm1_m6_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m6_g:" "test ah,02h" "jz direct_asm1_m6_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m6_b:" "test dh,02h" "jz direct_asm1_m6_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m6_done:" \
    "movzx ebp,word ptr [si+14]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,01h" "jz direct_asm1_m7_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm1_m7_g:" "test ah,01h" "jz direct_asm1_m7_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm1_m7_b:" "test dh,01h" "jz direct_asm1_m7_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm1_m7_done:" \
    "pop bx" "pop bp" "add si,16" \
    "mov al,fs:[di]" "inc di" \
    "mov ah,gs:[bp]" "inc bp" \
    "mov dh,es:[bx]" "inc bx" \
    "push bp" "push bx" \
    "movzx ebp,word ptr [si]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,80h" "jz direct_asm2_m0_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m0_g:" "test ah,80h" "jz direct_asm2_m0_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m0_b:" "test dh,80h" "jz direct_asm2_m0_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m0_done:" \
    "movzx ebp,word ptr [si+2]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,40h" "jz direct_asm2_m1_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m1_g:" "test ah,40h" "jz direct_asm2_m1_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m1_b:" "test dh,40h" "jz direct_asm2_m1_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m1_done:" \
    "movzx ebp,word ptr [si+4]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,20h" "jz direct_asm2_m2_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m2_g:" "test ah,20h" "jz direct_asm2_m2_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m2_b:" "test dh,20h" "jz direct_asm2_m2_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m2_done:" \
    "movzx ebp,word ptr [si+6]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,10h" "jz direct_asm2_m3_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m3_g:" "test ah,10h" "jz direct_asm2_m3_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m3_b:" "test dh,10h" "jz direct_asm2_m3_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m3_done:" \
    "movzx ebp,word ptr [si+8]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,08h" "jz direct_asm2_m4_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m4_g:" "test ah,08h" "jz direct_asm2_m4_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m4_b:" "test dh,08h" "jz direct_asm2_m4_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m4_done:" \
    "movzx ebp,word ptr [si+10]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,04h" "jz direct_asm2_m5_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m5_g:" "test ah,04h" "jz direct_asm2_m5_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m5_b:" "test dh,04h" "jz direct_asm2_m5_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m5_done:" \
    "movzx ebp,word ptr [si+12]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,02h" "jz direct_asm2_m6_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m6_g:" "test ah,02h" "jz direct_asm2_m6_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m6_b:" "test dh,02h" "jz direct_asm2_m6_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m6_done:" \
    "movzx ebp,word ptr [si+14]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,01h" "jz direct_asm2_m7_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_asm2_m7_g:" "test ah,01h" "jz direct_asm2_m7_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_asm2_m7_b:" "test dh,01h" "jz direct_asm2_m7_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_asm2_m7_done:" \
    "pop bx" "pop bp" "add si,16" \
    "dec cx" "jnz direct_asm1_loop" \
    "pop gs" "pop fs" "pop es" "pop ds" \
    "pop bp" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp es fs gs];

/* Exact one-codeword RGB scatter for the short function-pattern gaps.  The
 * algorithm and packed placement entries are identical to directScatter386;
 * only the source/map start and count are parameterized. */
static void directScatterRange386(void);
#pragma aux directScatterRange386 = \
    "push ax" "push bx" "push cx" "push dx" "push si" "push di" "push bp" \
    "push ds" "push es" "push fs" "push gs" \
    "mov di,word ptr ss:direct_range_source_offset" "mov bp,di" "mov bx,di" \
    "mov ax,word ptr ss:direct_asm_red+2" "mov fs,ax" \
    "mov ax,word ptr ss:direct_asm_green+2" "mov gs,ax" \
    "mov ax,word ptr ss:direct_asm_blue+2" "mov es,ax" \
    "mov si,word ptr ss:direct_range_map" \
    "mov ax,word ptr ss:direct_range_map+2" "mov ds,ax" \
    "mov cx,word ptr ss:direct_range_count" "test cx,cx" "jz direct_range_done" \
    "direct_range_loop:" \
    "mov al,fs:[di]" "inc di" "mov ah,gs:[bp]" "inc bp" \
    "mov dh,es:[bx]" "inc bx" "push bp" "push bx" \
    "movzx ebp,word ptr [si]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,80h" "jz direct_range_m0_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m0_g:" "test ah,80h" "jz direct_range_m0_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m0_b:" "test dh,80h" "jz direct_range_m0_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m0_done:" \
    "movzx ebp,word ptr [si+2]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,40h" "jz direct_range_m1_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m1_g:" "test ah,40h" "jz direct_range_m1_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m1_b:" "test dh,40h" "jz direct_range_m1_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m1_done:" \
    "movzx ebp,word ptr [si+4]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,20h" "jz direct_range_m2_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m2_g:" "test ah,20h" "jz direct_range_m2_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m2_b:" "test dh,20h" "jz direct_range_m2_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m2_done:" \
    "movzx ebp,word ptr [si+6]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,10h" "jz direct_range_m3_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m3_g:" "test ah,10h" "jz direct_range_m3_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m3_b:" "test dh,10h" "jz direct_range_m3_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m3_done:" \
    "movzx ebp,word ptr [si+8]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,08h" "jz direct_range_m4_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m4_g:" "test ah,08h" "jz direct_range_m4_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m4_b:" "test dh,08h" "jz direct_range_m4_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m4_done:" \
    "movzx ebp,word ptr [si+10]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,04h" "jz direct_range_m5_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m5_g:" "test ah,04h" "jz direct_range_m5_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m5_b:" "test dh,04h" "jz direct_range_m5_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m5_done:" \
    "movzx ebp,word ptr [si+12]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,02h" "jz direct_range_m6_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m6_g:" "test ah,02h" "jz direct_range_m6_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m6_b:" "test dh,02h" "jz direct_range_m6_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m6_done:" \
    "movzx ebp,word ptr [si+14]" "mov bx,bp" "and bx,1fffh" "shr ebp,13" \
    "mov dl,byte ptr ss:pixel_mask[bp]" \
    "test al,01h" "jz direct_range_m7_g" "xor byte ptr ss:screen_320[bx],dl" \
    "direct_range_m7_g:" "test ah,01h" "jz direct_range_m7_b" "xor byte ptr ss:screen_green[bx],dl" \
    "direct_range_m7_b:" "test dh,01h" "jz direct_range_m7_done" "xor byte ptr ss:screen_blue[bx],dl" \
    "direct_range_m7_done:" \
    "pop bx" "pop bp" "add si,16" "dec cx" "jnz direct_range_loop" \
    "direct_range_done:" \
    "pop gs" "pop fs" "pop es" "pop ds" "pop bp" "pop di" "pop si" \
    "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp es fs gs];

/* Run-level two-column stripe kernel. All three codeword streams have the
 * same checked far offset. Each source byte is consumed as four two-bit row
 * pairs; DS:SI supplies the run's four combined masks and DI advances by the
 * fixed signed 40-byte row stride. Zero masks are intentionally XORed too:
 * eliminating twelve data-dependent branches is cheaper to test than a
 * second recipe/branch layer on statistically independent RGB data. */
static void directStripe386(u16 source_offset,u16 count,u16 dest,
        u16 pair_mask_offset);
#pragma aux directStripe386 = \
    "push bp" "push ds" "push es" "push fs" "push gs" \
    "mov bp,ax" \
    "mov ax,word ptr ss:direct_asm_red+2" "mov fs,ax" \
    "mov ax,word ptr ss:direct_asm_green+2" "mov gs,ax" \
    "mov ax,word ptr ss:direct_asm_blue+2" "mov es,ax" \
    "mov ax,word ptr ss:stripe_asm_pair_mask+2" "mov ds,ax" \
    "xor bh,bh" \
    "test cx,cx" "jz stripe_done" \
    "stripe_loop:" \
    "mov al,fs:[bp]" "mov ah,gs:[bp]" "mov dh,es:[bp]" "inc bp" \
    /* Row pair 0. */ \
    "rol al,2" "mov bl,al" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_320[di],dl" \
    "rol ah,2" "mov bl,ah" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_green[di],dl" \
    "rol dh,2" "mov bl,dh" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_blue[di],dl" \
    "add di,word ptr ss:stripe_asm_step" \
    /* Row pair 1. */ \
    "rol al,2" "mov bl,al" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_320[di],dl" \
    "rol ah,2" "mov bl,ah" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_green[di],dl" \
    "rol dh,2" "mov bl,dh" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_blue[di],dl" \
    "add di,word ptr ss:stripe_asm_step" \
    /* Row pair 2. */ \
    "rol al,2" "mov bl,al" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_320[di],dl" \
    "rol ah,2" "mov bl,ah" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_green[di],dl" \
    "rol dh,2" "mov bl,dh" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_blue[di],dl" \
    "add di,word ptr ss:stripe_asm_step" \
    /* Row pair 3. */ \
    "rol al,2" "mov bl,al" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_320[di],dl" \
    "rol ah,2" "mov bl,ah" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_green[di],dl" \
    "rol dh,2" "mov bl,dh" "and bl,3" "mov dl,ds:[si+bx]" "xor byte ptr ss:screen_blue[di],dl" \
    "add di,word ptr ss:stripe_asm_step" \
    "dec cx" "jnz stripe_loop" \
    "stripe_done:" \
    "pop gs" "pop fs" "pop es" "pop ds" "pop bp" \
    parm [ax] [cx] [di] [si] modify [ax bx cx dx di];

/* Four regular two-column runs fill one framebuffer byte. Pack their source
 * bytes into EAX, normalize the alternating QR zigzag directions, transpose
 * the four-by-four matrix of two-bit pairs, then update four destination rows
 * once each. This plane kernel is invoked three times per group. */
static void directStripeGroupPlane386(void);
#pragma aux directStripeGroupPlane386 = \
    "push ax" "push bx" "push cx" "push dx" "push si" "push di" "push bp" "push fs" \
    "mov ax,word ptr ss:stripe_group_source+2" "mov fs,ax" \
    "mov bp,word ptr ss:stripe_group_src0" \
    "mov si,word ptr ss:stripe_group_src1" \
    "mov bx,word ptr ss:stripe_group_src2" \
    "mov di,word ptr ss:stripe_group_dest" \
    "mov cx,word ptr ss:stripe_group_count_asm" \
    "test cx,cx" "jz stripe_group_done" \
    "stripe_group_loop:" \
    "mov al,fs:[bp]" "dec bp" \
    "mov ah,fs:[si]" "inc si" \
    "mov dl,fs:[bx]" "dec bx" \
    "push di" "mov di,word ptr ss:stripe_group_src3" \
    "mov dh,fs:[di]" "inc di" "mov word ptr ss:stripe_group_src3,di" "pop di" \
    "movzx eax,ax" "movzx edx,dx" "shl edx,16" "or eax,edx" \
    /* Reverse all bits for the upward 03/30 lanes; swap adjacent pair bits
     * for the downward 0C/C0 lanes. */ \
    "push bx" "mov edx,eax" "xor bh,bh" \
    "mov bl,al" "mov al,byte ptr ss:stripe_bit_reverse[bx]" \
    "mov bl,ah" "mov ah,byte ptr ss:stripe_pair_swap[bx]" \
    "shr edx,16" \
    "mov bl,dl" "mov dl,byte ptr ss:stripe_bit_reverse[bx]" \
    "mov bl,dh" "mov dh,byte ptr ss:stripe_pair_swap[bx]" \
    "and eax,0000ffffh" "shl edx,16" "or eax,edx" "pop bx" \
    /* Transpose four bytes of four two-bit row pairs. The result byte order
     * is bottom-to-top, matching the destination walk below. */ \
    "mov edx,eax" "shr edx,6" "xor edx,eax" "and edx,00cc00cch" \
    "xor eax,edx" "shl edx,6" "xor eax,edx" \
    "mov edx,eax" "shr edx,12" "xor edx,eax" "and edx,0000f0f0h" \
    "xor eax,edx" "shl edx,12" "xor eax,edx" \
    "xor byte ptr ss:[di],al" "sub di,40" \
    "xor byte ptr ss:[di],ah" "sub di,40" \
    "ror eax,16" "xor byte ptr ss:[di],al" "sub di,40" \
    "xor byte ptr ss:[di],ah" "add di,280" \
    "dec cx" "jnz stripe_group_loop" \
    "stripe_group_done:" \
    "pop fs" "pop bp" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp fs];

/* Four far LUT reads replace byte normalization and the 4x4 two-bit
 * transpose for the exact same four-lane group operation.  The table is
 * derived from the placement geometry during renderer setup. */
static void directStripeGroupLutPlane386(void);
#pragma aux directStripeGroupLutPlane386 = \
    "push ax" "push bx" "push cx" "push dx" "push si" "push di" "push bp" \
    "push ds" "push fs" \
    "mov ax,word ptr ss:stripe_group_source+2" "mov fs,ax" \
    "mov ax,word ptr ss:stripe_group_lut+2" "mov ds,ax" \
    "mov bp,word ptr ss:stripe_group_src0" \
    "mov si,word ptr ss:stripe_group_src1" \
    "mov bx,word ptr ss:stripe_group_src2" \
    "mov di,word ptr ss:stripe_group_dest" \
    "mov cx,word ptr ss:stripe_group_count_asm" \
    "test cx,cx" "jz stripe_group_lut_done" \
    "stripe_group_lut_loop:" \
    "xor eax,eax" "mov al,fs:[bp]" "dec bp" "shl eax,2" \
    "mov edx,dword ptr ds:[eax]" \
    "xor eax,eax" "mov al,fs:[si]" "inc si" "shl eax,2" \
    "xor edx,dword ptr ds:[eax+1024]" \
    "xor eax,eax" "mov al,fs:[bx]" "dec bx" "shl eax,2" \
    "xor edx,dword ptr ds:[eax+2048]" \
    "push di" "mov di,word ptr ss:stripe_group_src3" \
    "xor eax,eax" "mov al,fs:[di]" "inc di" \
    "mov word ptr ss:stripe_group_src3,di" "pop di" "shl eax,2" \
    "xor edx,dword ptr ds:[eax+3072]" \
    "xor byte ptr ss:[di],dl" "sub di,40" \
    "xor byte ptr ss:[di],dh" "sub di,40" \
    "shr edx,16" "xor byte ptr ss:[di],dl" "sub di,40" \
    "xor byte ptr ss:[di],dh" "add di,280" \
    "dec cx" "jnz stripe_group_lut_loop" \
    "stripe_group_lut_done:" \
    "pop fs" "pop ds" "pop bp" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp ds fs];
/* Same byte transpose as the production group kernel, but two lanes begin
 * halfway through a normalized codeword.  Combine the current low half with
 * the following high half before transposing.  Phase 1 shifts the reverse
 * lanes (03/30); phase 2 shifts the forward lanes (0C/C0). */
static void directStripePhasePlane386(void);
#pragma aux directStripePhasePlane386 = \
    "push ax" "push bx" "push cx" "push dx" "push si" "push di" "push bp" "push fs" \
    "mov ax,word ptr ss:stripe_group_source+2" "mov fs,ax" \
    "mov bp,word ptr ss:stripe_group_src0" \
    "mov si,word ptr ss:stripe_group_src1" \
    "mov bx,word ptr ss:stripe_group_src2" \
    "mov di,word ptr ss:stripe_group_dest" \
    "mov cx,word ptr ss:stripe_group_count_asm" \
    "test cx,cx" "jz stripe_phase_done" \
    "stripe_phase_loop:" \
    "mov al,fs:[bp]" "dec bp" \
    "mov ah,fs:[si]" "inc si" \
    "mov dl,fs:[bx]" "dec bx" \
    "push di" "mov di,word ptr ss:stripe_group_src3" \
    "mov dh,fs:[di]" "inc di" "mov word ptr ss:stripe_group_src3,di" "pop di" \
    "movzx eax,ax" "movzx edx,dx" "shl edx,16" "or eax,edx" \
    "push bx" "mov edx,eax" "xor bh,bh" \
    "mov bl,al" "mov al,byte ptr ss:stripe_bit_reverse[bx]" \
    "mov bl,ah" "mov ah,byte ptr ss:stripe_pair_swap[bx]" \
    "shr edx,16" \
    "mov bl,dl" "mov dl,byte ptr ss:stripe_bit_reverse[bx]" \
    "mov bl,dh" "mov dh,byte ptr ss:stripe_pair_swap[bx]" \
    "and eax,0000ffffh" "shl edx,16" "or eax,edx" "pop bx" \
    "cmp byte ptr ss:stripe_phase_type,1" "jne stripe_phase_forward" \
    /* Reverse lanes use the next descending source codeword. */ \
    "push si" "xor dx,dx" \
    "mov dl,fs:[bp]" "mov si,dx" \
    "mov dl,byte ptr ss:stripe_bit_reverse[si]" \
    "mov dh,fs:[bx]" "movzx si,dh" \
    "mov dh,byte ptr ss:stripe_bit_reverse[si]" "pop si" \
    "shl al,4" "shr dl,4" "or al,dl" \
    "ror eax,16" "shl al,4" "shr dh,4" "or al,dh" "ror eax,16" \
    "jmp short stripe_phase_merged" \
    "stripe_phase_forward:" \
    /* Forward lanes use the next ascending source codeword. */ \
    "push bx" "xor dx,dx" \
    "mov dl,fs:[si]" "movzx bx,dl" \
    "mov dl,byte ptr ss:stripe_pair_swap[bx]" \
    "mov bx,word ptr ss:stripe_group_src3" "mov dh,fs:[bx]" \
    "movzx bx,dh" "mov dh,byte ptr ss:stripe_pair_swap[bx]" "pop bx" \
    "shl ah,4" "shr dl,4" "or ah,dl" \
    "ror eax,16" "shl ah,4" "shr dh,4" "or ah,dh" "ror eax,16" \
    "stripe_phase_merged:" \
    "mov edx,eax" "shr edx,6" "xor edx,eax" "and edx,00cc00cch" \
    "xor eax,edx" "shl edx,6" "xor eax,edx" \
    "mov edx,eax" "shr edx,12" "xor edx,eax" "and edx,0000f0f0h" \
    "xor eax,edx" "shl edx,12" "xor eax,edx" \
    "xor byte ptr ss:[di],al" "sub di,40" \
    "xor byte ptr ss:[di],ah" "sub di,40" \
    "ror eax,16" "xor byte ptr ss:[di],al" "sub di,40" \
    "xor byte ptr ss:[di],ah" "add di,280" \
    "dec cx" "jnz stripe_phase_loop" \
    "stripe_phase_done:" \
    "pop fs" "pop bp" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp fs];

/* Three-lane complete-byte kernel. Lane 03 is deliberately zero; the
 * uninterrupted 0C/30/C0 streams are normalized and transposed exactly like
 * the four-lane production group kernel. */
static void directStripeTriplePlane386(void);
#pragma aux directStripeTriplePlane386 = \
    "push ax" "push bx" "push cx" "push dx" "push si" "push di" "push bp" "push fs" \
    "mov ax,word ptr ss:stripe_group_source+2" "mov fs,ax" \
    "mov si,word ptr ss:stripe_group_src1" \
    "mov bx,word ptr ss:stripe_group_src2" \
    "mov di,word ptr ss:stripe_group_dest" \
    "mov cx,word ptr ss:stripe_group_count_asm" \
    "test cx,cx" "jz stripe_triple_done" \
    "stripe_triple_loop:" \
    "xor eax,eax" "mov ah,fs:[si]" "inc si" \
    "mov dl,fs:[bx]" "dec bx" \
    "push di" "mov di,word ptr ss:stripe_group_src3" \
    "mov dh,fs:[di]" "inc di" "mov word ptr ss:stripe_group_src3,di" "pop di" \
    "movzx edx,dx" "shl edx,16" "or eax,edx" \
    "push bx" "mov edx,eax" "xor bh,bh" \
    "mov bl,ah" "mov ah,byte ptr ss:stripe_pair_swap[bx]" \
    "shr edx,16" \
    "mov bl,dl" "mov dl,byte ptr ss:stripe_bit_reverse[bx]" \
    "mov bl,dh" "mov dh,byte ptr ss:stripe_pair_swap[bx]" \
    "and eax,0000ffffh" "shl edx,16" "or eax,edx" "pop bx" \
    "mov edx,eax" "shr edx,6" "xor edx,eax" "and edx,00cc00cch" \
    "xor eax,edx" "shl edx,6" "xor eax,edx" \
    "mov edx,eax" "shr edx,12" "xor edx,eax" "and edx,0000f0f0h" \
    "xor eax,edx" "shl edx,12" "xor eax,edx" \
    "xor byte ptr ss:[di],al" "sub di,40" \
    "xor byte ptr ss:[di],ah" "sub di,40" \
    "ror eax,16" "xor byte ptr ss:[di],al" "sub di,40" \
    "xor byte ptr ss:[di],ah" "add di,280" \
    "dec cx" "jnz stripe_triple_loop" \
    "stripe_triple_done:" \
    "pop fs" "pop bp" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp fs];

static void directStripeTripleLutPlane386(void);
#pragma aux directStripeTripleLutPlane386 = \
    "push ax" "push bx" "push cx" "push dx" "push si" "push di" \
    "push ds" "push fs" \
    "mov ax,word ptr ss:stripe_group_source+2" "mov fs,ax" \
    "mov ax,word ptr ss:stripe_group_lut+2" "mov ds,ax" \
    "mov si,word ptr ss:stripe_group_src1" \
    "mov bx,word ptr ss:stripe_group_src2" \
    "mov di,word ptr ss:stripe_group_dest" \
    "mov cx,word ptr ss:stripe_group_count_asm" \
    "test cx,cx" "jz stripe_triple_lut_done" \
    "stripe_triple_lut_loop:" \
    "xor eax,eax" "mov al,fs:[si]" "inc si" "shl eax,2" \
    "mov edx,dword ptr ds:[eax+1024]" \
    "xor eax,eax" "mov al,fs:[bx]" "dec bx" "shl eax,2" \
    "xor edx,dword ptr ds:[eax+2048]" \
    "push di" "mov di,word ptr ss:stripe_group_src3" \
    "xor eax,eax" "mov al,fs:[di]" "inc di" \
    "mov word ptr ss:stripe_group_src3,di" "pop di" "shl eax,2" \
    "xor edx,dword ptr ds:[eax+3072]" \
    "xor byte ptr ss:[di],dl" "sub di,40" \
    "xor byte ptr ss:[di],dh" "sub di,40" \
    "shr edx,16" "xor byte ptr ss:[di],dl" "sub di,40" \
    "xor byte ptr ss:[di],dh" "add di,280" \
    "dec cx" "jnz stripe_triple_lut_loop" \
    "stripe_triple_lut_done:" \
    "pop fs" "pop ds" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di ds fs];
/* Predecoded 10-byte edge recipes avoid the generic C run lookup and update
 * all three planes while the source byte and destination are hot. */
static void directStripeEdges386(void);
#pragma aux directStripeEdges386 = \
    "push ax" "push bx" "push cx" "push dx" "push si" "push di" "push bp" \
    "push es" "push fs" "push gs" \
    "mov ax,word ptr ss:direct_asm_red+2" "mov fs,ax" \
    "mov ax,word ptr ss:direct_asm_green+2" "mov gs,ax" \
    "mov ax,word ptr ss:direct_asm_blue+2" "mov es,ax" \
    "mov si,offset stripe_phase_edges" \
    "mov cx,word ptr ss:stripe_phase_edge_count" \
    "xor bh,bh" "test cx,cx" "jz stripe_edges_done" \
    "stripe_edges_loop:" \
    "mov bp,word ptr ss:[si]" "add bp,word ptr ss:stripe_edge_source_offset" \
    "mov al,fs:[bp]" "mov ah,gs:[bp]" "mov dh,es:[bp]" \
    "push cx" "mov cl,byte ptr ss:[si+4]" \
    "shr al,cl" "shr ah,cl" "shr dh,cl" "pop cx" \
    "mov di,word ptr ss:[si+2]" \
    "mov bl,al" "and bl,3" "mov dl,byte ptr ss:[si+bx+5]" \
    "xor byte ptr ss:screen_320[di],dl" \
    "mov bl,ah" "and bl,3" "mov dl,byte ptr ss:[si+bx+5]" \
    "xor byte ptr ss:screen_green[di],dl" \
    "mov bl,dh" "and bl,3" "mov dl,byte ptr ss:[si+bx+5]" \
    "xor byte ptr ss:screen_blue[di],dl" \
    "add si,10" "dec cx" "jnz stripe_edges_loop" \
    "stripe_edges_done:" \
    "pop gs" "pop fs" "pop es" "pop bp" "pop di" "pop si" "pop dx" "pop cx" "pop bx" "pop ax" \
    modify [ax bx cx dx si di bp es fs gs];
#else
static void directScatterBw386(void) {}
static void directScatter386(void) {}
static void directScatterRange386(void) {}
static void directStripe386(u16 source_offset,u16 count,u16 dest,
        u16 pair_mask_offset) {
    (void)source_offset;(void)count;(void)dest;(void)pair_mask_offset;
}
static void directStripeGroupPlane386(void) {}
static void directStripePhasePlane386(void) {}
static void directStripeTriplePlane386(void) {}
static void directStripeEdges386(void) {}
#endif

static void direct_scatter_rgb3_range_optimized(
        const u8 *const codewords[VGA_RGB_CHANNELS],u16 source_base,
        u16 first,u16 count) {
#ifdef __WATCOMC__
    (void)codewords;
    direct_range_source_offset=(u16)(source_base+first);
    direct_range_count=count;
    direct_range_map=placement_entry+(u32)first*8UL;
    directScatterRange386();
#else
    (void)source_base;
    direct_scatter_rgb3_range(codewords,first,count);
#endif
}

static int direct_scatter_bw_optimized(const u8 *codewords) {
    if(rgb3_active||!placement_entry||!codewords)return 0;
#ifdef __WATCOMC__
    direct_asm_bw=codewords;
    direct_asm_map=placement_entry;
    directScatterBw386();
    return 1;
#else
    return direct_scatter_bw(codewords);
#endif
}

static int direct_scatter_rgb3_optimized(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    if(!rgb3_active||!placement_entry||!codewords||!codewords[VGA_RGB_RED]||
            !codewords[VGA_RGB_GREEN]||!codewords[VGA_RGB_BLUE])return 0;
#ifdef __WATCOMC__
    direct_asm_red=codewords[VGA_RGB_RED];
    direct_asm_green=codewords[VGA_RGB_GREEN];
    direct_asm_blue=codewords[VGA_RGB_BLUE];
    direct_asm_map=placement_entry;
    directScatter386();
    return 1;
#else
    return direct_scatter_rgb3(codewords);
#endif
}

static void stripe_group_plane(const VgaStripeGroup *group,
        const u8 far *codewords,int channel) {
    u16 offset=FP_OFF(codewords);
    stripe_group_source=codewords;
    stripe_group_src0=(u16)(offset+group->first[0]+group->count-1);
    stripe_group_src1=(u16)(offset+group->first[1]);
    stripe_group_src2=(u16)(offset+group->first[2]+group->count-1);
    stripe_group_src3=(u16)(offset+group->first[3]);
    stripe_group_dest=(u16)(FP_OFF((u8 far *)rgb_screen(channel))+
        group->dest+3u*VGA_BYTES_PER_LINE);
    stripe_group_count_asm=group->count;
#ifdef __WATCOMC__
    if(stripe_group_lut) {
        directStripeGroupLutPlane386();
        return;
    }
#endif
    directStripeGroupPlane386();
}

static void stripe_phase_group_plane(const VgaStripePhaseGroup *group,
        const u8 far *codewords,int channel) {
    u16 offset=FP_OFF(codewords);
    u16 reverse_extra=(u16)(group->phase==1?1:0);
    stripe_group_source=codewords;
    stripe_group_src0=(u16)(offset+group->first[0]+group->count-1+
        reverse_extra);
    stripe_group_src1=(u16)(offset+group->first[1]);
    stripe_group_src2=(u16)(offset+group->first[2]+group->count-1+
        reverse_extra);
    stripe_group_src3=(u16)(offset+group->first[3]);
    stripe_group_dest=(u16)(FP_OFF((u8 far *)rgb_screen(channel))+
        group->dest+3u*VGA_BYTES_PER_LINE);
    stripe_group_count_asm=group->count;
    stripe_phase_type=group->phase;
    directStripePhasePlane386();
}

static void stripe_triple_plane(const VgaStripeTriple *group,
        const u8 far *codewords,int channel) {
    u16 offset=FP_OFF(codewords);
    stripe_group_source=codewords;
    stripe_group_src1=(u16)(offset+group->first[0]);
    stripe_group_src2=(u16)(offset+group->first[1]+group->count-1);
    stripe_group_src3=(u16)(offset+group->first[2]);
    stripe_group_dest=(u16)(FP_OFF((u8 far *)rgb_screen(channel))+
        group->dest+3u*VGA_BYTES_PER_LINE);
    stripe_group_count_asm=group->count;
#ifdef __WATCOMC__
    if(stripe_group_lut) {
        directStripeTripleLutPlane386();
        return;
    }
#endif
    directStripeTriplePlane386();
}


static void scatter_phase_edges(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
#ifdef __WATCOMC__
    (void)codewords;
    stripe_edge_source_offset=FP_OFF(direct_asm_red);
    directStripeEdges386();
#else
    u16 edge_index,channel;
    for(edge_index=0;edge_index<stripe_phase_edge_count;++edge_index) {
        const VgaStripeEdge *edge=&stripe_phase_edges[edge_index];
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
            u8 value=(u8)((codewords[channel][edge->codeword]>>edge->shift)&3u);
            rgb_screen(channel)[edge->dest]^=edge->pair_mask[value];
        }
    }
#endif
}

static int direct_scatter_rgb3_phased_optimized(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
#ifdef __WATCOMC__
    u16 run_index,group_index,channel,offset,current=0;
    if(!rgb3_active||!placement_entry||!stripe_runs||!stripe_run_count||
       !stripe_group_count||!stripe_phase_group_count||!codewords||
       !codewords[VGA_RGB_RED]||!codewords[VGA_RGB_GREEN]||
       !codewords[VGA_RGB_BLUE])return 0;
    offset=FP_OFF(codewords[VGA_RGB_RED]);
    if(FP_OFF(codewords[VGA_RGB_GREEN])!=offset||
       FP_OFF(codewords[VGA_RGB_BLUE])!=offset||
       (u32)offset+DOSFER_QR_CODEWORDS>0xFFFFUL)return 0;
    direct_asm_red=codewords[VGA_RGB_RED];
    direct_asm_green=codewords[VGA_RGB_GREEN];
    direct_asm_blue=codewords[VGA_RGB_BLUE];
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(run->first>current)
            direct_scatter_rgb3_range_optimized(codewords,offset,current,
                (u16)(run->first-current));
        if(!run->group_flags) {
            stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
            stripe_asm_pair_mask=run->pair_mask;
            directStripe386((u16)(offset+run->first),run->count,run->dest,
                FP_OFF(run->pair_mask));
        }
        current=(u16)(run->first+run->count);
    }
    if(current<DOSFER_QR_CODEWORDS)
        direct_scatter_rgb3_range_optimized(codewords,offset,current,
            (u16)(DOSFER_QR_CODEWORDS-current));
    for(group_index=0;group_index<stripe_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_group_plane(&stripe_groups[group_index],codewords[channel],
                channel);
    for(group_index=0;group_index<stripe_phase_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
            stripe_phase_group_plane(&stripe_phase_groups[group_index],
                codewords[channel],channel);
        }
    scatter_phase_edges(codewords);
    for(group_index=0;group_index<stripe_triple_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_triple_plane(&stripe_triples[group_index],
                codewords[channel],channel);
    return 1;
#else
    return direct_scatter_rgb3(codewords);
#endif
}

static int direct_scatter_rgb3_grouped_optimized(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
#ifdef __WATCOMC__
    u16 run_index,group_index,channel,offset,current=0;
    if(!rgb3_active||!placement_entry||!stripe_runs||!stripe_run_count||
       !stripe_group_count||!codewords||!codewords[VGA_RGB_RED]||
       !codewords[VGA_RGB_GREEN]||!codewords[VGA_RGB_BLUE])return 0;
    offset=FP_OFF(codewords[VGA_RGB_RED]);
    if(FP_OFF(codewords[VGA_RGB_GREEN])!=offset||
       FP_OFF(codewords[VGA_RGB_BLUE])!=offset||
       (u32)offset+DOSFER_QR_CODEWORDS>0xFFFFUL)return 0;
    direct_asm_red=codewords[VGA_RGB_RED];
    direct_asm_green=codewords[VGA_RGB_GREEN];
    direct_asm_blue=codewords[VGA_RGB_BLUE];
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(run->first>current)
            direct_scatter_rgb3_range(codewords,current,
                (u16)(run->first-current));
        if(!(run->group_flags&VGA_STRIPE_GROUP_EXACT)) {
            stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
            stripe_asm_pair_mask=run->pair_mask;
            directStripe386((u16)(offset+run->first),run->count,run->dest,
                FP_OFF(run->pair_mask));
        }
        current=(u16)(run->first+run->count);
    }
    if(current<DOSFER_QR_CODEWORDS)
        direct_scatter_rgb3_range(codewords,current,
            (u16)(DOSFER_QR_CODEWORDS-current));
    for(group_index=0;group_index<stripe_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_group_plane(&stripe_groups[group_index],codewords[channel],
                channel);
    return 1;
#else
    return direct_scatter_rgb3(codewords);
#endif
}

static int direct_scatter_rgb3_stripe_optimized(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
#ifdef __WATCOMC__
    u16 run_index,current=0,offset;
    if(!rgb3_active||!placement_entry||!codewords||
       !codewords[VGA_RGB_RED]||!codewords[VGA_RGB_GREEN]||
       !codewords[VGA_RGB_BLUE])return 0;
    if(!stripe_runs||!stripe_run_count)
        return direct_scatter_rgb3_optimized(codewords);
    offset=FP_OFF(codewords[VGA_RGB_RED]);
    if(FP_OFF(codewords[VGA_RGB_GREEN])!=offset||
       FP_OFF(codewords[VGA_RGB_BLUE])!=offset||
       (u32)offset+DOSFER_QR_CODEWORDS>0xFFFFUL)
        return direct_scatter_rgb3_optimized(codewords);
    if(stripe_phase_group_count)
        return direct_scatter_rgb3_phased_optimized(codewords);
    if(stripe_group_count)
        return direct_scatter_rgb3_grouped_optimized(codewords);
    direct_asm_red=codewords[VGA_RGB_RED];
    direct_asm_green=codewords[VGA_RGB_GREEN];
    direct_asm_blue=codewords[VGA_RGB_BLUE];
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(run->first>current)
            direct_scatter_rgb3_range(codewords,current,
                (u16)(run->first-current));
        stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
        stripe_asm_pair_mask=run->pair_mask;
        directStripe386((u16)(offset+run->first),run->count,run->dest,
            FP_OFF(run->pair_mask));
        current=(u16)(run->first+run->count);
    }
    if(current<DOSFER_QR_CODEWORDS)
        direct_scatter_rgb3_range(codewords,current,
            (u16)(DOSFER_QR_CODEWORDS-current));
    return 1;
#else
    return direct_scatter_rgb3(codewords);
#endif
}

#ifdef DOSFER_DIRECT_BENCH
int vga_rgb3_direct_scatter_asm(const u8 *const codewords[VGA_RGB_CHANNELS]) {
    return direct_scatter_rgb3_optimized(codewords);
}

int vga_rgb3_direct_scatter_stripe(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 saved_group_count=stripe_group_count;
    u16 saved_phase_group_count=stripe_phase_group_count;
    int result;
    stripe_group_count=0;
    stripe_phase_group_count=0;
    result=direct_scatter_rgb3_stripe_optimized(codewords);
    stripe_group_count=saved_group_count;
    stripe_phase_group_count=saved_phase_group_count;
    return result;
}

void vga_rgb3_stripe_stats(u16 *runs,u16 *covered) {
    if(runs)*runs=stripe_run_count;
    if(covered)*covered=stripe_codewords_covered;
}

int vga_rgb3_stripe_run_info(u16 index,u16 *first,u16 *count,u16 *top,
        u16 *bottom,u8 *mask,signed char *direction,u8 *group_flags) {
    VgaStripeRun far *run;
    if(index>=stripe_run_count)return 0;
    run=&stripe_runs[index];
    if(first)*first=run->first;
    if(count)*count=run->count;
    if(top)*top=stripe_run_top(run);
    if(bottom)*bottom=stripe_run_bottom(run);
    if(mask)*mask=run->pair_mask[3];
    if(direction)*direction=run->direction;
    if(group_flags)*group_flags=run->group_flags;
    return 1;
}

static int benchmark_stripe_sources(
        const u8 *const codewords[VGA_RGB_CHANNELS],u16 *offset) {
    if(!rgb3_active||!placement_entry||!stripe_runs||!stripe_run_count||
       !codewords||!codewords[VGA_RGB_RED]||!codewords[VGA_RGB_GREEN]||
       !codewords[VGA_RGB_BLUE])return 0;
    *offset=FP_OFF(codewords[VGA_RGB_RED]);
    if(FP_OFF(codewords[VGA_RGB_GREEN])!=*offset||
       FP_OFF(codewords[VGA_RGB_BLUE])!=*offset||
       (u32)*offset+DOSFER_QR_CODEWORDS>0xFFFFUL)return 0;
    direct_asm_red=codewords[VGA_RGB_RED];
    direct_asm_green=codewords[VGA_RGB_GREEN];
    direct_asm_blue=codewords[VGA_RGB_BLUE];
    return 1;
}

int vga_rgb3_direct_scatter_fallback_asm(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 run_index,current=0,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(run->first>current)
            direct_scatter_rgb3_range_optimized(codewords,offset,current,
                (u16)(run->first-current));
        current=(u16)(run->first+run->count);
    }
    if(current<DOSFER_QR_CODEWORDS)
        direct_scatter_rgb3_range_optimized(codewords,offset,current,
            (u16)(DOSFER_QR_CODEWORDS-current));
    return 1;
}

int vga_rgb3_direct_scatter_range_asm_full(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
#ifdef __WATCOMC__
    u16 run_index,group_index,channel,offset,current=0;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(run->first>current)
            direct_scatter_rgb3_range_optimized(codewords,offset,current,
                (u16)(run->first-current));
        if(!run->group_flags) {
            stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
            stripe_asm_pair_mask=run->pair_mask;
            directStripe386((u16)(offset+run->first),run->count,run->dest,
                FP_OFF(run->pair_mask));
        }
        current=(u16)(run->first+run->count);
    }
    if(current<DOSFER_QR_CODEWORDS)
        direct_scatter_rgb3_range_optimized(codewords,offset,current,
            (u16)(DOSFER_QR_CODEWORDS-current));
    for(group_index=0;group_index<stripe_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_group_plane(&stripe_groups[group_index],codewords[channel],
                channel);
    for(group_index=0;group_index<stripe_phase_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_phase_group_plane(&stripe_phase_groups[group_index],
                codewords[channel],channel);
    scatter_phase_edges(codewords);
    for(group_index=0;group_index<stripe_triple_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_triple_plane(&stripe_triples[group_index],
                codewords[channel],channel);
    return 1;
#else
    return direct_scatter_rgb3(codewords);
#endif
}

void vga_rgb3_stripe_triple_stats(u16 *groups,u16 *source_codewords,
        u16 *destination_bytes) {
    if(groups)*groups=stripe_triple_count;
    if(source_codewords)*source_codewords=stripe_triple_codewords;
    if(destination_bytes)*destination_bytes=(u16)(stripe_triple_codewords/3u*4u);
}


int vga_rgb3_direct_scatter_triple_baseline(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 run_index,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(!(run->group_flags&VGA_STRIPE_GROUP_TRIPLE))continue;
        stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
        stripe_asm_pair_mask=run->pair_mask;
        directStripe386((u16)(offset+run->first),run->count,run->dest,
            FP_OFF(run->pair_mask));
    }
    return 1;
}

int vga_rgb3_direct_scatter_triples(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 group_index,channel,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    (void)offset;
    for(group_index=0;group_index<stripe_triple_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_triple_plane(&stripe_triples[group_index],
                codewords[channel],channel);
    return 1;
}


static int direct_scatter_rgb3_phase_control(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
#ifdef __WATCOMC__
    u16 run_index,group_index,channel,offset,current=0;
    if(!benchmark_stripe_sources(codewords,&offset)||!stripe_triple_count)
        return 0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(run->first>current)
            direct_scatter_rgb3_range(codewords,current,
                (u16)(run->first-current));
        if(!(run->group_flags&(VGA_STRIPE_GROUP_EXACT|
                               VGA_STRIPE_GROUP_PHASE))) {
            stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
            stripe_asm_pair_mask=run->pair_mask;
            directStripe386((u16)(offset+run->first),run->count,run->dest,
                FP_OFF(run->pair_mask));
        }
        current=(u16)(run->first+run->count);
    }
    if(current<DOSFER_QR_CODEWORDS)
        direct_scatter_rgb3_range(codewords,current,
            (u16)(DOSFER_QR_CODEWORDS-current));
    for(group_index=0;group_index<stripe_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_group_plane(&stripe_groups[group_index],codewords[channel],
                channel);
    for(group_index=0;group_index<stripe_phase_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_phase_group_plane(&stripe_phase_groups[group_index],
                codewords[channel],channel);
    scatter_phase_edges(codewords);
    return 1;
#else
    return direct_scatter_rgb3(codewords);
#endif
}

int vga_rgb3_direct_scatter_stripe_runs(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 run_index,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
        stripe_asm_pair_mask=run->pair_mask;
        directStripe386((u16)(offset+run->first),run->count,run->dest,
            FP_OFF(run->pair_mask));
    }
    return 1;
}

int vga_rgb3_direct_scatter_stripe_fallback(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 run_index,current=0,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    (void)offset;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(run->first>current)
            direct_scatter_rgb3_range(codewords,current,
                (u16)(run->first-current));
        current=(u16)(run->first+run->count);
    }
    if(current<DOSFER_QR_CODEWORDS)
        direct_scatter_rgb3_range(codewords,current,
            (u16)(DOSFER_QR_CODEWORDS-current));
    return 1;
}

int vga_rgb3_direct_scatter_stripe_setup(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 run_index,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
        stripe_asm_pair_mask=run->pair_mask;
        directStripe386((u16)(offset+run->first),0,run->dest,
            FP_OFF(run->pair_mask));
    }
    return 1;
}

void vga_rgb3_stripe_geometry_stats(u16 run_count[4],u16 codeword_count[4],
        u16 *other_runs,u16 *other_codewords) {
    static const u8 geometry_mask[4]={0xC0,0x30,0x0C,0x03};
    u16 run_index;
    int geometry;
    for(geometry=0;geometry<4;++geometry)
        run_count[geometry]=codeword_count[geometry]=0;
    if(other_runs)*other_runs=0;
    if(other_codewords)*other_codewords=0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        for(geometry=0;geometry<4;++geometry)
            if(run->pair_mask[3]==geometry_mask[geometry])break;
        if(geometry<4) {
            ++run_count[geometry];
            codeword_count[geometry]=(u16)(codeword_count[geometry]+run->count);
        } else {
            if(other_runs)++*other_runs;
            if(other_codewords)*other_codewords=(u16)(*other_codewords+run->count);
        }
    }
}

void vga_rgb3_stripe_group_stats(u16 *groups,u16 *source_codewords,
        u16 *destination_bytes) {
    if(groups)*groups=stripe_group_count;
    if(source_codewords)*source_codewords=stripe_group_codewords;
    if(destination_bytes)*destination_bytes=stripe_group_codewords;
}

void vga_rgb3_stripe_phase_stats(u16 *groups,u16 *source_codewords,
        u16 *destination_bytes,u16 *edge_contributions) {
    if(groups)*groups=stripe_phase_group_count;
    if(source_codewords)*source_codewords=stripe_phase_codewords;
    if(destination_bytes)*destination_bytes=stripe_phase_destination_bytes;
    if(edge_contributions)*edge_contributions=stripe_phase_edge_count;
}

int vga_rgb3_direct_scatter_group_baseline(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 group_index,run_index,slot,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    for(group_index=0;group_index<stripe_group_count;++group_index) {
        VgaStripeGroup *group=&stripe_groups[group_index];
        for(slot=0;slot<4;++slot)for(run_index=0;
                run_index<stripe_run_count;++run_index) {
            VgaStripeRun far *run=&stripe_runs[run_index];
            if(run->first!=group->first[slot])continue;
            stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
            stripe_asm_pair_mask=run->pair_mask;
            directStripe386((u16)(offset+run->first),run->count,run->dest,
                FP_OFF(run->pair_mask));
            break;
        }
    }
    return 1;
}

int vga_rgb3_direct_scatter_groups(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 group_index,channel,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    (void)offset;
    for(group_index=0;group_index<stripe_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_group_plane(&stripe_groups[group_index],
                codewords[channel],channel);
    return 1;
}

int vga_rgb3_direct_scatter_grouped(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    return direct_scatter_rgb3_grouped_optimized(codewords);
}

int vga_rgb3_direct_scatter_phase_baseline(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 run_index,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    for(run_index=0;run_index<stripe_run_count;++run_index) {
        VgaStripeRun far *run=&stripe_runs[run_index];
        if(!(run->group_flags&VGA_STRIPE_GROUP_PHASE))continue;
        stripe_asm_step=(u16)(run->direction*VGA_BYTES_PER_LINE);
        stripe_asm_pair_mask=run->pair_mask;
        directStripe386((u16)(offset+run->first),run->count,run->dest,
            FP_OFF(run->pair_mask));
    }
    return 1;
}

int vga_rgb3_direct_scatter_phase_groups(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 group_index,channel,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    (void)offset;
    for(group_index=0;group_index<stripe_phase_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_phase_group_plane(&stripe_phase_groups[group_index],
                codewords[channel],channel);
    scatter_phase_edges(codewords);
    return 1;
}

int vga_rgb3_direct_scatter_phase_kernel(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 group_index,channel,offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    (void)offset;
    for(group_index=0;group_index<stripe_phase_group_count;++group_index)
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            stripe_phase_group_plane(&stripe_phase_groups[group_index],
                codewords[channel],channel);
    return 1;
}

int vga_rgb3_direct_scatter_phase_edges(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    u16 offset;
    if(!benchmark_stripe_sources(codewords,&offset))return 0;
    (void)offset;
    scatter_phase_edges(codewords);
    return 1;
}

int vga_rgb3_direct_scatter_phased(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    return direct_scatter_rgb3_phase_control(codewords);
}

int vga_rgb3_direct_scatter_with_triples(
        const u8 *const codewords[VGA_RGB_CHANNELS]) {
    return direct_scatter_rgb3_phased_optimized(codewords);
}

#endif

#ifdef DOSFER_MAP_STATS
void vga_rgb3_map_stats_reset(void) {
    rgb_map_loads=rgb_red_xors=rgb_green_xors=rgb_blue_xors=0;
}

void vga_rgb3_map_stats(u32 *map_loads,u32 *red_xors,u32 *green_xors,
                        u32 *blue_xors) {
    if(map_loads)*map_loads=rgb_map_loads;
    if(red_xors)*red_xors=rgb_red_xors;
    if(green_xors)*green_xors=rgb_green_xors;
    if(blue_xors)*blue_xors=rgb_blue_xors;
}

/* Count distinct shadow-raster bytes within each eight-module codeword. A
 * duplicate is a byte-level RMW that a dense renderer could potentially
 * combine after accumulating per-plane XOR masks. */
void vga_rgb3_map_layout_stats(u32 *entries,u32 *within_codeword_bytes,
                               u32 *global_unique_bytes) {
    u16 codeword,bit,prior,offset;
    static u8 seen[VGA_VISIBLE_BYTES];
    u32 total_entries=0,total_within_codeword=0,total_global=0;
    if(!placement_entry) {
        if(entries)*entries=0;
        if(within_codeword_bytes)*within_codeword_bytes=0;
        if(global_unique_bytes)*global_unique_bytes=0;
        return;
    }
    memset(seen,0,sizeof(seen));
    for(codeword=0;codeword<DOSFER_QR_CODEWORDS;++codeword) {
        for(bit=0;bit<8;++bit) {
            offset=(u16)(placement_entry[(u32)codeword*8+bit]&0x1FFF);
            ++total_entries;
            if(!seen[offset]) {seen[offset]=1;++total_global;}
            for(prior=0;prior<bit;++prior)
                if((placement_entry[(u32)codeword*8+prior]&0x1FFF)==offset)break;
            if(prior==bit)++total_within_codeword;
        }
    }
    if(entries)*entries=total_entries;
    if(within_codeword_bytes)*within_codeword_bytes=total_within_codeword;
    if(global_unique_bytes)*global_unique_bytes=total_global;
}
#endif

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
    prepare_placement_map();
    /* Capture is optional: low-memory systems retain the delta/full paths. */
    if(placement_entry)capture_bw_template(codewords);
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
    if(!vga_active||!placement_entry||screen_invert!=invert)return 0;
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
    prepare_placement_map();
    /* Direct rendering is optional on low-memory systems. A failed template
     * allocation leaves the canonical full-render path operational. */
    if(placement_entry)capture_rgb3_template((const u8 *const *)codewords);
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
    if(!vga_active||!rgb3_active||!placement_entry||screen_invert!=invert)return 0;
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

    if(!alternate||!current||!placement_entry){
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
