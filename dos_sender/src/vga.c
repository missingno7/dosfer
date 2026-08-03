#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <malloc.h>
#include <string.h>
#include "vga.h"
#include "qrcodegen.h"
#include "timing.h"

static u8 far *screen;
static int screen_invert=-1;
static int use_320,active_320,screen_stride=80,screen_height=480,display_page,write_page;
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

#ifdef __WATCOMC__
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
    "xor [bp],ah" \
    "dn0: add bx,2" \
    "inc dx" \
    "test al,40h" \
    "jz short dn1" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor [bp],ah" \
    "dn1: add bx,2" \
    "inc dx" \
    "test al,20h" \
    "jz short dn2" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor [bp],ah" \
    "dn2: add bx,2" \
    "inc dx" \
    "test al,10h" \
    "jz short dn3" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor [bp],ah" \
    "dn3: add bx,2" \
    "inc dx" \
    "test al,08h" \
    "jz short dn4" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor [bp],ah" \
    "dn4: add bx,2" \
    "inc dx" \
    "test al,04h" \
    "jz short dn5" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor [bp],ah" \
    "dn5: add bx,2" \
    "inc dx" \
    "test al,02h" \
    "jz short dn6" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor [bp],ah" \
    "dn6: add bx,2" \
    "inc dx" \
    "test al,01h" \
    "jz short dn7" \
    "mov bp,fs:[bx]" \
    "mov ah,gs:[edx]" \
    "xor [bp],ah" \
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
    bios_mode(active_320?0x0D:0x12);display_page=0;write_page=active_320?1:0;screen_invert=-1;return 1;
}
void vga_use_320(int enabled){use_320=enabled!=0;}
void vga_leave(void) {
    free_delta();if(screen){_ffree(screen);screen=0;}screen_invert=-1;
    bios_mode(3);bios_mode(3);
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
    outp(0x3C4,2);outp(0x3C5,0x0F);outp(0x3CE,0);outp(0x3CF,0);outp(0x3CE,1);outp(0x3CF,0);
    outp(0x3CE,3);outp(0x3CF,0);outp(0x3CE,5);outp(0x3CF,0);outp(0x3CE,8);outp(0x3CF,0xFF);
    dosferCopyNear320(screen_320,vram+base);
    vga_wait_retrace();memset(&r,0,sizeof(r));r.h.ah=5;r.h.al=(u8)write_page;int86(0x10,&r,&r);
    display_page=write_page;write_page^=1;
}
static int prepare_delta(const u8 *codewords,u16 codeword_len,int n) {
    const u16 *bytes=qrcodegen_dosferPlacementBytes();const u8 *masks=qrcodegen_dosferPlacementMasks();
    int bits=qrcodegen_dosferPlacementBits(),i,bit,x,y;u16 linear,off;u8 m;
    int total=(n+8)*2,x0=(640-total)/2,y0=8,data_x=((x0+12)/8)*8,data_byte=data_x>>3,px;
    if(!bytes||!masks||bits<=0||bits!=(int)codeword_len*8)return 0;
    free_delta();delta_offset=(u16 far *)_fmalloc((u32)bits*sizeof(u16));
    if(active_320)delta_mask=(u8 far *)_fmalloc((u32)bits);
    if(!delta_offset||(active_320&&!delta_mask)||bits>QR40_DATA_BITS||codeword_len>sizeof(previous_codewords)){free_delta();return 0;}
    if(!active_320)memset(delta_select,0,(size_t)(bits+3)/4);
    for(i=0;i<bits;++i){m=masks[i];bit=0;while(((u8)1<<bit)!=m)bit++;
        linear=(u16)(((bytes[i]-1)<<3)+bit);y=linear/n;x=linear-y*n;
        if(active_320){px=(320-(n+8))/2+4+x;delta_offset[i]=(u16)((y+4)*40+(px>>3));delta_mask[i]=(u8)(0x80>>(px&7));}
        else{/* Keep offsets relative: screen[off] already adds the far pointer base. */
            off=(u16)((y0+(y+4)*2)*80+data_byte+(x>>2));delta_offset[i]=off;delta_select[i>>2]|=(u8)((x&3)<<((i&3)*2));}}
    _fmemcpy(previous_codewords,codewords,codeword_len);delta_bits=(u16)bits;
    delta_codewords=codeword_len;delta_n=n;qrcodegen_dosferReleaseMatrixCache();return 1;
}
static void update_delta(const u8 *codewords,u8 far *pixels) {
    if(active_320){const u16 far *offset=delta_offset;const u8 far *masks=delta_mask;u16 i,off;u8 changed;
        (void)pixels;
        for(i=0;i<delta_codewords;++i,offset+=8,masks+=8){changed=(u8)(codewords[i]^previous_codewords[i]);previous_codewords[i]=codewords[i];
            #define DOSFER_TOGGLE_320(k,b) if(changed&(b)){off=offset[k];screen_320[off]^=masks[k];}
            DOSFER_TOGGLE_320(0,0x80)DOSFER_TOGGLE_320(1,0x40)DOSFER_TOGGLE_320(2,0x20)DOSFER_TOGGLE_320(3,0x10)
            DOSFER_TOGGLE_320(4,0x08)DOSFER_TOGGLE_320(5,0x04)DOSFER_TOGGLE_320(6,0x02)DOSFER_TOGGLE_320(7,0x01)
            #undef DOSFER_TOGGLE_320
        }return;}
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
int vga_delta_ready(void){return delta_offset!=0;}
u32 vga_screen_hash(void) {
    u32 i,h=2166136261UL;if(!active_320&&!screen)return 0;
    if(active_320)for(i=0;i<8000;++i){h^=screen_320[i];h*=16777619UL;}
    else for(i=0;i<screen_bytes;++i){h^=screen[i];h*=16777619UL;}
    return h;
}
int vga_display_matches(void) {
    u8 far *vram;u16 i;
    if(!active_320)return 1;
    outp(0x3CE,4);outp(0x3CF,0); /* Read plane 0; all four QR planes are identical. */
    vram=(u8 far *)MK_FP(0xA000,(u16)(display_page<<13));
    for(i=0;i<7680;++i)if(vram[i]!=screen_320[i])return 0; /* Rows 0..191 exclude BIOS status text. */
    return 1;
}
int vga_show_qr_stream(const u8 *qr,const u8 *codewords,u16 codeword_len,int n,int scale,int invert,const char *a,const char *b,int marker,int delta_only){
    int total=(n+8)*scale,x0=(640-total)/2,y0=8;
    if(active_320){(void)b;(void)marker;if(scale!=1)return 0;
        if(delta_only){if(!delta_offset||delta_n!=n||delta_codewords!=codeword_len||screen_invert!=invert)return 0;update_delta(codewords,screen);}
        else{if(!build_qr_image_320(qr,n,invert))return 0;prepare_delta(codewords,codeword_len,n);}
        copy_320_flip();status_text(24,a);return 1;}
    if(scale==2||scale==4)x0=(((x0+4*scale+4)/8)*8)-4*scale;
    if(delta_only){if(!delta_offset||delta_n!=n||delta_codewords!=codeword_len||screen_invert!=invert)return 0;update_delta(codewords,screen);
        fill_rect(x0-12,y0+total+4,24,4,!invert);fill_rect(x0+total-12,y0+total+4,24,4,!invert);fill_rect(marker?x0-12:x0+total-12,y0+total+4,24,4,invert);
    }else{if(!build_qr_image(qr,n,scale,invert,marker))return 0;if(scale==2)prepare_delta(codewords,codeword_len,n);}
    if(delta_only)copy_qr_delta_image(n,scale);else copy_qr_image();status_text(26,a);status_text(28,b);return 1;
}
int vga_show_qr(const u8 *qr,int n,int scale,int invert,const char *a,const char *b,int marker) {
    if(active_320){(void)b;(void)marker;if(scale!=1||!build_qr_image_320(qr,n,invert))return 0;copy_320_flip();status_text(24,a);return 1;}
    if(!build_qr_image(qr,n,scale,invert,marker))return 0;
    copy_qr_image();
    status_text(26,a); status_text(28,b);
    return 1;
}
void vga_benchmark_qr(const u8 *qr,int n,int scale,int loops,u32 *build_ms,u32 *copy_ms,u32 *text_ms) {
    int i;u32 a,b;
    if(active_320){a=timer_ticks();for(i=0;i<loops;++i)build_qr_image_320(qr,n,0);b=timer_ticks();*build_ms=timer_elapsed_ms(a,b);
        a=timer_ticks();for(i=0;i<loops;++i)copy_320_flip();b=timer_ticks();*copy_ms=timer_elapsed_ms(a,b);
        a=timer_ticks();for(i=0;i<loops;++i)status_text(24,"DOSfer 320x200 status benchmark");b=timer_ticks();*text_ms=timer_elapsed_ms(a,b);return;}
    a=timer_ticks();for(i=0;i<loops;++i)build_qr_image(qr,n,scale,0,i&1);b=timer_ticks();*build_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i)copy_qr_image();b=timer_ticks();*copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i){status_text(26,"DOSfer VGA status benchmark");status_text(28,"0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ");}b=timer_ticks();*text_ms=timer_elapsed_ms(a,b);
}
void vga_benchmark_delta(const u8 *codewords,u16 codeword_len,int n,int loops,u32 *update_ms,u32 *copy_ms,u32 *text_ms) {
    u8 far *alternate=(u8 far *)_fmalloc(codeword_len);int i;u32 a,b;
    if(!alternate||!delta_offset){*update_ms=*copy_ms=*text_ms=0;if(alternate)_ffree(alternate);return;}
    for(i=0;i<codeword_len;++i)alternate[i]=(u8)~codewords[i];
    a=timer_ticks();for(i=0;i<loops;++i)update_delta((i&1)?codewords:alternate,screen);b=timer_ticks();*update_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i){if(active_320)copy_320_flip();else copy_qr_delta_image(n,2);}b=timer_ticks();*copy_ms=timer_elapsed_ms(a,b);
    a=timer_ticks();for(i=0;i<loops;++i){if(active_320)status_text(24,"DOSfer 320 delta benchmark");else{status_text(26,"DOSfer delta status benchmark");status_text(28,"0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ");}}b=timer_ticks();*text_ms=timer_elapsed_ms(a,b);
    _ffree(alternate);
}
void speaker_beep(void) {
    u16 div=1193180UL/880; u8 old=inp(0x61);
    outp(0x43,0xB6); outp(0x42,(u8)div); outp(0x42,(u8)(div>>8));
    outp(0x61,old|3); delay(80); outp(0x61,old);
}
