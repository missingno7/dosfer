#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <malloc.h>
#include <string.h>
#include "vga.h"
#include "qrcodegen.h"
#include "timing.h"

static u8 far *screen;
static u8 far *font8;
static int screen_invert=-1;
static int use_320,active_320,screen_stride=80,screen_height=480,display_page,write_page;
static u8 page_initialized[2],page_invert[2];
static int vga_active;
static u32 screen_bytes=80UL*480UL;
#define QR40_CODEWORDS 3706
#define QR40_DATA_BITS (QR40_CODEWORDS*8)
static u16 far *delta_offset;
static u8 far *delta_mask;
static u8 delta_select[(QR40_DATA_BITS+3)/4];
static u8
#ifdef __WATCOMC__
    __near
#endif
    previous_codewords[QR40_CODEWORDS];
static u8
#ifdef __WATCOMC__
    __near
#endif
    screen_320[8000];
static u16 delta_bits,delta_codewords;
static int delta_n;
static int prepare_delta(const u8 *codewords,u16 codeword_len,int n);
static void update_delta(const u8 *codewords,u8 far *pixels);
#ifdef DOSFER_PROFILE
/* render, VGA upload/setup, retrace wait, page flip, BIOS status drawing */
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

static void dosferDelta386(const u8 far *codewords,u16 len,u8 *previous,
        const u16 far *offsets,const u8 *selectors,u8 far *pixels);
#pragma aux dosferDelta386 = \
    "push bp" \
    "movzx edx,dx" \
    "movzx ecx,cx" \
    "shl ecx,16" \
    "test ecx,ecx" \
    "jnz short delta_begin" \
    "jmp delta_done" \
    "delta_begin:" \
    "mov bp,dx" \
    "movzx ebp,bp" \
    "shl ebp,16" \
    "delta_loop:" \
    "mov al,es:[si]" \
    "xchg al,[di]" \
    "xor al,es:[si]" \
    "inc si" \
    "inc di" \
    "ror ebp,16" \
    "mov ah,[bp]" \
    "inc bp" \
    "rol ebp,16" \
    "test al,80h" \
    "jz short d0" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d0: shr ah,2" \
    "add bx,2" \
    "test al,40h" \
    "jz short d1" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d1: shr ah,2" \
    "add bx,2" \
    "test al,20h" \
    "jz short d2" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d2: shr ah,2" \
    "add bx,2" \
    "test al,10h" \
    "jz short d3" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d3: add bx,2" \
    "ror ebp,16" \
    "mov ah,[bp]" \
    "inc bp" \
    "rol ebp,16" \
    "test al,08h" \
    "jz short d4" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d4: shr ah,2" \
    "add bx,2" \
    "test al,04h" \
    "jz short d5" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d5: shr ah,2" \
    "add bx,2" \
    "test al,02h" \
    "jz short d6" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d6: shr ah,2" \
    "add bx,2" \
    "test al,01h" \
    "jz short d7" \
    "mov bp,fs:[bx]" \
    "mov ch,0c0h" \
    "mov cl,ah" \
    "and cl,3" \
    "shl cl,1" \
    "shr ch,cl" \
    "xor gs:[bp],ch" \
    "xor gs:[bp+80],ch" \
    "d7: add bx,2" \
    "sub ecx,10000h" \
    "test ecx,0ffff0000h" \
    "jz short delta_done" \
    "jmp delta_loop" \
    "delta_done:" \
    "pop bp" \
    parm [es si] [cx] [di] [fs bx] [dx] [gs ax] \
    modify [ax bx cx si di];

/* 320x200 entries pack the 13-bit byte offset and 3-bit pixel selector.
 * Keeping the complete entry in one far array leaves GS available for the
 * shadow page, and avoids two far-table reads for every changed module. */
static void dosferDelta320(const u8 far *codewords,u16 len,u8 *previous,
        const u16 far *entries,u8 far *pixels);
#pragma aux dosferDelta320 = \
    "push bp" \
    "movzx ecx,cx" \
    "shl ecx,16" \
    "movzx edx,dx" \
    "shl edx,16" \
    "test ecx,ecx" \
    "jnz short d320_begin" \
    "jmp d320_done" \
    "d320_begin:" \
    "d320_loop:" \
    "mov al,es:[si]" \
    "xchg al,[di]" \
    "xor al,es:[si]" \
    "inc si" \
    "inc di" \
    "test al,80h" \
    "jz short d320_0" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_0: add bx,2" \
    "test al,40h" \
    "jz short d320_1" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_1: add bx,2" \
    "test al,20h" \
    "jz short d320_2" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_2: add bx,2" \
    "test al,10h" \
    "jz short d320_3" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_3: add bx,2" \
    "test al,08h" \
    "jz short d320_4" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_4: add bx,2" \
    "test al,04h" \
    "jz short d320_5" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_5: add bx,2" \
    "test al,02h" \
    "jz short d320_6" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_6: add bx,2" \
    "test al,01h" \
    "jz short d320_7" \
    "mov bp,fs:[bx]" \
    "mov dx,bp" \
    "shr dx,13" \
    "and bp,1fffh" \
    "mov cl,dl" \
    "mov dh,80h" \
    "shr dh,cl" \
    "ror edx,16" \
    "add bp,dx" \
    "rol edx,16" \
    "xor gs:[bp],dh" \
    "d320_7: add bx,2" \
    "sub ecx,10000h" \
    "test ecx,0ffff0000h" \
    "jnz d320_loop" \
    "d320_done:" \
    "pop bp" \
    parm [es si] [cx] [di] [fs bx] [gs dx] \
    modify [ax bx cx dx si di];

