#include <string.h>
#include "dosfer.h"

#ifdef DOSFER_PROFILE
#include "timing.h"
u32 dosferProtocolProfileTicks[3];
#endif

#define CRC_POLY 0xEDB88320UL
#define WHITEN_CONST 0xD05FE123UL
#define WHITEN_STEP 0x9E3779B9UL
#define WHITEN_FALLBACK 0xA5A5A5A5UL

static u32 crc_table[256];
static u32 crc_slice[3][256];
static int crc_ready;

static u32 xorshift32(u32 x) {
    x^=x<<13;
    x^=x>>17;
    return x^(x<<5);
}

static u32 whitening_seed(u32 session,u32 index) {
    u32 state=session^(index*WHITEN_STEP)^WHITEN_CONST;
    return state?state:WHITEN_FALLBACK;
}

static void crc_init(void) {
    u16 i,j;

    for(i=0;i<256;++i) {
        u32 c=(u32)i;
        for(j=0;j<8;++j)
            c=(c&1)?(CRC_POLY^(c>>1)):(c>>1);
        crc_table[i]=c;
    }

    for(i=0;i<256;++i) {
        u32 c=crc_table[i];
        crc_slice[0][i]=(c>>8)^crc_table[(u8)c];
        crc_slice[1][i]=(crc_slice[0][i]>>8)^crc_table[(u8)crc_slice[0][i]];
        crc_slice[2][i]=(crc_slice[1][i]>>8)^crc_table[(u8)crc_slice[1][i]];
    }
    crc_ready=1;
}

static u32 crc_word(u32 crc,u32 value) {
    crc^=value;
    return crc_slice[2][(u8)crc]^crc_slice[1][(u8)(crc>>8)]^
           crc_slice[0][(u8)(crc>>16)]^crc_table[(u8)(crc>>24)];
}

static u32 crc_byte(u32 crc,u8 value) {
    return crc_table[(u8)(crc^value)]^(crc>>8);
}

u32 crc32_update(u32 crc,const void *data,u16 len) {
    const u8 *p=(const u8 *)data;

    if(!crc_ready)crc_init();
    crc^=0xFFFFFFFFUL;
    while(len>=4) {
        crc=crc_word(crc,*(const u32 *)p);
        p+=4;
        len-=4;
    }
    while(len--)
        crc=crc_byte(crc,*p++);
    return crc^0xFFFFFFFFUL;
}

u32 crc32_bytes(const void *data,u16 len) {
    return crc32_update(0,data,len);
}

void put_u16(u8 *p,u16 value) {
    p[0]=(u8)(value>>8);
    p[1]=(u8)value;
}

void put_u32(u8 *p,u32 value) {
    p[0]=(u8)(value>>24);
    p[1]=(u8)(value>>16);
    p[2]=(u8)(value>>8);
    p[3]=(u8)value;
}

/* Copy and CRC the transport payload in one pass. Whitening deliberately uses
 * one xorshift32 word per four bytes, which is cheap on a 386 and prevents
 * low-entropy file regions from creating camera-hostile QR patterns. */
static u32 copy_payload_crc(u8 *dst,const u8 *src,u16 len,u16 flags,
        u32 session,u32 global_index) {
    u32 crc=0xFFFFFFFFUL;
    u32 left=whitening_seed(session,global_index);
    u32 right=0;

    if(!crc_ready)crc_init();
    if(flags&FF_PAIR_WHITENED)
        right=whitening_seed(session,global_index+1UL);

    while(len>=4) {
        u32 value=*(const u32 *)src;
        if(flags&FF_PAIR_WHITENED) {
            left=xorshift32(left);
            right=xorshift32(right);
            value^=left^right;
        } else if(flags&FF_WHITENED) {
            left=xorshift32(left);
            value^=left;
        }
        *(u32 *)dst=value;
        crc=crc_word(crc,value);
        src+=4;
        dst+=4;
        len-=4;
    }

    if(len) {
        u32 key=0;
        if(flags&FF_PAIR_WHITENED) {
            left=xorshift32(left);
            right=xorshift32(right);
            key=left^right;
        } else if(flags&FF_WHITENED) {
            left=xorshift32(left);
            key=left;
        }
        while(len--) {
            u8 value=*src++;
            if(flags&(FF_PAIR_WHITENED|FF_WHITENED)) {
                value^=(u8)key;
                key>>=8;
            }
            *dst++=value;
            crc=crc_byte(crc,value);
        }
    }
    return crc^0xFFFFFFFFUL;
}

