#include <string.h>
#include "dosfer.h"

#ifdef DOSFER_PROFILE
#include "timing.h"
u32 dosferProtocolProfileTicks[3];
#endif

static u32 crc_table[256];
static u32 crc_slice[3][256];
static int crc_ready;

static void crc_init(void) {
    u16 i, j;
    for (i=0; i<256; ++i) {
        u32 c=(u32)i;
        for (j=0; j<8; ++j) c=(c&1) ? (0xEDB88320UL^(c>>1)) : (c>>1);
        crc_table[i]=c;
    }
    for(i=0;i<256;++i){u32 c=crc_table[i];crc_slice[0][i]=(c>>8)^crc_table[(u8)c];
        crc_slice[1][i]=(crc_slice[0][i]>>8)^crc_table[(u8)crc_slice[0][i]];
        crc_slice[2][i]=(crc_slice[1][i]>>8)^crc_table[(u8)crc_slice[1][i]];}
    crc_ready=1;
}

u32 crc32_update(u32 crc, const void far *data, u16 len) {
    const u8 far *p=(const u8 far *)data;
    if (!crc_ready) crc_init();
    crc ^= 0xFFFFFFFFUL;
    while (len--) crc=crc_table[(u8)(crc^*p++)]^(crc>>8);
    return crc^0xFFFFFFFFUL;
}

u32 crc32_bytes(const void far *data, u16 len) { return crc32_update(0,data,len); }
void put_u16(u8 far *p,u16 v) { p[0]=(u8)(v>>8); p[1]=(u8)v; }
void put_u32(u8 far *p,u32 v) { p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }

/* Reversible transport whitening. One xorshift32 step supplies four bytes,
   keeping the cost small on a 386 while preventing low-entropy file regions
   from turning into long, camera-hostile QR patterns. */
static u32 copy_payload_crc(u8 far *dst,const u8 far *src,u16 n,u16 flags,u32 session,u32 global_index) {
    u32 c=0xFFFFFFFFUL,state=session^(global_index*0x9E3779B9UL)^0xD05FE123UL,key;u16 chunk;
    if(!crc_ready)crc_init();if(!state)state=0xA5A5A5A5UL;
    if(flags&FF_PAIR_WHITENED){u32 right=session^((global_index+1UL)*0x9E3779B9UL)^0xD05FE123UL;if(!right)right=0xA5A5A5A5UL;
        while(n>=4){u32 v;state^=state<<13;state^=state>>17;state^=state<<5;right^=right<<13;right^=right>>17;right^=right<<5;v=*(const u32 far *)src^state^right;*(u32 far *)dst=v;
            c^=v;c=crc_slice[2][(u8)c]^crc_slice[1][(u8)(c>>8)]^crc_slice[0][(u8)(c>>16)]^crc_table[(u8)(c>>24)];src+=4;dst+=4;n-=4;}
        if(n){state^=state<<13;state^=state>>17;state^=state<<5;right^=right<<13;right^=right>>17;right^=right<<5;key=state^right;chunk=n;while(chunk--){u8 v=(u8)(*src++^(u8)key);*dst++=v;c=crc_table[(u8)(c^v)]^(c>>8);key>>=8;}}}
    else if(flags&FF_WHITENED){while(n>=4){u32 v;state^=state<<13;state^=state>>17;state^=state<<5;v=*(const u32 far *)src^state;*(u32 far *)dst=v;
            c^=v;c=crc_slice[2][(u8)c]^crc_slice[1][(u8)(c>>8)]^crc_slice[0][(u8)(c>>16)]^crc_table[(u8)(c>>24)];src+=4;dst+=4;n-=4;}
        if(n){state^=state<<13;state^=state>>17;state^=state<<5;key=state;chunk=n;while(chunk--){u8 v=(u8)(*src++^(u8)key);*dst++=v;c=crc_table[(u8)(c^v)]^(c>>8);key>>=8;}}}
    else{while(n>=4){u32 v=*(const u32 far *)src;*(u32 far *)dst=v;c^=v;c=crc_slice[2][(u8)c]^crc_slice[1][(u8)(c>>8)]^crc_slice[0][(u8)(c>>16)]^crc_table[(u8)(c>>24)];src+=4;dst+=4;n-=4;}
        while(n--){u8 v=*src++;*dst++=v;c=crc_table[(u8)(c^v)]^(c>>8);}}
    return c^0xFFFFFFFFUL;
}