static void dosferCopyNear320(const u8 far *src,u8 far *dest);
#pragma aux dosferCopyNear320 = \
    "mov cx,2000" \
    "copy320_loop:" \
    "mov eax,fs:[si]" \
    "mov es:[di],eax" \
    "add si,4" \
    "add di,4" \
    "loop copy320_loop" \
    parm [fs si] [es di] modify [ax cx si di];

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

static void dosferDelta320Near(const u8 far *codewords,u16 len,u8 __near *previous,
        const u16 far *offsets,const u8 far *masks);
#pragma aux dosferDelta320Near = \
    "push bp" \
    "movzx edx,dx" \
    "test cx,cx" \
    "jnz short dn_begin" \
    "jmp dn_done" \
    "dn_begin:" \
    "dn_loop:" \
    "mov al,es:[si]" \
    "xchg al,[di]" \
    "xor al,es:[si]" \
    "inc si" \
    "inc di" \
    "test al,80h" \
    "jz short dn0" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn0: add bx,2" \
    "inc dx" \
    "test al,40h" \
    "jz short dn1" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn1: add bx,2" \
    "inc dx" \
    "test al,20h" \
    "jz short dn2" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn2: add bx,2" \
    "inc dx" \
    "test al,10h" \
    "jz short dn3" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn3: add bx,2" \
    "inc dx" \
    "test al,08h" \
    "jz short dn4" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn4: add bx,2" \
    "inc dx" \
    "test al,04h" \
    "jz short dn5" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn5: add bx,2" \
    "inc dx" \
    "test al,02h" \
    "jz short dn6" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn6: add bx,2" \
    "inc dx" \
    "test al,01h" \
    "jz short dn7" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor ds:[bp],ah" \
    "dn7: add bx,2" \
    "inc dx" \
    "dec cx" \
    "jz short dn_done" \
    "jmp dn_loop" \
    "dn_done:" \
    "pop bp" \
    parm [es si] [cx] [di] [fs bx] [gs dx] modify [ax bx cx dx si di];

#endif

static void free_delta(void) {
    if(delta_offset)_ffree(delta_offset);
    if(delta_mask)_ffree(delta_mask);
    delta_offset=0;delta_mask=0;delta_bits=delta_codewords=0;delta_n=0;
}

