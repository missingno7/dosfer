#include <string.h>
#include "dosfer.h"

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

u32 crc32_update(u32 crc, const void *data, u16 len) {
    const u8 *p=(const u8 *)data;
    if (!crc_ready) crc_init();
    crc ^= 0xFFFFFFFFUL;
    while(len>=4){u32 v=*(const u32 *)p;crc^=v;crc=crc_slice[2][(u8)crc]^crc_slice[1][(u8)(crc>>8)]^crc_slice[0][(u8)(crc>>16)]^crc_table[(u8)(crc>>24)];p+=4;len-=4;}
    while (len--) crc=crc_table[(u8)(crc^*p++)]^(crc>>8);
    return crc^0xFFFFFFFFUL;
}

u32 crc32_bytes(const void *data, u16 len) { return crc32_update(0,data,len); }
void put_u16(u8 *p,u16 v) { p[0]=(u8)(v>>8); p[1]=(u8)v; }
void put_u32(u8 *p,u32 v) { p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }

/* Reversible transport whitening. One xorshift32 step supplies four bytes,
   keeping the cost small on a 386 while preventing low-entropy file regions
   from turning into long, camera-hostile QR patterns. */
static u32 copy_payload_crc(u8 *dst,const u8 *src,u16 n,u16 flags,u32 session,u32 global_index) {
    u32 c=0xFFFFFFFFUL,state=session^(global_index*0x9E3779B9UL)^0xD05FE123UL,key;u16 chunk;
    if(!crc_ready)crc_init();if(!state)state=0xA5A5A5A5UL;
    if(flags&FF_WHITENED){while(n>=4){u32 v;state^=state<<13;state^=state>>17;state^=state<<5;v=*(const u32 *)src^state;*(u32 *)dst=v;
            c^=v;c=crc_slice[2][(u8)c]^crc_slice[1][(u8)(c>>8)]^crc_slice[0][(u8)(c>>16)]^crc_table[(u8)(c>>24)];src+=4;dst+=4;n-=4;}
        if(n){state^=state<<13;state^=state>>17;state^=state<<5;key=state;chunk=n;while(chunk--){u8 v=(u8)(*src++^(u8)key);*dst++=v;c=crc_table[(u8)(c^v)]^(c>>8);key>>=8;}}}
    else{while(n>=4){u32 v=*(const u32 *)src;*(u32 *)dst=v;c^=v;c=crc_slice[2][(u8)c]^crc_slice[1][(u8)(c>>8)]^crc_slice[0][(u8)(c>>16)]^crc_table[(u8)(c>>24)];src+=4;dst+=4;n-=4;}
        while(n--){u8 v=*src++;*dst++=v;c=crc_table[(u8)(c^v)]^(c>>8);}}
    return c^0xFFFFFFFFUL;
}

u16 make_record(u8 *out,u8 type,u32 rid,u32 fid,const u8 *body,u16 n) {
    memcpy(out,"DQRC",4); out[4]=1; out[5]=type; put_u16(out+6,0);
    put_u32(out+8,rid); put_u32(out+12,fid); put_u32(out+16,(u32)n);
    put_u32(out+20,crc32_bytes(body,n));
    if (n) memcpy(out+24,body,n);
    return (u16)(24+n);
}

u16 make_frame(u8 *out,u8 kind,u16 flags,u32 session,u32 window,u32 gi,
               u16 wi,u16 wc,u32 sid,u32 off,const u8 *payload,u16 n) {
    u32 hcrc,pcrc=0;
    memset(out,0,48); memcpy(out,"DQR1",4); out[4]=1; out[5]=kind;
    put_u16(out+6,flags); put_u32(out+8,session); put_u32(out+12,window);
    put_u32(out+16,gi); put_u16(out+20,wi); put_u16(out+22,wc);
    put_u32(out+24,sid); put_u32(out+28,off); put_u16(out+32,n);
    put_u16(out+34,48);
    if(n)pcrc=copy_payload_crc(out+48,payload,n,flags,session,gi);
    put_u32(out+36,pcrc);
    put_u32(out+44,0); hcrc=crc32_bytes(out,48); put_u32(out+40,hcrc);
    return (u16)(48+n);
}

int config_load(Config *c,const char *path) {
    FILE *f=fopen(path,"rb"); char magic[4];
    if (!f) return 0;
    if (fread(magic,1,4,f)!=4 || memcmp(magic,"DQCF",4) ||
        fread(c,1,sizeof(*c),f)!=sizeof(*c)) { fclose(f); return 0; }
    fclose(f);
    if (c->qr_version<5 || c->qr_version>40 || c->ecc>3 ||
        c->module_pixels<1 || c->module_pixels>6 ||
        c->frame_payload<96 || c->frame_payload>MAX_FRAME_PAYLOAD ||
        c->window_frames<4 || c->window_frames>MAX_WINDOW) return 0;
    /* Continuous mode has no user-facing opt-in in v1. Never let a stale or
       older config file silently skip the manual window acknowledgement. */
    c->continuous=0;
    return 1;
}
int config_save(const Config *c,const char *path) {
    FILE *f=fopen(path,"wb");
    if (!f) return 0;
    fwrite("DQCF",1,4,f); fwrite(c,1,sizeof(*c),f); fclose(f); return 1;
}