u16 make_record(u8 far *out,u16 capacity,u8 type,u32 rid,u32 fid,const u8 far *body,u16 n) {
    if(capacity<RECORD_HEADER_SIZE || n>(u16)(capacity-RECORD_HEADER_SIZE))return 0;
    _fmemcpy(out,"DQRC",4); out[4]=1; out[5]=type; put_u16(out+6,0);
    put_u32(out+8,rid); put_u32(out+12,fid); put_u32(out+16,(u32)n);
    put_u32(out+20,crc32_bytes(body,n));
    if (n) _fmemcpy(out+24,body,n);
    return (u16)(24+n);
}

static u16 write_frame_header(u8 far *out,u8 kind,u16 flags,u32 session,u32 window,u32 gi,
               u16 wi,u16 wc,u32 sid,u32 off,u32 pcrc,u16 n) {
    u32 hcrc;
    _fmemset(out,0,48); _fmemcpy(out,"DQR1",4); out[4]=1; out[5]=kind;
    put_u16(out+6,flags); put_u32(out+8,session); put_u32(out+12,window);
    put_u32(out+16,gi); put_u16(out+20,wi); put_u16(out+22,wc);
    put_u32(out+24,sid); put_u32(out+28,off); put_u16(out+32,n);
    put_u16(out+34,48);put_u32(out+36,pcrc);
    put_u32(out+44,0); hcrc=crc32_bytes(out,48); put_u32(out+40,hcrc);
    return (u16)(48+n);
}

u16 make_frame_header_crc(u8 far *out,u8 kind,u16 flags,u32 session,u32 window,u32 gi,
               u16 wi,u16 wc,u32 sid,u32 off,u32 pcrc,u16 n) {
    return write_frame_header(out,kind,flags,session,window,gi,wi,wc,sid,off,pcrc,n);
}

u16 make_frame(u8 far *out,u8 kind,u16 flags,u32 session,u32 window,u32 gi,
               u16 wi,u16 wc,u32 sid,u32 off,const u8 far *payload,u16 n) {
    u32 pcrc=0;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks(),profile_now;
#endif
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferProtocolProfileTicks[0]+=profile_now-profile_start;profile_start=profile_now;
#endif
    if(n) {
        /* Payload may already live in far producer memory. Whitening/CRC now
           use explicit far accesses so DS clobber from earlier far work cannot
           truncate the destination to a near DGROUP offset. */
        _fmemcpy(out+48,payload,n);
        pcrc=copy_payload_crc(out+48,out+48,n,flags,session,gi);
    }
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferProtocolProfileTicks[1]+=profile_now-profile_start;profile_start=profile_now;
#endif
    write_frame_header(out,kind,flags,session,window,gi,wi,wc,sid,off,pcrc,n);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();dosferProtocolProfileTicks[2]+=profile_now-profile_start;
#endif
    return (u16)(48+n);
}
u16 make_plane_frame(u8 far *out,u32 session,u32 window,u32 group_global,
                     u16 group_index,u16 window_count,u8 group_width,u8 coefficient,
                     const u8 far *payload,u16 payload_len) {
    u16 flags;
    u32 pcrc;
    if(!payload||(group_width!=3&&group_width!=4)||!coefficient||coefficient>0x0F)return 0;
    /* All PLANE3 equations have odd coefficient weight, so a common group
       whitening stream survives in the parity equation.  PLANE4's 0xF
       equation has four bases: their common stream cancels, hence its parity
       intentionally remains unwhitened.  This keeps the production affine
       correction header-only instead of turning it into an 8 KiB raster. */
    flags=(group_width==4&&coefficient==0x0F)?0:FF_PLANE_WHITENED;
    _fmemcpy(out+48,payload,payload_len);
    pcrc=copy_payload_crc(out+48,out+48,payload_len,
        flags?FF_WHITENED:0,session,group_global);
    return write_frame_header(out,FK_PLANE_CODED,flags,session,window,
        coefficient,group_index,window_count,group_global,group_width,pcrc,payload_len);
}