static void bios_mode(u8 mode) {
    union REGS r; r.h.ah=0; r.h.al=mode; int86(0x10,&r,&r);
}
int vga_enter(void) {
    free_delta();
    active_320=use_320;screen_stride=active_320?40:80;screen_height=active_320?200:480;screen_bytes=(u32)screen_stride*screen_height;
    screen=active_320?0:(u8 far *)_fmalloc(screen_bytes);
    if (!active_320&&!screen) return 0;
    bios_mode(active_320?0x0D:0x12);vga_active=1;display_page=0;write_page=active_320?1:0;screen_invert=-1;font8=0;
    page_initialized[0]=page_initialized[1]=0;
    if(active_320)font8=dosferFont8();
    return 1;
}
void vga_use_320(int enabled){use_320=enabled!=0;}
void vga_leave(void) {
    free_delta();if(screen){_ffree(screen);screen=0;}screen_invert=-1;
    if(vga_active){bios_mode(3);bios_mode(3);vga_active=0;}
}
void vga_wait_retrace(void) {
    while (inp(0x3DA)&8) ;
    while (!(inp(0x3DA)&8)) ;
}
static void bit_set(int x,int y,int white) {
    u8 far *p=screen+(u32)y*screen_stride+(x>>3); u8 m=(u8)(0x80>>(x&7));
    if(white) *p|=m; else *p&=(u8)~m;
}
static void fill_rect(int x,int y,int w,int h,int white) {
    int xx,yy;
    for(yy=y;yy<y+h;++yy) for(xx=x;xx<x+w;++xx) bit_set(xx,yy,white);
}
static void status_text(int row,const char *s) {
    union REGS r; int i;
    r.h.ah=2;r.h.bh=(u8)display_page;r.h.dh=(u8)row;r.h.dl=0;int86(0x10,&r,&r);
    for(i=0;s[i]&&i<(active_320?40:76);++i){r.h.ah=0x0E;r.h.al=s[i];r.h.bh=(u8)display_page;r.h.bl=15;int86(0x10,&r,&r);}
}
static void status_text_320(const char *s) {
    int x,y;u8 far *glyph;u8 bg=screen_invert?0x00:0xFF;
    for(y=192;y<200;++y)memset(screen_320+y*40,bg,40);
    if(!font8)return;
    for(x=0;s[x]&&x<40;++x){glyph=font8+(u16)(u8)s[x]*8;
        for(y=0;y<8;++y)screen_320[(192+y)*40+x]=screen_invert?glyph[y]:(u8)~glyph[y];}
}
static int build_qr_image(const u8 *qr,int n,int scale,int invert,int marker) {
    int total=(n+8)*scale,x0=(640-total)/2,y0=8,mx,my,xx,yy,dark,index,px;
    u8 mask;
    u8 row[80];
    if(!screen || total>440) return 0;
    if(screen_invert!=invert){_fmemset(screen,invert?0x00:0xFF,80UL*480UL);screen_invert=invert;}
    if(scale==2||scale==4) {
        int per_byte=8/scale,data_x=((x0+4*scale+4)/8)*8,data_byte=data_x>>3;
        int row_bytes=(n+per_byte-1)/per_byte,remain,byte_pos;
        static const u8 expand2[16]={0xFF,0x3F,0xCF,0x0F,0xF3,0x33,0xC3,0x03,0xFC,0x3C,0xCC,0x0C,0xF0,0x30,0xC0,0x00};
        static const u8 expand4[4]={0xFF,0x0F,0xF0,0x00};
        u8 packed[52],pattern;
        u16 word;
        x0=data_x-4*scale;
        for(my=0;my<n;++my) {
            index=my*n;
            for(mx=0;mx<n;mx+=per_byte) {
                byte_pos=(index>>3)+1;word=qr[byte_pos]|((u16)qr[byte_pos+1]<<8);
                pattern=(u8)((word>>(index&7))&((1<<per_byte)-1));
                remain=n-mx;if(remain<per_byte)pattern&=(u8)((1<<remain)-1);
                packed[mx/per_byte]=scale==2?expand2[pattern]:expand4[pattern];
                if(invert)packed[mx/per_byte]=(u8)~packed[mx/per_byte];
                index+=per_byte;
            }
            for(yy=0;yy<scale;++yy)
                _fmemcpy(screen+(u32)(y0+(my+4)*scale+yy)*80UL+data_byte,packed,(size_t)row_bytes);
        }
        fill_rect(x0-12,y0+total+4,24,4,!invert);
        fill_rect(x0+total-12,y0+total+4,24,4,!invert);
        fill_rect(marker?x0-12:x0+total-12,y0+total+4,24,4,invert);
        return 1;
    }
    _fmemset(screen,invert?0x00:0xFF,80UL*480UL);
    for(my=0;my<n;++my) {
        memset(row,invert?0x00:0xFF,sizeof(row));
        index=my*n;
        for(mx=0;mx<n;++mx) {
            dark=(qr[(index>>3)+1]>>(index&7))&1;index++;
            if(dark)for(xx=0;xx<scale;++xx){px=x0+(mx+4)*scale+xx;mask=(u8)(0x80>>(px&7));if(invert)row[px>>3]|=mask;else row[px>>3]&=(u8)~mask;}
        }
        for(yy=0;yy<scale;++yy)
            _fmemcpy(screen+(u32)(y0+(my+4)*scale+yy)*80UL,row,80);
    }
    fill_rect(x0-12,y0+total+4,24,4,marker?0:1);
    fill_rect(x0+total-12,y0+total+4,24,4,marker?0:1);
    return 1;
}
static int build_qr_image_320(const u8 *qr,int n,int invert) {
    int total=n+8,x0=(320-total)/2,my,mx,index,dark,px;u8 mask;
    if(total>192)return 0;memset(screen_320,invert?0x00:0xFF,sizeof(screen_320));screen_invert=invert;
    for(my=0;my<n;++my){index=my*n;for(mx=0;mx<n;++mx){dark=(qr[(index>>3)+1]>>(index&7))&1;index++;
        if(dark){px=x0+4+mx;mask=(u8)(0x80>>(px&7));if(invert)screen_320[(4+my)*40+(px>>3)]|=mask;else screen_320[(4+my)*40+(px>>3)]&=(u8)~mask;}}}
    return 1;
}
static void copy_qr_image(void) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);
    vga_wait_retrace();
    outp(0x3C4,2); outp(0x3C5,0x0F);
    outp(0x3CE,0); outp(0x3CF,0);
    outp(0x3CE,1); outp(0x3CF,0);
    outp(0x3CE,3); outp(0x3CF,0);
    outp(0x3CE,5); outp(0x3CF,0);
    outp(0x3CE,8); outp(0x3CF,0xFF);
    _fmemcpy(vram,screen,80UL*480UL);
}
static void copy_qr_delta_image(int n,int scale) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);int total=(n+8)*scale,x0=(640-total)/2,y0=8;
    int data_x=((x0+4*scale+4)/8)*8,start,end,row,width;
    x0=data_x-4*scale;start=(x0-12)>>3;end=(x0+total+12+7)>>3;width=end-start;
    vga_wait_retrace();outp(0x3C4,2);outp(0x3C5,0x0F);outp(0x3CE,0);outp(0x3CF,0);
    outp(0x3CE,1);outp(0x3CF,0);outp(0x3CE,3);outp(0x3CF,0);outp(0x3CE,5);outp(0x3CF,0);outp(0x3CE,8);outp(0x3CF,0xFF);
    for(row=y0;row<y0+total+8;++row)_fmemcpy(vram+(u32)row*80UL+start,screen+(u32)row*80UL+start,(size_t)width);
}
static void copy_320_flip(void) {
    /* Mode 0Dh displays 8000 bytes, but the BIOS page stride is rounded to
     * 0x2000. Uploading page 1 at 8000 made alternate frames start 192 bytes
     * before the address selected by INT 10h/AH=05. */
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);union REGS r;u16 base=(u16)(write_page<<13);
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks(),profile_now;
#endif
    outp(0x3C4,2);outp(0x3C5,0x0F);outp(0x3CE,0);outp(0x3CF,0);outp(0x3CE,1);outp(0x3CF,0);
    outp(0x3CE,3);outp(0x3CF,0);outp(0x3CE,5);outp(0x3CF,0);outp(0x3CE,8);outp(0x3CF,0xFF);
    if(!page_initialized[write_page]||page_invert[write_page]!=(u8)screen_invert)
        dosferCopyNear320(screen_320,vram+base);
    else dosferCopyPartial320(screen_320,vram+base);
    page_initialized[write_page]=1;page_invert[write_page]=(u8)screen_invert;
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[1]+=profile_now-profile_start;profile_start=profile_now;
#endif
    vga_wait_retrace();
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[2]+=profile_now-profile_start;profile_start=profile_now;
#endif
    memset(&r,0,sizeof(r));r.h.ah=5;r.h.al=(u8)write_page;int86(0x10,&r,&r);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferVgaProfileTicks[3]+=profile_now-profile_start;