u16 make_record(u8 *out,u16 capacity,u8 type,u32 record_id,u32 file_id,
        const u8 *body,u16 body_len) {
    if(capacity<RECORD_HEADER_SIZE||body_len>(u16)(capacity-RECORD_HEADER_SIZE))
        return 0;

    memcpy(out,"DQRC",4);
    out[4]=1;
    out[5]=type;
    put_u16(out+6,0);
    put_u32(out+8,record_id);
    put_u32(out+12,file_id);
    put_u32(out+16,(u32)body_len);
    put_u32(out+20,crc32_bytes(body,body_len));
    if(body_len)memcpy(out+RECORD_HEADER_SIZE,body,body_len);
    return (u16)(RECORD_HEADER_SIZE+body_len);
}

static u16 write_frame_header(u8 *out,u8 kind,u16 flags,u32 session,u32 window,
        u32 global_index,u16 window_index,u16 window_count,u32 stream_id,
        u32 stream_offset,u32 payload_crc,u16 payload_len) {
    u32 header_crc;

    memset(out,0,FRAME_HEADER_SIZE);
    memcpy(out,"DQR1",4);
    out[4]=1;
    out[5]=kind;
    put_u16(out+6,flags);
    put_u32(out+8,session);
    put_u32(out+12,window);
    put_u32(out+16,global_index);
    put_u16(out+20,window_index);
    put_u16(out+22,window_count);
    put_u32(out+24,stream_id);
    put_u32(out+28,stream_offset);
    put_u16(out+32,payload_len);
    put_u16(out+34,FRAME_HEADER_SIZE);
    put_u32(out+36,payload_crc);
    put_u32(out+44,0);
    header_crc=crc32_bytes(out,FRAME_HEADER_SIZE);
    put_u32(out+40,header_crc);
    return (u16)(FRAME_HEADER_SIZE+payload_len);
}

u16 make_frame_header_crc(u8 *out,u8 kind,u16 flags,u32 session,u32 window,
        u32 global_index,u16 window_index,u16 window_count,u32 stream_id,
        u32 stream_offset,u32 payload_crc,u16 payload_len) {
    return write_frame_header(out,kind,flags,session,window,global_index,
        window_index,window_count,stream_id,stream_offset,payload_crc,payload_len);
}

u16 make_frame(u8 *out,u8 kind,u16 flags,u32 session,u32 window,u32 global_index,
        u16 window_index,u16 window_count,u32 stream_id,u32 stream_offset,
        const u8 *payload,u16 payload_len) {
    u32 payload_crc=0;
#ifdef DOSFER_PROFILE
    u32 profile_start=timer_ticks(),profile_now;
#endif

#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();
    dosferProtocolProfileTicks[0]+=profile_now-profile_start;
    profile_start=profile_now;
#endif
    if(payload_len)
        payload_crc=copy_payload_crc(out+FRAME_HEADER_SIZE,payload,payload_len,
            flags,session,global_index);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();
    dosferProtocolProfileTicks[1]+=profile_now-profile_start;
    profile_start=profile_now;
#endif
    write_frame_header(out,kind,flags,session,window,global_index,
        window_index,window_count,stream_id,stream_offset,payload_crc,payload_len);
#ifdef DOSFER_PROFILE
    profile_now=timer_ticks();
    dosferProtocolProfileTicks[2]+=profile_now-profile_start;
#endif
    return (u16)(FRAME_HEADER_SIZE+payload_len);
}