#endif
    display_page=write_page;write_page^=1;
}

/* Mode 0Dh is four independent 64 KiB bitplanes.  An 8 KiB display page
 * leaves exactly eight slots per plane: 4 planes x 8 slots = 32 1-bit frames.
 * The DAC maps the resulting four-bit pixel index to a color, so selecting a
 * source plane needs no VRAM copy. */
static void plane_mode_setup(u8 map_mask) {
    outp(0x3C4,2);outp(0x3C5,map_mask);
    outp(0x3CE,0);outp(0x3CF,0);
    outp(0x3CE,1);outp(0x3CF,0);
    outp(0x3CE,3);outp(0x3CF,0);
    outp(0x3CE,5);outp(0x3CF,0);
    outp(0x3CE,8);outp(0x3CF,0xFF);
}
static void plane_palette(u8 select_mask,int parity) {
    u8 i,v,bits;
    outp(0x3C8,0);
    for(i=0;i<16;++i) {
        bits=(u8)(i&select_mask);
        v=parity?(u8)((bits^(bits>>1)^(bits>>2)^(bits>>3))&1):(u8)(bits!=0);
        v=v?63:0;outp(0x3C9,v);outp(0x3C9,v);outp(0x3C9,v);
    }
}
static void plane_palette_normal(void) {
    u8 i,v;outp(0x3C8,0);
    for(i=0;i<16;++i){v=i==15?63:0;outp(0x3C9,v);outp(0x3C9,v);outp(0x3C9,v);}
}
static u16 crtc_start(void) {
    u8 hi,lo;
    outp(0x3D4,0x0C);hi=inp(0x3D5);
    outp(0x3D4,0x0D);lo=inp(0x3D5);
    return (u16)(((u16)hi<<8)|lo);
}
static void crtc_set_start(u16 start) {
    outp(0x3D4,0x0C);outp(0x3D5,(u8)(start>>8));
    outp(0x3D4,0x0D);outp(0x3D5,(u8)start);
}
static u16 plane_page_step(void) {
    union REGS r;u16 first,second;
    memset(&r,0,sizeof(r));r.h.ah=5;r.h.al=0;int86(0x10,&r,&r);first=crtc_start();
    memset(&r,0,sizeof(r));r.h.ah=5;r.h.al=1;int86(0x10,&r,&r);second=crtc_start();
    memset(&r,0,sizeof(r));r.h.ah=5;r.h.al=0;int86(0x10,&r,&r);
    return (u16)(second-first);
}
static void attribute_plane_enable(u8 mask);
static void plane_show(u16 start,u8 plane) {
    crtc_set_start(start);
    attribute_plane_enable((u8)(1U<<plane));
}
static void attribute_plane_enable(u8 mask) {
    /* Attribute Controller index 12h masks plane bits before palette lookup.
     * Bit 5 in the address write keeps video output enabled. */
    inp(0x3DA);outp(0x3C0,0x32);outp(0x3C0,mask);outp(0x3C0,0x20);
}
static u8 attribute_plane_enable_read(void) {
    inp(0x3DA);outp(0x3C0,0x32);return inp(0x3C1);
}
int vga_verify_plane_xor3(const u8 *expected_qr,int qr_size,int invert,
                           u8 plane_mask,int *color_plane_enable_ok) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);u8 plane,value;
    u16 i;
    *color_plane_enable_ok=0;
    if(!active_320||(plane_mask&0x0F)!=plane_mask)return 0;
    /* Fixed parity palette + Color Plane Enable is the display path under
     * test.  The readback below independently reconstructs the same XOR. */
    plane_palette(0x0F,1);attribute_plane_enable(plane_mask);crtc_set_start(0);
    *color_plane_enable_ok=(attribute_plane_enable_read()&0x0F)==plane_mask;
    if(!build_qr_image_320(expected_qr,qr_size,invert))goto fail;
    for(i=0;i<8000;++i) {
        value=0;
        for(plane=0;plane<4;++plane)if(plane_mask&(1U<<plane)){
            outp(0x3CE,4);outp(0x3CF,plane);value^=vram[i];}
        if(value!=screen_320[i])goto fail;
    }
    attribute_plane_enable(0x0F);plane_palette_normal();plane_mode_setup(0x0F);
    page_initialized[0]=page_initialized[1]=0;return 1;
fail:
    attribute_plane_enable(0x0F);plane_palette_normal();plane_mode_setup(0x0F);
    page_initialized[0]=page_initialized[1]=0;return 0;
}
int vga_verify_plane_xor4_correction(const u8 *expected_qr,const u8 *correction_qr,
                                     int qr_size,int invert,int *restored_ok,
                                     int *color_plane_enable_ok) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);u8 before,after,value,plane;
    u16 i;u32 initial_hash=0,final_hash=0;
    *restored_ok=*color_plane_enable_ok=0;
    if(!active_320)return 0;
    /* Render Q(delta) once for this proof.  Production will use the same
     * sparse correction geometry directly, not a fifth generic QR render. */
    if(!build_qr_image_320(correction_qr,qr_size,invert))return 0;
    outp(0x3CE,4);outp(0x3CF,3);
    for(i=0;i<8000;++i)initial_hash=(initial_hash*33U)^vram[i];
    plane_mode_setup(0x08);outp(0x3CE,4);outp(0x3CF,3);
    for(i=0;i<8000;++i)vram[i]^=screen_320[i];
    plane_palette(0x0F,1);attribute_plane_enable(0x0F);crtc_set_start(0);
    *color_plane_enable_ok=(attribute_plane_enable_read()&0x0F)==0x0F;
    if(!build_qr_image_320(expected_qr,qr_size,invert))goto fail;
    for(i=0;i<8000;++i) {
        value=0;
        for(plane=0;plane<4;++plane){outp(0x3CE,4);outp(0x3CF,plane);value^=vram[i];}
        if(value!=screen_320[i])goto fail;
    }
    if(!build_qr_image_320(correction_qr,qr_size,invert))goto fail;
    plane_mode_setup(0x08);outp(0x3CE,4);outp(0x3CF,3);
    for(i=0;i<8000;++i)vram[i]^=screen_320[i];
    outp(0x3CE,4);outp(0x3CF,3);
    for(i=0;i<8000;++i)final_hash=(final_hash*33U)^vram[i];
    *restored_ok=initial_hash==final_hash;
    attribute_plane_enable(0x0F);plane_palette_normal();plane_mode_setup(0x0F);
    page_initialized[0]=page_initialized[1]=0;return *restored_ok;
fail:
    attribute_plane_enable(0x0F);plane_palette_normal();plane_mode_setup(0x0F);
    page_initialized[0]=page_initialized[1]=0;return 0;
}
int vga_verify_affine_correction(const u8 *zero_qr,const u8 *zero_codewords,
                                 const u8 *correction_codewords,const u8 *expected_qr,
                                 u16 codeword_len,int qr_size,int invert,u16 *changed_codewords) {
    u16 i;u32 got=0,want=0;
    *changed_codewords=0;
    if(!active_320||codeword_len>QR40_CODEWORDS)return 0;
    for(i=0;i<codeword_len;++i)if(zero_codewords[i]!=correction_codewords[i])++*changed_codewords;
    if(!build_qr_image_320(zero_qr,qr_size,invert)||
       !prepare_delta(zero_codewords,codeword_len,qr_size))return 0;
    update_delta(correction_codewords,screen_320);
    for(i=0;i<8000;++i)got=(got*33U)^screen_320[i];
    if(!build_qr_image_320(expected_qr,qr_size,invert))return 0;
    for(i=0;i<8000;++i)want=(want*33U)^screen_320[i];
    return got==want;
}
void vga_benchmark_planes(u32 *store_ms,u32 *burst_ms,u32 *vblank_ms,
                          u16 *page_step,int *verified) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);u8 plane,slot,round;
    u16 i,start;u32 a,b;
    *store_ms=*burst_ms=*vblank_ms=0;*page_step=0;*verified=0;
    if(!active_320)return;
    *page_step=plane_page_step();
    if(!*page_step)return;
    /* Stage a complete 32-frame window without consuming conventional RAM.
     * The same canonical screen is sufficient to measure real write traffic;
     * the readback loop checks every plane and every page slot. */
    a=timer_ticks();
    for(plane=0;plane<4;++plane){plane_mode_setup((u8)(1U<<plane));
        for(slot=0;slot<8;++slot)_fmemcpy(vram+((u16)slot<<13),screen_320,8000);}
    b=timer_ticks();*store_ms=timer_elapsed_ms(a,b);
    for(plane=0;plane<4;++plane){outp(0x3CE,4);outp(0x3CF,plane);
        for(slot=0;slot<8;++slot)for(i=0;i<8000;++i)
            if(vram[((u16)slot<<13)+i]!=screen_320[i])goto done;}
    *verified=1;
    /* CRTC start selects one of eight page slots; palette selection selects
     * one of four stored frames in that slot.  Repeat so the DOS timer can
     * measure controller-only burst rate with useful resolution. */
    plane_palette(0x0F,1);attribute_plane_enable(0x01);
    a=timer_ticks();for(round=0;round<24;++round)for(slot=0;slot<8;++slot)
        for(plane=0;plane<4;++plane)plane_show((u16)(slot*(*page_step)),plane);
    b=timer_ticks();*burst_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(slot=0;slot<8;++slot)for(plane=0;plane<4;++plane){
        vga_wait_retrace();plane_show((u16)(slot*(*page_step)),plane);}
    b=timer_ticks();*vblank_ms=timer_elapsed_ms(a,b);
done:
    plane_palette_normal();plane_mode_setup(0x0F);crtc_set_start(0);
    page_initialized[0]=page_initialized[1]=0;
}
int vga_benchmark_plane_batch4(const u8 *first_qr,const u8 *codewords,
                                u16 codeword_len,int qr_size,int invert,
                                u32 *render_ms,u32 *upload_ms,
                                u32 *playback_ms,int *verified) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);u8 plane;
    u16 i;u32 a,b;
    *render_ms=*upload_ms=*playback_ms=0;*verified=0;
    if(!active_320||codeword_len>QR40_CODEWORDS)return 0;
    /* This is the honest four-frame staging baseline.  A is built once from
     * the fixed QR template; B/C/D reuse the prepared placement map and
     * advance the same RAM delta renderer before each plane upload. */
    a=timer_ticks();
    if(!build_qr_image_320(first_qr,qr_size,invert)||
       !prepare_delta(codewords,codeword_len,qr_size))return 0;
    b=timer_ticks();*render_ms=timer_elapsed_ms(a,b);
    for(plane=0;plane<4;++plane) {
        if(plane) {a=timer_ticks();update_delta(codewords+(u32)plane*codeword_len,screen_320);
            b=timer_ticks();*render_ms+=timer_elapsed_ms(a,b);}
        a=timer_ticks();plane_mode_setup((u8)(1U<<plane));
        _fmemcpy(vram,screen_320,8000);b=timer_ticks();*upload_ms+=timer_elapsed_ms(a,b);
        outp(0x3CE,4);outp(0x3CF,plane);
        for(i=0;i<8000;++i)if(vram[i]!=screen_320[i])goto done;
    }
    *verified=1;
    plane_palette(0x0F,1);attribute_plane_enable(0x01);
    a=timer_ticks();for(i=0;i<24;++i)for(plane=0;plane<4;++plane)
        plane_show(0,plane);b=timer_ticks();*playback_ms=timer_elapsed_ms(a,b);
done:
    plane_palette_normal();plane_mode_setup(0x0F);crtc_set_start(0);
    page_initialized[0]=page_initialized[1]=0;
    return *verified;
}
int vga_benchmark_plane_batch4_steady(const u8 *codewords,u16 codeword_len,
                                       u32 *render_ms,u32 *upload_ms,
                                       u32 *playback_ms,int *verified) {
    u8 far *vram=(u8 far *)MK_FP(0xA000,0);u8 plane;
    u16 i;u32 a,b;
    *render_ms=*upload_ms=*playback_ms=0;*verified=0;
    if(!active_320||!delta_offset||codeword_len!=delta_codewords)return 0;
    /* Warm batch: the delta placement map and the preceding frame already
     * exist.  This is the relevant sustained cost, not the cold map build. */
    for(plane=0;plane<4;++plane) {
        a=timer_ticks();update_delta(codewords+(u32)plane*codeword_len,screen_320);
        b=timer_ticks();*render_ms+=timer_elapsed_ms(a,b);
        a=timer_ticks();plane_mode_setup((u8)(1U<<plane));
        _fmemcpy(vram,screen_320,8000);b=timer_ticks();*upload_ms+=timer_elapsed_ms(a,b);
        outp(0x3CE,4);outp(0x3CF,plane);
        for(i=0;i<8000;++i)if(vram[i]!=screen_320[i])goto done;
    }
    *verified=1;
    plane_palette(0x0F,1);attribute_plane_enable(0x01);
    a=timer_ticks();for(i=0;i<24;++i)for(plane=0;plane<4;++plane)
        plane_show(0,plane);b=timer_ticks();*playback_ms=timer_elapsed_ms(a,b);
done:
    plane_palette_normal();plane_mode_setup(0x0F);crtc_set_start(0);
    page_initialized[0]=page_initialized[1]=0;
    return *verified;
}
static int prepare_delta(const u8 *codewords,u16 codeword_len,int n) {
    const u16 *bytes=qrcodegen_dosferPlacementBytes();const u8 *masks=qrcodegen_dosferPlacementMasks();
    int bits=qrcodegen_dosferPlacementBits(),i,bit,x,y;u16 linear,off;u8 m;
    int total=(n+8)*2,x0=(640-total)/2,y0=8,data_x=((x0+12)/8)*8,data_byte=data_x>>3,px;
    if(!bytes||!masks||bits<=0||bits!=(int)codeword_len*8)return 0;
    free_delta();delta_offset=(u16 far *)_fmalloc((u32)bits*sizeof(u16));
    if(active_320)delta_mask=(u8 far *)_fmalloc((u32)bits);
    if(!delta_offset||(active_320&&!delta_mask)||bits>QR40_DATA_BITS||codeword_len>QR40_CODEWORDS){free_delta();return 0;}
    if(!active_320)memset(delta_select,0,(size_t)(bits+3)/4);
    for(i=0;i<bits;++i){m=masks[i];bit=0;while(((u8)1<<bit)!=m)bit++;
        linear=(u16)(((bytes[i]-1)<<3)+bit);y=linear/n;x=linear-y*n;
        if(active_320){px=(320-(n+8))/2+4+x;
            delta_offset[i]=(u16)((y+4)*40+(px>>3));delta_mask[i]=(u8)(0x80>>(px&7));
        }
        else{/* Keep offsets relative: screen[off] already adds the far pointer base. */
            off=(u16)((y0+(y+4)*2)*80+data_byte+(x>>2));delta_offset[i]=off;delta_select[i>>2]|=(u8)((x&3)<<((i&3)*2));}}
    _fmemcpy(previous_codewords,codewords,codeword_len);delta_bits=(u16)bits;
    delta_codewords=codeword_len;delta_n=n;qrcodegen_dosferReleaseMatrixCache();return 1;
}
static void update_delta(const u8 *codewords,u8 far *pixels) {
    if(active_320){const u16 far *offset=delta_offset;
        const u8 far *masks=delta_mask;
        u16 i,off;
        u8 changed;
        (void)pixels;
        /* V40 has an even 3,706 codewords. Dispatching two bytes per loop
           keeps the proven sparse offset/mask layout while halving loop and
           far-pointer update overhead without changing the map layout. */
        for(i=0;i+1<delta_codewords;i+=2,offset+=16,masks+=16){
            changed=(u8)(codewords[i]^previous_codewords[i]);previous_codewords[i]=codewords[i];
            #define DOSFER_TOGGLE_320_2(k,b) if(changed&(b)){off=offset[k];screen_320[off]^=masks[k];}
            DOSFER_TOGGLE_320_2(0,0x80)DOSFER_TOGGLE_320_2(1,0x40)DOSFER_TOGGLE_320_2(2,0x20)DOSFER_TOGGLE_320_2(3,0x10)
            DOSFER_TOGGLE_320_2(4,0x08)DOSFER_TOGGLE_320_2(5,0x04)DOSFER_TOGGLE_320_2(6,0x02)DOSFER_TOGGLE_320_2(7,0x01)
            changed=(u8)(codewords[i+1]^previous_codewords[i+1]);previous_codewords[i+1]=codewords[i+1];
            DOSFER_TOGGLE_320_2(8,0x80)DOSFER_TOGGLE_320_2(9,0x40)DOSFER_TOGGLE_320_2(10,0x20)DOSFER_TOGGLE_320_2(11,0x10)
            DOSFER_TOGGLE_320_2(12,0x08)DOSFER_TOGGLE_320_2(13,0x04)DOSFER_TOGGLE_320_2(14,0x02)DOSFER_TOGGLE_320_2(15,0x01)
            #undef DOSFER_TOGGLE_320_2
        }
        if(i<delta_codewords){changed=(u8)(codewords[i]^previous_codewords[i]);previous_codewords[i]=codewords[i];
            #define DOSFER_TOGGLE_320_TAIL(k,b) if(changed&(b)){off=offset[k];screen_320[off]^=masks[k];}
            DOSFER_TOGGLE_320_TAIL(0,0x80)DOSFER_TOGGLE_320_TAIL(1,0x40)DOSFER_TOGGLE_320_TAIL(2,0x20)DOSFER_TOGGLE_320_TAIL(3,0x10)
            DOSFER_TOGGLE_320_TAIL(4,0x08)DOSFER_TOGGLE_320_TAIL(5,0x04)DOSFER_TOGGLE_320_TAIL(6,0x02)DOSFER_TOGGLE_320_TAIL(7,0x01)
            #undef DOSFER_TOGGLE_320_TAIL
        }
        return;}
#ifdef DOSFER_ASM_DELTA
    dosferDelta386(codewords,delta_codewords,previous_codewords,delta_offset,delta_select,pixels);
#else
    static const u8 pixel_mask[4]={0xC0,0x30,0x0C,0x03};
    const u16 far *offset=delta_offset;const u8 *selectors=delta_select;
    u16 i,off;u8 changed,sel,mask;
    for(i=0;i<delta_codewords;++i,offset+=8,selectors+=2){changed=(u8)(codewords[i]^previous_codewords[i]);previous_codewords[i]=codewords[i];
        sel=selectors[0];
        #define DOSFER_TOGGLE(k,b) if(changed&(b)){off=offset[k];mask=pixel_mask[sel&3];pixels[off]^=mask;pixels[off+80]^=mask;}sel>>=2;
        DOSFER_TOGGLE(0,0x80)DOSFER_TOGGLE(1,0x40)DOSFER_TOGGLE(2,0x20)DOSFER_TOGGLE(3,0x10)
        sel=selectors[1];
        DOSFER_TOGGLE(4,0x08)DOSFER_TOGGLE(5,0x04)DOSFER_TOGGLE(6,0x02)DOSFER_TOGGLE(7,0x01)
        #undef DOSFER_TOGGLE
    }
#endif
}
int vga_delta_ready(void){
    return delta_offset!=0;
}
void vga_delta_stats(u16 *groups,u16 *types,int *rotation) {
    *groups=delta_bits;*types=0;
    *rotation=0;
}
u32 vga_screen_hash(void) {
    u32 i,h=2166136261UL;if(!active_320&&!screen)return 0;
    /* Status text intentionally differs between a streamed frame and its
       canonical redraw; hash the complete QR/quiet-zone area (rows 0..191). */
    if(active_320)for(i=0;i<7680;++i){h^=screen_320[i];h*=16777619UL;}
    else for(i=0;i<screen_bytes;++i){h^=screen[i];h*=16777619UL;}
    return h;
}
int vga_display_matches(void) {
    u8 far *vram;u16 i;
    if(!active_320)return 1;
    outp(0x3CE,4);outp(0x3CF,0); /* Read plane 0; all four QR planes are identical. */
    vram=(u8 far *)MK_FP(0xA000,(u16)(display_page<<13));
    for(i=0;i<8000;++i)if(vram[i]!=screen_320[i])return 0;
    return 1;
}
int vga_show_qr_stream(const u8 *qr,const u8 *codewords,u16 codeword_len,int n,int scale,int invert,const char *a,const char *b,int marker,int delta_only){
    int total=(n+8)*scale,x0=(640-total)/2,y0=8;
#ifdef DOSFER_PROFILE
    u32 profile_start,profile_now;
#endif
    if(active_320){(void)b;(void)marker;if(scale!=1)return 0;
#ifdef DOSFER_PROFILE
        profile_start=timer_ticks();
#endif
        if(delta_only){if(!delta_offset||delta_n!=n||delta_codewords!=codeword_len||screen_invert!=invert)return 0;update_delta(codewords,screen);}
        else{if(!build_qr_image_320(qr,n,invert))return 0;prepare_delta(codewords,codeword_len,n);}
#ifdef DOSFER_PROFILE
        profile_now=timer_ticks();dosferVgaProfileTicks[0]+=profile_now-profile_start;
#endif
#ifdef DOSFER_PROFILE
        profile_start=timer_ticks();
#endif
        status_text_320(a);
#ifdef DOSFER_PROFILE
        profile_now=timer_ticks();dosferVgaProfileTicks[4]+=profile_now-profile_start;
#endif
        copy_320_flip();
        return 1;}
    if(scale==2||scale==4)x0=(((x0+4*scale+4)/8)*8)-4*scale;
    if(delta_only){if(!delta_offset||delta_n!=n||delta_codewords!=codeword_len||screen_invert!=invert)return 0;update_delta(codewords,screen);
        fill_rect(x0-12,y0+total+4,24,4,!invert);fill_rect(x0+total-12,y0+total+4,24,4,!invert);fill_rect(marker?x0-12:x0+total-12,y0+total+4,24,4,invert);
    }else{if(!build_qr_image(qr,n,scale,invert,marker))return 0;if(scale==2)prepare_delta(codewords,codeword_len,n);}
    if(delta_only)copy_qr_delta_image(n,scale);else copy_qr_image();status_text(26,a);status_text(28,b);return 1;
}
int vga_show_qr(const u8 *qr,int n,int scale,int invert,const char *a,const char *b,int marker) {
    if(active_320){(void)b;(void)marker;if(scale!=1||!build_qr_image_320(qr,n,invert))return 0;status_text_320(a);copy_320_flip();return 1;}
    if(!build_qr_image(qr,n,scale,invert,marker))return 0;
    copy_qr_image();
    status_text(26,a); status_text(28,b);
    return 1;
}
void vga_benchmark_qr(const u8 *qr,int n,int scale,int loops,u32 *build_ms,u32 *copy_ms,u32 *text_ms) {
    int i;u32 a,b;
    if(active_320){a=timer_ticks();for(i=0;i<loops;++i)build_qr_image_320(qr,n,0);b=timer_ticks();*build_ms=timer_elapsed_ms(a,b);
        a=timer_ticks();for(i=0;i<loops;++i)copy_320_flip();b=timer_ticks();*copy_ms=timer_elapsed_ms(a,b);
        a=timer_ticks();for(i=0;i<loops;++i)status_text_320("DOSfer 320x200 status benchmark");b=timer_ticks();*text_ms=timer_elapsed_ms(a,b);return;}
    a=timer_ticks();for(i=0;i<loops;++i)build_qr_image(qr,n,scale,0,i&1);b=timer_ticks();*build_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)copy_qr_image();b=timer_ticks();*copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i){status_text(26,"DOSfer VGA status benchmark");status_text(28,"0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ");}b=timer_ticks();*text_ms=timer_elapsed_ms(a,b);
}
void vga_benchmark_delta(const u8 *codewords,u16 codeword_len,int n,int loops,u32 *update_ms,u32 *copy_ms,u32 *text_ms) {
    u8 far *alternate=(u8 far *)_fmalloc(codeword_len);int i;u32 a,b,state=0x6A67C69DUL;
    if(!alternate||!delta_offset){*update_ms=*copy_ms=*text_ms=0;if(alternate)_ffree(alternate);return;}
    /* Representative DATA/XOR traffic changes about half the bits. The old
     * complement pattern changed all eight bits and overstated real redraw
     * cost by roughly 2x. Keep this deterministic for comparisons. */
    for(i=0;i<codeword_len;++i){state^=state<<13;state^=state>>17;state^=state<<5;alternate[i]=(u8)(codewords[i]^(u8)state);}
    a=timer_ticks();for(i=0;i<loops;++i)update_delta((i&1)?codewords:alternate,screen);b=timer_ticks();*update_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i){if(active_320)copy_320_flip();else copy_qr_delta_image(n,2);}b=timer_ticks();*copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i){if(active_320)status_text_320("DOSfer 320 delta benchmark");else{status_text(26,"DOSfer delta status benchmark");status_text(28,"0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ");}}b=timer_ticks();*text_ms=timer_elapsed_ms(a,b);
    _ffree(alternate);
}
void speaker_beep(void) {
    u16 div=1193180UL/880; u8 old=inp(0x61);
    outp(0x43,0xB6); outp(0x42,(u8)div); outp(0x42,(u8)(div>>8));
    outp(0x61,old|3); delay(80); outp(0x61,old);
}
