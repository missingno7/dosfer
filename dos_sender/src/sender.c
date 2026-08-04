#include <conio.h>
#include <bios.h>
#include <ctype.h>
#include <dos.h>
#include <io.h>
#include <i86.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dosfer.h"
#include "qrcodegen.h"
#include "timing.h"
#include "vga.h"

#ifdef DOSFER_PROFILE
extern u32 dosferQrProfileTicks[6];
extern u32 dosferProtocolProfileTicks[3];
extern u32 dosferVgaProfileTicks[5];
#endif

#define MANIFEST_NAME "DOSFER.$$$"
#define DISK_BUFFER 16384
#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(40)

typedef struct {
    FILE *manifest, *source;
    ManifestEntry entry;
    int state, have_entry, finished;
    u32 record_id, global_index, file_offset, file_crc;
    u32 file_count, dir_count, total_bytes;
    u8 disk[2][DISK_BUFFER];
    u16 disk_len[2], disk_pos[2];
    int disk_slot, io_error;
} Producer;

static Producer producer;
static Window current_window, previous_window;
static int have_previous;
static u8 qr_temp[QR_BUFFER+1], qr_code[QR_BUFFER+1], raw_frame[MAX_QR_BYTES];
static u8 far record_body[MAX_FRAME_PAYLOAD];
static u32 selected_files, selected_dirs, selected_bytes;
static u32 completed_session,completed_frames,completed_bytes;
static u32 last_visible_tick;
static int qr_delta_ready;
static u8 qr_mask=0;
static int encoded_qr_mask=-1;
static u16 chain_anchor=0;
static u8 chain_payload[MAX_FRAME_PAYLOAD];
static u8 far *chain_left_codewords,*chain_right_codewords,*chain_cached_raw;
static int chain_cache_valid;
static u32 chain_cache_session,chain_cache_window,chain_cache_global;
static u16 chain_cache_rawlen,chain_cache_index;
static u8 chain_cache_mask;

static void producer_close(void) {
    if(producer.source){fclose(producer.source);producer.source=0;}
}
static void free_window(Window *w) {
    u16 i;
    for(i=0;i<MAX_WINDOW;++i)if(w->frames[i].payload){
        _ffree(w->frames[i].payload);w->frames[i].payload=0;
        w->frames[i].payload_capacity=w->frames[i].payload_len=0;
    }
    w->count=0;
}
static void free_chain_cache(void) {
    if(chain_left_codewords)_ffree(chain_left_codewords);
    if(chain_right_codewords)_ffree(chain_right_codewords);
    if(chain_cached_raw)_ffree(chain_cached_raw);
    chain_left_codewords=chain_right_codewords=chain_cached_raw=0;
    chain_cache_valid=0;
}
static void sender_cleanup(void) {
    producer_close();free_window(&current_window);free_window(&previous_window);
    free_chain_cache();vga_leave();
}

static int ensure_chain_cache(void) {
    u16 cw=(u16)qrcodegen_dosferCodewordBytes(40);
    if(chain_left_codewords&&chain_right_codewords&&chain_cached_raw)return 1;
    chain_left_codewords=(u8 far *)_fmalloc(cw);chain_right_codewords=(u8 far *)_fmalloc(cw);
    chain_cached_raw=(u8 far *)_fmalloc(FRAME_HEADER_SIZE);
    if(!chain_left_codewords||!chain_right_codewords||!chain_cached_raw){
        if(chain_left_codewords)_ffree(chain_left_codewords);if(chain_right_codewords)_ffree(chain_right_codewords);if(chain_cached_raw)_ffree(chain_cached_raw);
        chain_left_codewords=chain_right_codewords=chain_cached_raw=0;return 0;}
    return 1;
}

static u16 qr_codeword_bytes(const Config *cfg) {
    return (u16)qrcodegen_dosferCodewordBytes(cfg->qr_version);
}
static int validate_config(const Config *cfg) {
    u32 raw_bytes=(u32)FRAME_HEADER_SIZE+cfg->frame_payload;
    u32 used_bits,capacity_bits;
    int data_bytes=qrcodegen_dosferDataCodewordBytes(cfg->qr_version,
        (enum qrcodegen_Ecc)cfg->ecc);
    int modules=cfg->qr_version*4+17,total;
    if(cfg->qr_version<5||cfg->qr_version>40||cfg->ecc>3||
       cfg->module_pixels<1||cfg->module_pixels>6||
       cfg->frame_payload<96||cfg->frame_payload>MAX_FRAME_PAYLOAD||
       cfg->window_frames<4||cfg->window_frames>MAX_WINDOW||
       cfg->repetitions<1||cfg->repetitions>20||
       cfg->redundancy>MAX_WINDOW||
       (cfg->chain_width&&(cfg->chain_width<2||cfg->chain_width>MAX_WINDOW||
        (cfg->chain_width&1)||cfg->chain_width/2>=cfg->window_frames))||!data_bytes) {
        puts("Invalid sender configuration values.");return 0;
    }
    used_bits=raw_bytes*8UL;
    if(cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1)used_bits+=32UL;
    else used_bits+=(cfg->qr_version<10)?12UL:20UL;
    capacity_bits=(u32)data_bytes*8UL;
    if(used_bits>capacity_bits) {
        u32 overhead=(cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1)?32UL:
            (cfg->qr_version<10?12UL:20UL);
        u32 frame_bytes=(capacity_bits-overhead)/8UL;
        u32 max_payload=frame_bytes>FRAME_HEADER_SIZE?frame_bytes-FRAME_HEADER_SIZE:0;
        printf("Payload %u does not fit QR v%u-%c; maximum is %lu bytes.\n",
            cfg->frame_payload,cfg->qr_version,"LMQH"[cfg->ecc],max_payload);
        return 0;
    }
    total=(modules+8)*cfg->module_pixels;
    if(cfg->qr_version==40&&cfg->module_pixels==1) {
        if(total>192){puts("QR does not fit the 320x200 transfer layout.");return 0;}
    } else if(total>440) {
        puts("QR does not fit the 640x480 transfer layout; reduce /SCALE.");return 0;
    }
    return 1;
}
static int can_delta(const Config *cfg,u8 display_mask) {
    return qr_delta_ready&&display_mask==encoded_qr_mask&&
        (cfg->module_pixels==2||(cfg->qr_version==40&&cfg->module_pixels==1));
}

static void default_config(Config *c) {
    memset(c,0,sizeof(*c)); c->qr_version=40; c->ecc=0; c->module_pixels=1;
    c->repetitions=1; c->frame_payload=2904; c->hold_ms=0;
    c->window_frames=32; c->speaker=1;c->redundancy=7;c->chain_width=0;
}
static u16 redundancy_group(const Config *c) {
    return c->chain_width?0:c->redundancy;
}
static const char *redundancy_name(const Config *c) {
    static char name[12];
    if(c->chain_width)sprintf(name,"C%u",c->chain_width);
    else if(c->redundancy)sprintf(name,"%u",c->redundancy);
    else strcpy(name,"OFF");
    return name;
}
static const char *base_name(const char *p) {
    const char *b=p,*q;
    for(q=p;*q;++q) if(*q=='\\' || *q=='/' || *q==':') b=q+1;
    return *b?b:"ROOT";
}
static void slash_to_forward(char *p) { while(*p){if(*p=='\\')*p='/';++p;} }
static int manifest_write(FILE *f,u8 kind,const char *src,const char *rel,
                          const struct find_t *d,u32 fid) {
    ManifestEntry e;
    if(strlen(src)>=PATH_BYTES || strlen(rel)>=PATH_BYTES) {
        printf("Path too long (max %u): %s\n",PATH_BYTES-1,src); return 0;
    }
    memset(&e,0,sizeof(e)); e.kind=kind; e.attributes=d->attrib;
    e.dos_date=d->wr_date; e.dos_time=d->wr_time; e.size=d->size; e.file_id=fid;
    strcpy(e.source,src); strcpy(e.relative,rel); slash_to_forward(e.relative);
    if(fwrite(&e,1,sizeof(e),f)!=sizeof(e)) return 0;
    if(kind==1){selected_files++;selected_bytes+=e.size;}else selected_dirs++;
    return 1;
}
static int scan_path(FILE *mf,const char *path,const char *rel) {
    struct find_t d, child; char spec[PATH_BYTES],src[PATH_BYTES],dst[PATH_BYTES];
    unsigned rc;
    if(_dos_findfirst(path,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&d)) {
        printf("Not found: %s\n",path); return 0;
    }
    if(!(d.attrib&_A_SUBDIR)) return manifest_write(mf,1,path,rel,&d,selected_files+1);
    if(!manifest_write(mf,2,path,rel,&d,0)) return 0;
    if(strlen(path)+5>=PATH_BYTES) return 0;
    sprintf(spec,"%s\\*.*",path);
    rc=_dos_findfirst(spec,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&child);
    while(!rc) {
        if(strcmp(child.name,".") && strcmp(child.name,"..") && stricmp(child.name,MANIFEST_NAME)) {
            if(strlen(path)+strlen(child.name)+2>=PATH_BYTES ||
               strlen(rel)+strlen(child.name)+2>=PATH_BYTES) return 0;
            sprintf(src,"%s\\%s",path,child.name); sprintf(dst,"%s/%s",rel,child.name);
            if(!scan_path(mf,src,dst)) return 0;
        }
        rc=_dos_findnext(&child);
    }
    return 1;
}
static int add_selection(FILE *mf,const char *path) {
    char clean[PATH_BYTES]; size_t n=strlen(path); struct find_t d;
    if(!n || n>=PATH_BYTES) return 0; strcpy(clean,path);
    while(n>1 && (clean[n-1]=='\\'||clean[n-1]=='/')) clean[--n]=0;
    if(_dos_findfirst(clean,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&d)) {
        printf("Cannot select %s\n",clean); return 0;
    }
    return scan_path(mf,clean,base_name(clean));
}

static void producer_open(FILE *mf) {
    memset(&producer,0,sizeof(producer)); producer.manifest=mf; rewind(mf);
}
static int source_matches_manifest(void) {
    struct find_t d;
    if(_dos_findfirst(producer.entry.source,
       _A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_ARCH,&d))return 0;
    return !(d.attrib&_A_SUBDIR)&&d.size==producer.entry.size&&
        d.wr_date==producer.entry.dos_date&&d.wr_time==producer.entry.dos_time;
}
static void start_file(void) {
    producer.source=fopen(producer.entry.source,"rb"); producer.file_offset=0;
    producer.file_crc=0; producer.disk_slot=0;producer.io_error=0;
    producer.disk_len[0]=producer.disk_len[1]=0;
    producer.disk_pos[0]=producer.disk_pos[1]=0;
    if(producer.source&&!source_matches_manifest()){
        printf("File changed after selection: %s\n",producer.entry.source);
        producer_close();return;
    }
    if(producer.source) {
        producer.disk_len[0]=(u16)fread(producer.disk[0],1,DISK_BUFFER,producer.source);
        producer.disk_len[1]=(u16)fread(producer.disk[1],1,DISK_BUFFER,producer.source);
        if(ferror(producer.source))producer.io_error=1;
    }
}
static u16 read_piece(u8 *out,u16 want) {
    u16 got=0,n; int s;
    while(got<want && producer.file_offset<producer.entry.size) {
        s=producer.disk_slot;
        if(producer.disk_pos[s]>=producer.disk_len[s]) {
            producer.disk_len[s]=(u16)fread(producer.disk[s],1,DISK_BUFFER,producer.source);
            producer.disk_pos[s]=0;
            if(ferror(producer.source))producer.io_error=1;
            if(!producer.disk_len[s]) break;
        }
        n=(u16)(producer.disk_len[s]-producer.disk_pos[s]);
        if(n>want-got)n=(u16)(want-got);
        memcpy(out+got,producer.disk[s]+producer.disk_pos[s],n);
        producer.disk_pos[s]+=n; got+=n; producer.file_offset+=n;
        if(producer.disk_pos[s]>=producer.disk_len[s]) producer.disk_slot^=1;
    }
    producer.file_crc=crc32_update(producer.file_crc,out,got); return got;
}
static u16 path_meta(u8 *b,const ManifestEntry *e,int with_size) {
    u16 n=(u16)strlen(e->relative),p=0;
    b[p++]=e->attributes;b[p++]=0;put_u16(b+p,e->dos_date);p+=2;
    put_u16(b+p,e->dos_time);p+=2;
    if(with_size){put_u32(b+p,e->size);p+=4;}
    put_u16(b+p,n);p+=2;memcpy(b+p,e->relative,n);return (u16)(p+n);
}
static int producer_next(PendingFrame *f,const Config *cfg,u32 session) {
    u8 far *body=record_body,*payload=f->payload;u16 payload_capacity=f->payload_capacity,n,cap;u32 off;
    (void)session;
    if(payload_capacity<cfg->frame_payload) {
        u8 far *larger=(u8 far *)_fmalloc(cfg->frame_payload);
        if(!larger){puts("Not enough DOS memory for transfer window");return -1;}
        if(payload)_ffree(payload);payload=larger;payload_capacity=cfg->frame_payload;
    }
    memset(f,0,sizeof(*f));f->payload=payload;f->payload_capacity=payload_capacity;
    f->global_index=producer.global_index++;
again:
    if(producer.state==0) {
        put_u32(body,(u32)time(NULL)); put_u16(body+4,6); memcpy(body+6,"DOSFER",6);
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_SESSION,producer.record_id++,0,body,12);
        if(!f->payload_len){puts("Session record does not fit configured payload.");return -1;}
        producer.state=1; return 1;
    }
    if(producer.state==1) {
        if(fread(&producer.entry,1,sizeof(producer.entry),producer.manifest)!=sizeof(producer.entry)) {
            producer.state=5; goto again;
        }
        if(producer.entry.kind==2) {
            n=path_meta(body,&producer.entry,0); producer.dir_count++;
            f->payload_len=make_record(f->payload,f->payload_capacity,RT_DIRECTORY,producer.record_id++,0,body,n);
            if(!f->payload_len){printf("Directory metadata does not fit /PAYLOAD:%u: %s\n",cfg->frame_payload,producer.entry.relative);return -1;}
            return 1;
        }
        n=path_meta(body,&producer.entry,1); start_file();
        if(!producer.source){printf("Cannot open %s\n",producer.entry.source);return -1;}
        producer.file_count++; producer.total_bytes+=producer.entry.size; producer.state=2;
        f->stream_id=producer.entry.file_id;
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_FILE_BEGIN,producer.record_id++,producer.entry.file_id,body,n);
        if(!f->payload_len){printf("File metadata does not fit /PAYLOAD:%u: %s\n",cfg->frame_payload,producer.entry.relative);return -1;}
        return 1;
    }
    if(producer.state==2) {
        if(producer.file_offset>=producer.entry.size){producer.state=3;goto again;}
        cap=(u16)(cfg->frame_payload-RECORD_HEADER_SIZE-4); off=producer.file_offset;
        put_u32(body,off); n=read_piece(body+4,cap);
        if((!n||producer.io_error)&&producer.file_offset<producer.entry.size){
            printf("Read failed or file changed during transfer: %s\n",producer.entry.source);return -1;}
        f->stream_id=producer.entry.file_id;f->stream_offset=off;
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_FILE_DATA,producer.record_id++,producer.entry.file_id,body,(u16)(n+4));
        if(!f->payload_len){puts("Internal error: file data record exceeds payload.");return -1;}
        return 1;
    }
    if(producer.state==3) {
        if(!source_matches_manifest()){
            printf("File changed during transfer: %s\n",producer.entry.source);return -1;}
        put_u32(body,producer.entry.size);put_u32(body+4,producer.file_crc);
        producer_close();producer.state=1;
        f->stream_id=producer.entry.file_id;
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_FILE_END,producer.record_id++,producer.entry.file_id,body,8);
        if(!f->payload_len){puts("File-end record does not fit configured payload.");return -1;}
        return 1;
    }
    if(producer.state==5) {
        put_u32(body,producer.file_count);put_u32(body+4,producer.dir_count);
        put_u32(body+8,0);put_u32(body+12,producer.total_bytes);
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_TRANSFER_END,producer.record_id++,0,body,16);
        if(!f->payload_len){puts("Transfer-end record does not fit configured payload.");return -1;}
        producer.state=6;return 1;
    }
    producer.finished=1; producer.global_index--; return 0;
}
static int fill_window(Window *w,const Config *cfg,u32 session,u32 id) {
    int rc; w->count=0;w->id=id;
    while(w->count<cfg->window_frames) {
        rc=producer_next(&w->frames[w->count],cfg,session);
        if(rc<0)return 0;if(!rc)break;w->count++;
    }
    return w->count>0;
}
static int reserve_window(Window *w,const Config *cfg) {
    u16 i;
    for(i=0;i<cfg->window_frames;++i)if(w->frames[i].payload_capacity<cfg->frame_payload) {
        u8 far *payload=(u8 far *)_fmalloc(cfg->frame_payload);
        if(!payload){
            printf("Not enough DOS memory for two %u-frame replay windows; reduce /WINDOW.\n",
                cfg->window_frames);return 0;
        }
        if(w->frames[i].payload)_ffree(w->frames[i].payload);
        w->frames[i].payload=payload;w->frames[i].payload_capacity=cfg->frame_payload;
    }
    return 1;
}
static int qr_encode_mask(const u8 *data,u16 n,const Config *cfg,int delta_only,u8 mask) {
    int ok;
    memcpy(qr_temp,data,n);
    /* AUTO evaluates all eight masks and dominated runtime on a 386. Mask 0 is
       fully standard-compliant and cut measured DOSBox encoding time sharply. */
    qrcodegen_dosferSetCodewordsOnly(delta_only!=0);
    if(cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1)
        ok=qrcodegen_encodeBinaryAligned(qr_temp,n,qr_code,(enum qrcodegen_Ecc)cfg->ecc,
            cfg->qr_version,cfg->qr_version,(enum qrcodegen_Mask)mask,0);
    else ok=qrcodegen_encodeBinary(qr_temp,n,qr_code,(enum qrcodegen_Ecc)cfg->ecc,
            cfg->qr_version,cfg->qr_version,(enum qrcodegen_Mask)mask,0);
    if(ok)encoded_qr_mask=mask;return ok;
}
static int qr_encode(const u8 *data,u16 n,const Config *cfg,int delta_only) {
    return qr_encode_mask(data,n,cfg,delta_only,qr_mask);
}
static int make_prepacked_data(const Window *w,u16 i,const Config *cfg,u32 session) {
    const PendingFrame *f=&w->frames[i];u16 n;
    qr_code[0]=0x70;qr_code[1]=0x34;qr_code[2]=0x0B;qr_code[3]=0x88;
    n=make_frame(qr_code+4,FK_DATA,FF_WHITENED,session,w->id,f->global_index,
        i,w->count,f->stream_id,f->stream_offset,f->payload,f->payload_len);
    if(n!=2952)return 0;
    _fmemcpy(raw_frame,qr_code+4,FRAME_HEADER_SIZE);
    if(!qrcodegen_dosferEncodePrepackedV40L(qr_code,qr_temp))return 0;
    encoded_qr_mask=qr_mask;return n;
}
static int show_frame(const Window *w,u16 i,const Config *cfg,u32 session,int repeated,int waiting,u16 hold_ms,u8 display_mask) {
    char a[79],b[79];u16 n;u32 elapsed;int rescue=hold_ms!=cfg->hold_ms||display_mask!=qr_mask;
    int delta=can_delta(cfg,display_mask);
    const PendingFrame *f=&w->frames[i];
    if(delta&&!repeated&&chain_cache_valid&&chain_cache_session==session&&
       chain_cache_window==w->id&&chain_cache_global==f->global_index&&
       chain_cache_index==i&&chain_cache_mask==display_mask) {
        n=chain_cache_rawlen;
        _fmemcpy(raw_frame,chain_cached_raw,FRAME_HEADER_SIZE);
        _fmemcpy(qr_temp,chain_right_codewords,qr_codeword_bytes(cfg));
        encoded_qr_mask=display_mask;
        chain_cache_valid=0;
    } else {
        if(delta&&!repeated&&display_mask==qr_mask&&cfg->qr_version==40&&cfg->ecc==0&&
           cfg->module_pixels==1&&f->payload_len==2904) {
            n=(u16)make_prepacked_data(w,i,cfg,session);if(!n)return 0;
        } else {
            n=make_frame(raw_frame,FK_DATA,(repeated?FF_REPEATED:0)|FF_WHITENED,session,w->id,f->global_index,
                i,w->count,f->stream_id,f->stream_offset,f->payload,f->payload_len);
            if(!qr_encode_mask(raw_frame,n,cfg,delta,display_mask))return 0;
        }
    }
    if(cfg->module_pixels==1){if(waiting)strcpy(a,"READY - focus camera - Enter / Esc");
        else if(rescue)sprintf(a,"W%lu F%u/%u RESCUE %ums M%u",w->id+1,i+1,w->count,hold_ms,display_mask);
        else strcpy(a,"TRANSFER  V40L");}
    else sprintf(a,"Session %08lX  Window %lu  Frame %u/%u  QR v%u %c W",
        session,w->id+1,i+1,w->count,cfg->qr_version,"LMQH"[cfg->ecc]);
    if(cfg->module_pixels!=1) {
        if(waiting)strcpy(b,"READY - focus camera, then press Enter to start; Esc cancels");
        else if(rescue)sprintf(b,"RESCUE hold %u ms  mask %u  Payload %u  Global %lu",hold_ms,display_mask,f->payload_len,f->global_index);
        else sprintf(b,"Hold %u ms  Payload %u  Global %lu  controls at window end",
            cfg->hold_ms,f->payload_len,f->global_index);
    }
    if(last_visible_tick) {
        elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());
        if(elapsed<hold_ms)timer_wait_ms((u16)(hold_ms-elapsed));
    }
    if(!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,a,b,(int)(f->global_index&1),delta))return 0;
    qr_delta_ready=vga_delta_ready();
    last_visible_tick=timer_ticks();return 1;
}
static u32 read_u32be(const u8 *p) {
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
}
static u16 xor_frame_payload(const PendingFrame *left,const PendingFrame *right) {
    u16 j,k,words,common=left->payload_len<right->payload_len?left->payload_len:right->payload_len;
    u16 n=left->payload_len>right->payload_len?left->payload_len:right->payload_len;
    words=common>>2;
    for(k=0;k<words;++k)((u32 *)chain_payload)[k]=((const u32 far *)left->payload)[k]^((const u32 far *)right->payload)[k];
    j=(u16)(words<<2);for(;j<common;++j)chain_payload[j]=left->payload[j]^right->payload[j];
    if(left->payload_len>common)_fmemcpy(chain_payload+common,left->payload+common,left->payload_len-common);
    else if(right->payload_len>common)_fmemcpy(chain_payload+common,right->payload+common,right->payload_len-common);
    return n;
}
static int show_chain(const Window *w,u16 i,const Config *cfg,u32 session,u16 hold_ms) {
    const PendingFrame *left=&w->frames[i],*right=&w->frames[i+1];
    u16 j,n=left->payload_len>right->payload_len?left->payload_len:right->payload_len,rawlen,right_rawlen=0;
    u32 elapsed,lengths=((u32)left->payload_len<<16)|right->payload_len;
    u32 chain_crc;
    u8 left_header[FRAME_HEADER_SIZE],header_xor[FRAME_HEADER_SIZE];
    char a[79],b[79];int delta=can_delta(cfg,qr_mask),cached=0,derived=0;
    if(delta&&cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1&&ensure_chain_cache()) {
        _fmemcpy(left_header,raw_frame,FRAME_HEADER_SIZE);
        _fmemcpy(chain_left_codewords,qr_temp,qr_codeword_bytes(cfg));
        qr_code[0]=0x70;qr_code[1]=0x34;qr_code[2]=0x0B;qr_code[3]=0x88;
        right_rawlen=make_frame(qr_code+4,FK_DATA,FF_WHITENED,session,w->id,right->global_index,
            i+1,w->count,right->stream_id,right->stream_offset,right->payload,right->payload_len);
        _fmemcpy(chain_cached_raw,qr_code+4,FRAME_HEADER_SIZE);
        if(right_rawlen==2952&&qrcodegen_dosferEncodePrepackedV40L(qr_code,qr_temp)) {
            encoded_qr_mask=qr_mask;
            _fmemcpy(chain_right_codewords,qr_temp,qr_codeword_bytes(cfg));
            chain_cache_valid=1;chain_cache_session=session;chain_cache_window=w->id;
            chain_cache_global=right->global_index;chain_cache_index=(u16)(i+1);
            chain_cache_rawlen=right_rawlen;chain_cache_mask=qr_mask;cached=1;
        }
    }
    if(cached&&cfg->frame_payload==2904&&left->payload_len==cfg->frame_payload&&
       right->payload_len==cfg->frame_payload&&right_rawlen==2952) {
        /* CRC32 is affine for equal-length strings. The transmitted chained
           payload is exactly DATA-left XOR DATA-right. */
        chain_crc=read_u32be(left_header+36)^read_u32be(chain_cached_raw+36)^0xA15AD0F2UL;
        rawlen=make_frame_header_crc(raw_frame,FK_CHAIN_XOR,FF_PAIR_WHITENED,session,w->id,
            left->global_index,i,w->count,lengths,0,chain_crc,n);
        for(j=0;j<FRAME_HEADER_SIZE;++j)
            header_xor[j]=(u8)(left_header[j]^chain_cached_raw[j]^raw_frame[j]);
        derived=qrcodegen_dosferDeriveXorV40L(chain_left_codewords,chain_right_codewords,
            header_xor,qr_temp);
        if(derived)encoded_qr_mask=qr_mask;
    }
    if(!derived) {
        n=xor_frame_payload(left,right);
        rawlen=make_frame(raw_frame,FK_CHAIN_XOR,FF_PAIR_WHITENED,session,w->id,left->global_index,
            i,w->count,lengths,0,chain_payload,n);
        if(!qr_encode_mask(raw_frame,rawlen,cfg,delta,qr_mask))return 0;
    }
    if(cfg->module_pixels==1)strcpy(a,"TRANSFER  V40L");
    else sprintf(a,"Session %08lX  Window %lu  Recovery %u-%u/%u  QR v%u %c",
        session,w->id+1,i+1,i+2,w->count,cfg->qr_version,"LMQH"[cfg->ecc]);
    if(cfg->module_pixels!=1)sprintf(b,"CHAIN XOR  Hold %u ms  Payload %u  Global %lu",hold_ms,n,left->global_index);
    if(last_visible_tick){elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());if(elapsed<hold_ms)timer_wait_ms((u16)(hold_ms-elapsed));}
    if(!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,
        cfg->module_pixels,cfg->invert,a,b,(int)(left->global_index&1),delta))return 0;
    qr_delta_ready=vga_delta_ready();last_visible_tick=timer_ticks();return 1;
}
static u16 xor_block_payload(const Window *w,u16 first,u16 count) {
    u16 i,j,k,words,n=0;
    for(i=0;i<count;++i)if(w->frames[first+i].payload_len>n)
        n=w->frames[first+i].payload_len;
    memset(chain_payload,0,n);
    for(i=0;i<count;++i) {
        const PendingFrame *f=&w->frames[first+i];
        words=f->payload_len>>2;
        for(k=0;k<words;++k)((u32 *)chain_payload)[k]^=((const u32 far *)f->payload)[k];
        j=(u16)(words<<2);for(;j<f->payload_len;++j)chain_payload[j]^=f->payload[j];
    }
    return n;
}
static int show_block_parity(const Window *w,u16 first,u16 count,const Config *cfg,
        u32 session,u16 hold_ms) {
    const PendingFrame *base=&w->frames[first];
    char a[79],b[79];u16 n=xor_block_payload(w,first,count),rawlen;
    u32 elapsed;int delta=can_delta(cfg,qr_mask);
    rawlen=make_frame(raw_frame,FK_BLOCK_XOR,FF_WHITENED,session,w->id,
        base->global_index,first,w->count,count,0,chain_payload,n);
    if(!qr_encode_mask(raw_frame,rawlen,cfg,delta,qr_mask))return 0;
    if(cfg->module_pixels==1)strcpy(a,"TRANSFER  V40L");
    else sprintf(a,"Session %08lX  Window %lu  Parity %u-%u/%u  QR v%u %c",
        session,w->id+1,first+1,first+count,w->count,cfg->qr_version,"LMQH"[cfg->ecc]);
    if(cfg->module_pixels!=1) {
        if(cfg->chain_width)sprintf(b,"C%u XOR  Hold %u ms  Payload %u  Global %lu",
            cfg->chain_width,hold_ms,n,base->global_index);
        else sprintf(b,"%u+1 XOR  Hold %u ms  Payload %u  Global %lu",
            count,hold_ms,n,base->global_index);
    }
    if(last_visible_tick){elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());
        if(elapsed<hold_ms)timer_wait_ms((u16)(hold_ms-elapsed));}
    if(!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,
        cfg->module_pixels,cfg->invert,a,b,(int)(base->global_index&1),delta))return 0;
    qr_delta_ready=vga_delta_ready();last_visible_tick=timer_ticks();return 1;
}
static void flush_keys(void) {
    while(_bios_keybrd(_KEYBRD_READY))_bios_keybrd(_KEYBRD_READ);
}
static int decision_key(void) {
    u16 key=_bios_keybrd(_KEYBRD_READ);u8 ascii=(u8)key,scan=(u8)(key>>8);
    if(scan==0x0D||scan==0x4E)return '+';
    if(scan==0x0C||scan==0x4A)return '-';
    if(scan==0x13)return 'R';if(scan==0x32)return 'M';if(scan==0x30)return 'B';
    if(scan==0x01)return 27;if(scan==0x1C)return 13;
    return ascii;
}
static int transmit(const Window *w,const Config *cfg,u32 session,const u8 *selected,u16 chosen,u16 rescue_round) {
    u16 r,i,first,count,group=redundancy_group(cfg),half=(u16)(cfg->chain_width/2),hold_ms=cfg->hold_ms;
    u32 adjusted;u8 display_mask=qr_mask;int factor=1;
    if(selected&&chosen) {
        if((u32)chosen*16UL<=w->count)factor=4;
        else if((u32)chosen*8UL<=w->count)factor=3;
        else if((u32)chosen*2UL<=w->count)factor=2;
        adjusted=(cfg->hold_ms<100?100UL:cfg->hold_ms)*(u32)factor;
        hold_ms=(u16)(adjusted>60000UL?60000UL:adjusted);
        display_mask=(u8)((qr_mask+1+(rescue_round?rescue_round-1:0)%7)&7);
    }
    for(r=0;r<cfg->repetitions;++r) {
        first=0;count=0;
        for(i=0;i<w->count;++i)if(!selected || selected[i]) {
            if(!show_frame(w,i,cfg,session,r>0,0,hold_ms,display_mask))return 0;
            if(!selected&&half&&((i+1)%half)==0) {
                first=(u16)(i+1-half);
                if(first+half<w->count) {
                    count=(u16)(w->count-first);
                    if(count>cfg->chain_width)count=cfg->chain_width;
                    if(cfg->chain_width==2) {
                        if(!show_chain(w,first,cfg,session,hold_ms))return 0;
                    } else if(!show_block_parity(w,first,count,cfg,session,hold_ms))return 0;
                    if(chain_anchor&&((i+1)%chain_anchor)==0)
                        if(!show_frame(w,i,cfg,session,0,0,hold_ms,display_mask))return 0;
                }
            }
            if(!selected&&group) {
                if(!count)first=i;count++;
                if(count==group||i+1==w->count) {
                    if(!show_block_parity(w,first,count,cfg,session,hold_ms))return 0;
                    count=0;
                }
            }
        }
    }
    return 1;
}
static void show_eow(const Window *w,const Config *cfg,u32 session,int filtered) {
    char a[79],b[79];u16 n;u32 elapsed;
    n=make_frame(raw_frame,FK_END_WINDOW,0,session,w->id,w->frames[w->count-1].global_index,
        0,w->count,0,0,0,0);
    {int delta=can_delta(cfg,qr_mask);if(qr_encode(raw_frame,n,cfg,delta)) {
        if(last_visible_tick){elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());if(elapsed<cfg->hold_ms)timer_wait_ms((u16)(cfg->hold_ms-elapsed));}
        if(cfg->module_pixels==1)sprintf(a,"W%lu DONE  Enter R M B +/- Esc",w->id+1);
        else sprintf(a,"WINDOW %lu FINISHED - check phone for missing frames",w->id+1);
        if(filtered)strcpy(b,"Enter next  R replay missing  M change/clear  B previous  +/- speed  Esc");
        else strcpy(b,"Enter next  R replay all  M set missing  B previous  +/- speed  Esc");
        if(!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,a,b,0,delta))return;
        qr_delta_ready=vga_delta_ready();
        last_visible_tick=timer_ticks();
    }}
    if(cfg->speaker)speaker_beep();
}
static u16 parse_ranges(char *s,u8 *selected,u16 count) {
    char *p=s;long a,b,i;u16 chosen=0;memset(selected,0,MAX_WINDOW);
    while(*p){while(*p==' '||*p==',')p++;if(!*p)break;a=strtol(p,&p,10);b=a;
        if(*p=='-'){p++;b=strtol(p,&p,10);}if(a<1)a=1;if(b>count)b=count;
        for(i=a;i<=b;++i)if(!selected[i-1]){selected[i-1]=1;chosen++;}while(*p&&*p!=',')p++;}
    return chosen;
}
static int blank_line(const char *s) {
    while(*s){if(*s!=' '&&*s!='\t'&&*s!='\r'&&*s!='\n')return 0;s++;}return 1;
}
static void exit_to_dos(u8 status) {
    union REGS r;memset(&r,0,sizeof(r));r.h.ah=0x4C;r.h.al=status;int86(0x21,&r,&r);
}
static int run_transfer(FILE *mf,Config *cfg) {
    u32 session=(timer_ticks()^(u32)time(NULL)^selected_bytes)|1UL,window_id=0;
    int key,have_selection=0;u16 chosen=0,rescue_round=0;u8 selected[MAX_WINDOW];char line[80];
    vga_use_320(cfg->qr_version==40&&cfg->module_pixels==1);producer_open(mf);if(!fill_window(&current_window,cfg,session,window_id))return 0;
    /* Reserve the next window before entering graphics mode. This prevents a
       large /WINDOW setting from failing only after the first batch is sent. */
    if(!producer.finished&&!reserve_window(&previous_window,cfg))return 0;
    if(cfg->chain_width==2&&cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1)ensure_chain_cache();
    if(!vga_enter()){puts("Not enough memory for VGA buffer");return 0;}qr_delta_ready=0;
    if(!show_frame(&current_window,0,cfg,session,0,1,cfg->hold_ms,qr_mask)){
        vga_leave();puts("Could not build the first QR; reduce /WINDOW or /PAYLOAD.");return 0;}
    flush_keys();do{key=decision_key();}while(key!=13&&key!=27);
    if(key==27){vga_leave();return 0;}
    for(;;) {
        key=transmit(&current_window,cfg,session,0,0,0);
        if(!key){vga_leave();puts("QR settings cannot fit the frame; reduce payload or increase version.");return 0;}
wait_ack:
        /* A window is uninterrupted; controls are accepted only after this
           fresh prompt is visible. */
        flush_keys();
        show_eow(&current_window,cfg,session,have_selection);
        key=decision_key();
        if(key=='r'||key=='R')goto replay;
        if(key=='b'||key=='B'){if(have_previous){transmit(&previous_window,cfg,session,0,0,0);goto wait_ack;}goto wait_ack;}
        if(key=='m'||key=='M'){
            vga_leave();printf("Missing frames in window %lu (example 1,3-5; blank = all): ",current_window.id+1);
            if(!fgets(line,sizeof(line),stdin))line[0]=0;
            if(blank_line(line)){have_selection=0;chosen=0;rescue_round=0;memset(selected,0,sizeof(selected));}
            else {
                chosen=parse_ranges(line,selected,current_window.count);
                if(!chosen){puts("No valid frame numbers entered. Press a key.");getch();if(!vga_enter())return 0;qr_delta_ready=0;goto wait_ack;}
                have_selection=1;rescue_round=1;
            }
            if(!vga_enter())return 0;qr_delta_ready=0;transmit(&current_window,cfg,session,have_selection?selected:0,chosen,rescue_round);goto wait_ack;
        }
        if(key=='+'||key=='='){if(cfg->hold_ms>=50)cfg->hold_ms-=50;else cfg->hold_ms=0;goto replay;}
        if(key=='-'){cfg->hold_ms+=50;goto replay;}
        if(key==27){
            vga_leave();puts("PAUSED. C cancels; any other key resumes.");key=getch();
            if(key=='c'||key=='C')return 0;if(!vga_enter())return 0;qr_delta_ready=0;goto wait_ack;
        }
        if(key!=13)goto wait_ack;
        {Window swap=previous_window;previous_window=current_window;current_window=swap;}
        have_previous=1;have_selection=0;chosen=0;rescue_round=0;window_id++;
        if(!fill_window(&current_window,cfg,session,window_id)){vga_leave();
            completed_session=session;completed_frames=producer.global_index;completed_bytes=producer.total_bytes;return 1;}
        continue;
replay:
        if(have_selection)rescue_round++;
        transmit(&current_window,cfg,session,have_selection?selected:0,chosen,rescue_round);goto wait_ack;
    }
}

static int calibration(Config *cfg) {
    u32 session=(timer_ticks()^(u32)time(NULL))|1UL,seq=0;u16 key,n,i;u8 ascii,scan;
    u8 payload[256];char a[79],b[79];
    vga_use_320(cfg->qr_version==40&&cfg->module_pixels==1);if(!vga_enter())return 0;
    for(;;){
        n=cfg->frame_payload;if(n>sizeof(payload))n=sizeof(payload);
        memcpy(payload,"CAL1",4);put_u32(payload+4,seq);put_u16(payload+8,cfg->hold_ms);
        for(i=10;i<n;++i)payload[i]=(u8)(i*31+seq);
        n=make_frame(raw_frame,FK_CALIBRATION,0,session,0,seq,(u16)(seq&0xFFFF),100,
            0,0,payload,n);
        if(!qr_encode(raw_frame,n,cfg,0)){cfg->frame_payload-=16;continue;}
        sprintf(a,"CALIBRATION v%u %c scale %u payload %u hold %u ms",
            cfg->qr_version,"LMQH"[cfg->ecc],cfg->module_pixels,cfg->frame_payload,cfg->hold_ms);
        strcpy(b,"+/- hold  I invert  Esc done");
        vga_show_qr(qr_code,qrcodegen_getSize(qr_code),cfg->module_pixels,cfg->invert,a,b,(int)(seq&1));
        timer_wait_ms(cfg->hold_ms);seq++;
        if(!_bios_keybrd(_KEYBRD_READY))continue;key=_bios_keybrd(_KEYBRD_READ);ascii=(u8)key;scan=(u8)(key>>8);
        if(ascii==27||scan==0x01)break;if(ascii=='+'||ascii=='='||scan==0x0D||scan==0x4E){if(cfg->hold_ms>=50)cfg->hold_ms-=50;else cfg->hold_ms=0;}
        if(ascii=='-'||scan==0x0C||scan==0x4A)cfg->hold_ms+=50;
        if(ascii=='i'||ascii=='I'||scan==0x17)cfg->invert=!cfg->invert;
    }
    vga_leave();return 1;
}
static void benchmark(const char *path,Config *cfg) {
    static const u8 whitening_test[16]={0x9D,0x3B,0x19,0x23,0xA8,0xAC,0x39,0x89,0x3E,0xAD,0x32,0x29,0xF4,0x3D,0x3F,0xEE};
    FILE *f;u8 *b=producer.disk[0];u16 n;u32 bytes=0,crc=0,t0,t1,protocol_ms,encode_ms,build_ms,copy_ms,text_ms,frame_ms,useful;int i,encoded=0;u16 rawlen;
    {u8 record_test[100];u16 exact,refused;memset(record_test,0xCC,sizeof(record_test));
     exact=make_record(record_test+2,96,RT_SESSION,1,0,record_body,72);
     refused=make_record(record_test+2,96,RT_FILE_BEGIN,2,1,record_body,73);
     printf("Checked record builder: %s\n",exact==96&&!refused&&record_test[0]==0xCC&&record_test[1]==0xCC&&record_test[98]==0xCC&&record_test[99]==0xCC?"PASS":"FAIL");}
    t0=timer_ticks();timer_wait_ms(100);t1=timer_ticks();
    printf("PIT pacing self-test: requested 100 ms, measured %lu ms\n",
        timer_elapsed_ms(t0,t1));
    vga_use_320(cfg->qr_version==40&&cfg->module_pixels==1);f=fopen(path,"rb");if(!f){printf("Cannot open %s\n",path);return;}
    t0=timer_ticks();while((n=(u16)fread(b,1,sizeof(b),f))!=0){crc=crc32_update(crc,b,n);bytes+=n;}t1=timer_ticks();fclose(f);
    printf("Disk+CRC: %lu bytes in %lu ms = %lu B/s, CRC %08lX\n",bytes,timer_elapsed_ms(t0,t1),timer_elapsed_ms(t0,t1)?bytes*1000UL/timer_elapsed_ms(t0,t1):0,crc);
    memset(b,0,16);make_frame(raw_frame,FK_DATA,FF_WHITENED,0x6A67C69DUL,0,4,4,32,1,4510,b,16);
    printf("Payload whitening self-test: %s\n",memcmp(raw_frame+48,whitening_test,16)?"FAIL":"PASS");
    memset(b,0xA5,cfg->frame_payload);
    t0=timer_ticks();for(i=0;i<25;i++)rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,(u32)i,0,1,0,0,b,cfg->frame_payload);t1=timer_ticks();
    protocol_ms=timer_elapsed_ms(t0,t1)/25UL;
    printf("Config: QR v%u-%c, frame payload %u, raw QR bytes %u, scale %u\n",cfg->qr_version,"LMQH"[cfg->ecc],cfg->frame_payload,rawlen,cfg->module_pixels);
    printf("Protocol framing+CRC: 25 in %lu ms (%lu ms/frame)\n",timer_elapsed_ms(t0,t1),protocol_ms);
    if(cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1){
        u16 cw=qr_codeword_bytes(cfg),matrix_bytes=QR_BUFFER+1,j,diff_index=0;
        int code_ok=0,matrix_ok=0;u8 diff_fast=0,diff_canonical=0;
        qrcodegen_dosferSetCodewordsOnly(1);qrcodegen_dosferSetAlignedFast(1);memcpy(qr_temp,raw_frame,rawlen);
        if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0)){
            memcpy(producer.disk[1],qr_temp,cw);qrcodegen_dosferSetAlignedFast(0);memcpy(qr_temp,raw_frame,rawlen);
            if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0)){
                code_ok=!memcmp(producer.disk[1],qr_temp,cw);
                if(!code_ok)for(j=0;j<cw;++j)if(producer.disk[1][j]!=qr_temp[j]){
                    diff_index=j;diff_fast=producer.disk[1][j];diff_canonical=qr_temp[j];break;}
            }}
        qrcodegen_dosferSetCodewordsOnly(0);qrcodegen_dosferSetAlignedFast(1);memcpy(qr_temp,raw_frame,rawlen);
        if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0)){
            memcpy(producer.disk[1],qr_code,matrix_bytes);qrcodegen_dosferSetAlignedFast(0);memcpy(qr_temp,raw_frame,rawlen);
            if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0))matrix_ok=!memcmp(producer.disk[1],qr_code,matrix_bytes);}
        qrcodegen_dosferSetAlignedFast(1);qrcodegen_dosferSetCodewordsOnly(0);
        printf("Aligned V40 oracle: codewords %s, matrix %s\n",code_ok?"MATCH":"FAIL",matrix_ok?"MATCH":"FAIL");
        if(!code_ok)printf("  first codeword difference %u: fast %02X canonical %02X\n",
            diff_index,diff_fast,diff_canonical);
        if(!code_ok||!matrix_ok)return;
        {u8 *raw_a=producer.disk[1],*raw_b=raw_frame,*raw_x=producer.disk[0]+4096;
         u8 *enc_a=producer.disk[0]+8192,*enc_b=producer.disk[1]+4096;u8 header_xor[48];u16 j;
         int payload_ok=1,derive_ok=0;u32 lengths=((u32)cfg->frame_payload<<16)|cfg->frame_payload;
         _fmemset(record_body,0x3C,cfg->frame_payload);for(j=0;j<cfg->frame_payload;++j)chain_payload[j]=(u8)(b[j]^record_body[j]);
         make_frame(raw_a,FK_DATA,FF_WHITENED,0x6A67C69DUL,2,100,0,2,0,0,b,cfg->frame_payload);
         make_frame(raw_b,FK_DATA,FF_WHITENED,0x6A67C69DUL,2,101,1,2,0,0,record_body,cfg->frame_payload);
         make_frame(raw_x,FK_CHAIN_XOR,FF_PAIR_WHITENED,0x6A67C69DUL,2,100,0,2,lengths,0,chain_payload,cfg->frame_payload);
         for(j=48;j<rawlen;++j)if(raw_x[j]!=(u8)(raw_a[j]^raw_b[j])){payload_ok=0;break;}
         qrcodegen_dosferSetCodewordsOnly(1);memcpy(qr_temp,raw_a,rawlen);qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0);memcpy(enc_a,qr_temp,cw);
         memcpy(qr_temp,raw_b,rawlen);qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0);memcpy(enc_b,qr_temp,cw);
         memcpy(qr_temp,raw_x,rawlen);qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0);
         for(j=0;j<48;++j)header_xor[j]=(u8)(raw_a[j]^raw_b[j]^raw_x[j]);
         qrcodegen_dosferDeriveXorV40L(enc_a,enc_b,header_xor,qr_code);derive_ok=!memcmp(qr_temp,qr_code,cw);
         qrcodegen_dosferSetCodewordsOnly(0);
         printf("Affine XOR oracle: raw payload %s, codewords %s\n",payload_ok?"MATCH":"FAIL",derive_ok?"MATCH":"FAIL");
         if(!payload_ok||!derive_ok)return;}
    }
#ifdef DOSFER_PROFILE
    memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
#endif
    t0=timer_ticks();for(i=0;i<25;++i)if(qr_encode(raw_frame,rawlen,cfg,0))encoded++;t1=timer_ticks();
    if(encoded!=25){printf("QR encode: DOES NOT FIT (%d/25 encoded)\n",encoded);return;}
    encode_ms=timer_elapsed_ms(t0,t1)/25UL;
    printf("QR encode: 25 frames in %lu ms (%lu ms/frame)\n",timer_elapsed_ms(t0,t1),encode_ms);
#ifdef DOSFER_PROFILE
    printf("  pack %lu  ECC %lu  func1 %lu  data %lu  func2 %lu  mask %lu ms total\n",
        timer_elapsed_ms(0,dosferQrProfileTicks[0]),timer_elapsed_ms(0,dosferQrProfileTicks[1]),
        timer_elapsed_ms(0,dosferQrProfileTicks[2]),timer_elapsed_ms(0,dosferQrProfileTicks[3]),
        timer_elapsed_ms(0,dosferQrProfileTicks[4]),timer_elapsed_ms(0,dosferQrProfileTicks[5]));
#endif
    if(vga_enter()) {
        vga_benchmark_qr(qr_code,qrcodegen_getSize(qr_code),cfg->module_pixels,25,&build_ms,&copy_ms,&text_ms);
        vga_leave();
        printf("VGA packed build: 25 in %lu ms (%lu ms/frame)\n",build_ms,build_ms/25UL);
        printf("VGA retrace+copy: 25 in %lu ms (%lu ms/frame)\n",copy_ms,copy_ms/25UL);
        printf("VGA status draw: 25 in %lu ms (%lu ms/frame)\n",text_ms,text_ms/25UL);
        frame_ms=((protocol_ms+encode_ms)>cfg->hold_ms?(protocol_ms+encode_ms):cfg->hold_ms)+build_ms/25UL+copy_ms/25UL+text_ms/25UL;
        useful=cfg->frame_payload>28?cfg->frame_payload-28:0;
        printf("Projected pipeline: %lu ms/frame, %lu raw QR B/s, %lu file-data B/s\n",
            frame_ms,frame_ms?rawlen*1000UL/frame_ms:0,frame_ms?useful*1000UL/frame_ms:0);
    }
    if((cfg->module_pixels==2||(cfg->qr_version==40&&cfg->module_pixels==1))&&vga_enter()){
        u32 first_ms,steady_ms,real_schedule_ms=0,dirty_hash=0,full_hash=0,switch_hash=0;
        u16 real_schedule_data=0,real_schedule_displays=0;
        int delta,ok=1,redraw_ok=0,switch_ok=0,display_ok=0;u8 alt_mask=(u8)((qr_mask+1)&7);qr_delta_ready=0;
        rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,0,0,1,0,0,b,cfg->frame_payload);
        t0=timer_ticks();if(!qr_encode(raw_frame,rawlen,cfg,0)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Streaming benchmark","first frame",0,0))ok=0;t1=timer_ticks();first_ms=timer_elapsed_ms(t0,t1);qr_delta_ready=vga_delta_ready();
        if(ok){u16 groups,types;int rotation;vga_delta_stats(&groups,&types,&rotation);printf("V40 renderer map: rotation %d, %u destination groups, %u LUT types\n",rotation*90,groups,types);}
#ifdef DOSFER_PROFILE
        memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
        memset(dosferProtocolProfileTicks,0,sizeof(dosferProtocolProfileTicks));
        memset(dosferVgaProfileTicks,0,sizeof(dosferVgaProfileTicks));
#endif
        t0=timer_ticks();for(i=1;i<25&&ok;++i){rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,(u32)i,0,1,0,0,b,cfg->frame_payload);delta=qr_delta_ready;if(!qr_encode(raw_frame,rawlen,cfg,delta)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Streaming benchmark","delta frame",i&1,delta))ok=0;qr_delta_ready=vga_delta_ready();}t1=timer_ticks();steady_ms=timer_elapsed_ms(t0,t1);
#ifdef DOSFER_PROFILE
        printf("  steady protocol x24: header %lu  payload+CRC %lu  header CRC %lu ms total\n",
            timer_elapsed_ms(0,dosferProtocolProfileTicks[0]),timer_elapsed_ms(0,dosferProtocolProfileTicks[1]),
            timer_elapsed_ms(0,dosferProtocolProfileTicks[2]));
        printf("  steady QR x24: pack %lu  ECC %lu  func1 %lu  data %lu  func2 %lu  mask %lu ms total\n",
            timer_elapsed_ms(0,dosferQrProfileTicks[0]),timer_elapsed_ms(0,dosferQrProfileTicks[1]),
            timer_elapsed_ms(0,dosferQrProfileTicks[2]),timer_elapsed_ms(0,dosferQrProfileTicks[3]),
            timer_elapsed_ms(0,dosferQrProfileTicks[4]),timer_elapsed_ms(0,dosferQrProfileTicks[5]));
        printf("  steady VGA x24: render %lu  upload %lu  retrace %lu  flip %lu  status %lu ms total\n",
            timer_elapsed_ms(0,dosferVgaProfileTicks[0]),timer_elapsed_ms(0,dosferVgaProfileTicks[1]),
            timer_elapsed_ms(0,dosferVgaProfileTicks[2]),timer_elapsed_ms(0,dosferVgaProfileTicks[3]),
            timer_elapsed_ms(0,dosferVgaProfileTicks[4]));
#endif
        if(ok){dirty_hash=vga_screen_hash();display_ok=vga_display_matches();if(qr_encode(raw_frame,rawlen,cfg,0)&&vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Redraw verification","full redraw",0,0)){full_hash=vga_screen_hash();redraw_ok=dirty_hash==full_hash;
            if(qr_encode_mask(raw_frame,rawlen,cfg,0,alt_mask)&&vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Rescue verification","alternate mask",0,0)&&qr_encode_mask(raw_frame,rawlen,cfg,0,qr_mask)&&vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Redraw verification","full redraw",0,0)){switch_hash=vga_screen_hash();switch_ok=switch_hash==full_hash;}}}
        if(ok){u32 du,dc,dt;vga_benchmark_delta(qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,24,&du,&dc,&dt);
            printf("Delta VGA: update %lu, retrace+partial copy %lu, status %lu ms/frame\n",du/24UL,dc/24UL,dt/24UL);}
        if(ok&&cfg->chain_width==2&&cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1) {
            Config scfg=*cfg;u32 schedule_ms,schedule_hash,canonical_hash;u32 schedule_fps100;
            int schedule_ok=1,affine_screen_ok=0;u16 si;
            scfg.hold_ms=0;current_window.id=7;current_window.count=13;
            for(si=0;si<current_window.count;++si) {
                PendingFrame *sf=&current_window.frames[si];
                sf->payload=(si&1)?record_body:b;sf->payload_len=scfg.frame_payload;
                sf->payload_capacity=scfg.frame_payload;sf->stream_id=1;
                sf->stream_offset=(u32)si*scfg.frame_payload;sf->global_index=200UL+si;
            }
            ensure_chain_cache();chain_cache_valid=0;qr_delta_ready=0;last_visible_tick=0;
            if(!show_frame(&current_window,0,&scfg,0x6A67C69DUL,0,0,0,qr_mask))schedule_ok=0;
#ifdef DOSFER_PROFILE
            memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
            memset(dosferProtocolProfileTicks,0,sizeof(dosferProtocolProfileTicks));
            memset(dosferVgaProfileTicks,0,sizeof(dosferVgaProfileTicks));
#endif
            t0=timer_ticks();
            for(si=0;si<12&&schedule_ok;++si) {
                if(!show_chain(&current_window,si,&scfg,0x6A67C69DUL,0) ||
                   !show_frame(&current_window,(u16)(si+1),&scfg,0x6A67C69DUL,0,0,0,qr_mask))
                    schedule_ok=0;
            }
            t1=timer_ticks();schedule_ms=timer_elapsed_ms(t0,t1);
            if(schedule_ok) {
                real_schedule_ms=schedule_ms;
                real_schedule_data=12;real_schedule_displays=24;
                schedule_fps100=schedule_ms?2400000UL/schedule_ms:0;
                printf("V40 DATA/XOR schedule: 24 displays in %lu ms = %lu ms/frame (%lu.%02lu FPS)\n",
                    schedule_ms,schedule_ms/24UL,schedule_fps100/100UL,schedule_fps100%100UL);
#ifdef DOSFER_PROFILE
                printf("  schedule protocol: header %lu  payload+CRC %lu  header CRC %lu ms total\n",
                    timer_elapsed_ms(0,dosferProtocolProfileTicks[0]),timer_elapsed_ms(0,dosferProtocolProfileTicks[1]),
                    timer_elapsed_ms(0,dosferProtocolProfileTicks[2]));
                printf("  schedule QR: pack %lu  ECC %lu  func1 %lu  data %lu  func2 %lu  mask %lu ms total\n",
                    timer_elapsed_ms(0,dosferQrProfileTicks[0]),timer_elapsed_ms(0,dosferQrProfileTicks[1]),
                    timer_elapsed_ms(0,dosferQrProfileTicks[2]),timer_elapsed_ms(0,dosferQrProfileTicks[3]),
                    timer_elapsed_ms(0,dosferQrProfileTicks[4]),timer_elapsed_ms(0,dosferQrProfileTicks[5]));
                printf("  schedule VGA: render %lu  upload %lu  retrace %lu  flip %lu  status %lu ms total\n",
                    timer_elapsed_ms(0,dosferVgaProfileTicks[0]),timer_elapsed_ms(0,dosferVgaProfileTicks[1]),
                    timer_elapsed_ms(0,dosferVgaProfileTicks[2]),timer_elapsed_ms(0,dosferVgaProfileTicks[3]),
                    timer_elapsed_ms(0,dosferVgaProfileTicks[4]));
#endif
                if(show_frame(&current_window,11,&scfg,0x6A67C69DUL,0,0,0,qr_mask)&&
                   show_chain(&current_window,11,&scfg,0x6A67C69DUL,0)) {
                    u32 verify_lengths=((u32)current_window.frames[11].payload_len<<16)|
                        current_window.frames[12].payload_len;
                    schedule_hash=vga_screen_hash();
                    xor_frame_payload(&current_window.frames[11],&current_window.frames[12]);
                    make_frame(raw_frame,FK_CHAIN_XOR,FF_PAIR_WHITENED,0x6A67C69DUL,current_window.id,
                        current_window.frames[11].global_index,11,current_window.count,verify_lengths,0,
                        chain_payload,scfg.frame_payload);
                    if(qr_encode(raw_frame,2952,&scfg,0)&&
                       vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(&scfg),177,1,scfg.invert,
                           "Affine verification","canonical rebuild",0,0)) {
                        canonical_hash=vga_screen_hash();affine_screen_ok=schedule_hash==canonical_hash;
                    }
                }
                printf("V40 affine framebuffer: %s\n",affine_screen_ok?"MATCH":"FAIL");
            } else puts("V40 DATA/XOR schedule: FAILED");
        } else if(ok&&cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1) {
            Config scfg=*cfg;u32 schedule_ms;u16 si,group=redundancy_group(cfg),half=(u16)(cfg->chain_width/2),first=0,count=0;
            u16 data_count=cfg->window_frames,display_count=data_count;int schedule_ok=1;
            scfg.hold_ms=0;current_window.id=7;current_window.count=data_count;
            for(si=0;si<data_count;++si) {
                PendingFrame *sf=&current_window.frames[si];
                sf->payload=(si&1)?record_body:b;sf->payload_len=scfg.frame_payload;
                sf->payload_capacity=scfg.frame_payload;sf->stream_id=1;
                sf->stream_offset=(u32)si*scfg.frame_payload;sf->global_index=200UL+si;
            }
            if(group)display_count+=(u16)((data_count+group-1)/group);
            else if(half)display_count+=(u16)((data_count-1)/half);
            qr_delta_ready=0;last_visible_tick=0;
            if(!show_frame(&current_window,0,&scfg,0x6A67C69DUL,0,0,0,qr_mask))schedule_ok=0;
#ifdef DOSFER_PROFILE
            memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
            memset(dosferProtocolProfileTicks,0,sizeof(dosferProtocolProfileTicks));
            memset(dosferVgaProfileTicks,0,sizeof(dosferVgaProfileTicks));
#endif
            t0=timer_ticks();
            for(si=0;si<data_count&&schedule_ok;++si) {
                if(!show_frame(&current_window,si,&scfg,0x6A67C69DUL,0,0,0,qr_mask))schedule_ok=0;
                if(group){if(!count)first=si;count++;
                    if(count==group||si+1==data_count){
                        if(!show_block_parity(&current_window,first,count,&scfg,0x6A67C69DUL,0))schedule_ok=0;
                        count=0;}}
                else if(half&&((si+1)%half)==0){first=(u16)(si+1-half);
                    if(first+half<data_count){count=(u16)(data_count-first);
                        if(count>cfg->chain_width)count=cfg->chain_width;
                        if(!show_block_parity(&current_window,first,count,&scfg,0x6A67C69DUL,0))schedule_ok=0;}}
            }
            t1=timer_ticks();schedule_ms=timer_elapsed_ms(t0,t1);
            if(schedule_ok){u32 fps100=schedule_ms?(u32)display_count*100000UL/schedule_ms:0;
                real_schedule_ms=schedule_ms;
                real_schedule_data=data_count;real_schedule_displays=display_count;
                printf("V40 %s schedule: %u DATA / %u displays in %lu ms (%lu.%02lu FPS)\n",
                    redundancy_name(cfg),data_count,display_count,schedule_ms,
                    fps100/100UL,fps100%100UL);
#ifdef DOSFER_PROFILE
                printf("  schedule protocol: header %lu  payload+CRC %lu  header CRC %lu ms total\n",
                    timer_elapsed_ms(0,dosferProtocolProfileTicks[0]),timer_elapsed_ms(0,dosferProtocolProfileTicks[1]),
                    timer_elapsed_ms(0,dosferProtocolProfileTicks[2]));
                printf("  schedule QR: pack %lu  ECC %lu  func1 %lu  data %lu  func2 %lu  mask %lu ms total\n",
                    timer_elapsed_ms(0,dosferQrProfileTicks[0]),timer_elapsed_ms(0,dosferQrProfileTicks[1]),
                    timer_elapsed_ms(0,dosferQrProfileTicks[2]),timer_elapsed_ms(0,dosferQrProfileTicks[3]),
                    timer_elapsed_ms(0,dosferQrProfileTicks[4]),timer_elapsed_ms(0,dosferQrProfileTicks[5]));
                printf("  schedule VGA: render %lu  upload %lu  retrace %lu  flip %lu  status %lu ms total\n",
                    timer_elapsed_ms(0,dosferVgaProfileTicks[0]),timer_elapsed_ms(0,dosferVgaProfileTicks[1]),
                    timer_elapsed_ms(0,dosferVgaProfileTicks[2]),timer_elapsed_ms(0,dosferVgaProfileTicks[3]),
                    timer_elapsed_ms(0,dosferVgaProfileTicks[4]));
#endif
            } else puts("V40 parity schedule: FAILED");
        }
        vga_leave();if(ok){u32 rate_ms=real_schedule_ms?real_schedule_ms:steady_ms;
            u32 fps100=rate_ms?(u32)(real_schedule_displays?real_schedule_displays:24)*100000UL/rate_ms:0;
            u32 state_fps100=steady_ms?2400000UL/steady_ms:0;
            u32 displayed=cfg->window_frames,effective;
            if(cfg->chain_width)
                displayed=cfg->window_frames+(cfg->window_frames-1)/(cfg->chain_width/2)+
                    (chain_anchor?(cfg->window_frames-1)/chain_anchor:0);
            else {u16 group=redundancy_group(cfg);if(group)displayed+=(cfg->window_frames+group-1)/group;}
            effective=rate_ms?useful*1000UL*(real_schedule_data?real_schedule_data:24)/rate_ms:0;
            printf("V%u stateful: first %lu ms, next 24 in %lu ms = %lu ms/frame (%lu.%02lu FPS)\n",cfg->qr_version,first_ms,steady_ms,steady_ms/24UL,state_fps100/100UL,state_fps100%100UL);
            printf("V%u effective schedule: %lu data / %lu displays, %lu file-data B/s\n",cfg->qr_version,(u32)cfg->window_frames,displayed,effective);
            if(cfg->chain_width){u32 equations=(cfg->window_frames-1)/(cfg->chain_width/2);
                u32 data100=fps100*cfg->window_frames/displayed,xor100=fps100*equations/displayed;
                printf("Displayed rates: DATA %lu.%02lu/s, C%u parity %lu.%02lu/s, anchors every %u\n",data100/100UL,data100%100UL,cfg->chain_width,xor100/100UL,xor100%100UL,chain_anchor);}
            else if(redundancy_group(cfg)){u32 data100=rate_ms?(u32)real_schedule_data*100000UL/rate_ms:0;
                u32 parity100=rate_ms?(u32)(real_schedule_displays-real_schedule_data)*100000UL/rate_ms:0;
                printf("Displayed rates: DATA %lu.%02lu/s, parity %lu.%02lu/s\n",
                    data100/100UL,data100%100UL,parity100/100UL,parity100%100UL);}
            printf("V%u dirty redraw: %s (%08lX/%08lX), displayed page: %s\n",cfg->qr_version,redraw_ok?"MATCH":"FAIL",dirty_hash,full_hash,display_ok?"MATCH":"FAIL");printf("V%u rescue mask switch: %s (%08lX/%08lX)\n",cfg->qr_version,switch_ok?"MATCH":"FAIL",switch_hash,full_hash);}else puts("Stateful benchmark: unavailable (not enough DOS memory)");
    }
}
static const char *option_value(const char *arg,const char *name) {
    size_t n=strlen(name);const char *p;
    if(arg[0]!='/'&&arg[0]!='-')return 0;p=arg+1;
    if(strnicmp(p,name,n)||(p[n]!=':'&&p[n]!='='))return 0;
    return p+n+1;
}
static int option_number(const char *arg,const char *a,const char *b,long lo,long hi,long *out) {
    const char *p=option_value(arg,a);char *end;long v;
    if(!p&&b)p=option_value(arg,b);if(!p)return 0;
    v=strtol(p,&end,10);if(!*p||*end||v<lo||v>hi)return -1;*out=v;return 1;
}
static int redundancy_value(Config *cfg,const char *p) {
    char *end;long v;
    if(!p||!*p)return -1;
    if(toupper(*p)=='C') {
        v=strtol(p+1,&end,10);
        if(!p[1]||*end||v<2||v>MAX_WINDOW||(v&1))return -1;
        cfg->chain_width=(u8)v;cfg->redundancy=0;return 1;
    }
    v=strtol(p,&end,10);
    if(*end||v<0||v>MAX_WINDOW)return -1;
    cfg->redundancy=(u8)v;cfg->chain_width=0;return 1;
}
static int config_option(Config *cfg,const char *arg) {
    long v;int rc;const char *p;
    p=option_value(arg,"RE");if(p)return redundancy_value(cfg,p);
    rc=option_number(arg,"V","VERSION",5,40,&v);if(rc){if(rc>0)cfg->qr_version=(u8)v;return rc;}
    p=option_value(arg,"ECC");if(p){char *e;if(p[1])return -1;e=strchr("LMQH",toupper(p[0]));if(!e)return -1;cfg->ecc=(u8)(e-"LMQH");return 1;}
    rc=option_number(arg,"SCALE",0,1,6,&v);if(rc){if(rc>0)cfg->module_pixels=(u8)v;return rc;}
    rc=option_number(arg,"PAYLOAD","P",96,MAX_FRAME_PAYLOAD,&v);if(rc){if(rc>0)cfg->frame_payload=(u16)v;return rc;}
    rc=option_number(arg,"HOLD","SPEED",0,60000,&v);if(rc){if(rc>0)cfg->hold_ms=(u16)v;return rc;}
    rc=option_number(arg,"WINDOW","W",4,MAX_WINDOW,&v);if(rc){if(rc>0)cfg->window_frames=(u16)v;return rc;}
    rc=option_number(arg,"REPEAT","R",1,20,&v);if(rc){if(rc>0)cfg->repetitions=(u8)v;return rc;}
    rc=option_number(arg,"MASK",0,0,7,&v);if(rc){if(rc>0)qr_mask=(u8)v;return rc;}
    rc=option_number(arg,"ANCHOR",0,0,MAX_WINDOW,&v);if(rc){if(rc>0)chain_anchor=(u16)v;return rc;}
    if(!stricmp(arg,"/INVERT")||!stricmp(arg,"-INVERT")){cfg->invert=1;return 1;}
    if(!stricmp(arg,"/NOINVERT")||!stricmp(arg,"-NOINVERT")){cfg->invert=0;return 1;}
    if(!stricmp(arg,"/BEEP")||!stricmp(arg,"-BEEP")){cfg->speaker=1;return 1;}
    if(!stricmp(arg,"/NOBEEP")||!stricmp(arg,"-NOBEEP")){cfg->speaker=0;return 1;}
    return 0;
}
static int split_re_option(const char *arg) {
    return !stricmp(arg,"/RE")||!stricmp(arg,"-RE");
}
static void usage(const Config *cfg) {
    puts("DOSfer 1.3 - optical DOS-to-Android file transfer");
    puts("DOSFER [options] file_or_directory [more paths ...]");
    puts("DOSFER /CAL [options]");
    puts("DOSFER /BENCH file [options]");
    puts("");
    puts("Default: V40-L, 320x200, 2904-byte payload, hold 0, window 32, /RE:7");
    puts("/RE:n or /RE n        n DATA + 1 XOR parity; 0 disables (default 7)");
    puts("/RE:Ck or /RE Ck      Even chain width C2..C64; k/2 must be < window");
    puts("/ANCHOR:n            Repeat each nth data frame in chain mode; 0 disables");
    puts("Advanced diagnostics: /V:n /ECC:L|M|Q|H /SCALE:n /PAYLOAD:n");
    puts("/HOLD:ms or /SPEED:ms  Minimum frame hold 0..60000 ms");
    puts("/WINDOW:n or /W:n    Frames per acknowledged batch 4..64");
    puts("/REPEAT:n or /R:n    Full passes per batch 1..20");
    puts("/MASK:n              Fixed QR mask 0..7 (default 0; payload is whitened)");
    puts("/INVERT /NOINVERT    Select black/white polarity");
    puts("/BEEP /NOBEEP        End-of-batch sound");
    printf("Current: /V:%u /ECC:%c /SCALE:%u /PAYLOAD:%u /HOLD:%u /WINDOW:%u /REPEAT:%u /MASK:%u /RE:%s %s %s\n",
        cfg->qr_version,"LMQH"[cfg->ecc],cfg->module_pixels,cfg->frame_payload,
        cfg->hold_ms,cfg->window_frames,cfg->repetitions,qr_mask,redundancy_name(cfg),
        cfg->invert?"/INVERT":"/NOINVERT",cfg->speaker?"/BEEP":"/NOBEEP");
    puts("Example: DOSFER /HOLD:0 /W:32 /RE:C8 FILE.ZIP");
    puts("The first QR waits for Enter so you can focus the camera; Esc cancels.");
    puts("M rescues missing frames with adaptive hold/mask; R repeats that set.");
    puts("Enter a blank M list to clear it and replay the complete window.");
    puts("Every batch stops for Enter/R/M/B; command-line options never auto-advance.");
}
int main(int argc,char **argv) {
    Config cfg;FILE *mf;int i,rc,action=0,path_count=0;char path[PATH_BYTES],again[8];const char *bench_path=0;u32 est;
    default_config(&cfg);
    for(i=1;i<argc;++i) {
        if(!stricmp(argv[i],"/?")||!stricmp(argv[i],"/HELP")||!stricmp(argv[i],"-HELP")){usage(&cfg);return 0;}
        if(!stricmp(argv[i],"/CAL")||!stricmp(argv[i],"-CAL")){if(action&&action!=1){puts("Choose either /CAL or /BENCH.");return 1;}action=1;continue;}
        if(!stricmp(argv[i],"/BENCH")||!stricmp(argv[i],"-BENCH")){if(action&&action!=2){puts("Choose either /CAL or /BENCH.");return 1;}action=2;continue;}
        if(split_re_option(argv[i])) {
            if(i+1>=argc||redundancy_value(&cfg,argv[++i])<0){puts("Invalid /RE value.");return 1;}
            continue;
        }
        rc=config_option(&cfg,argv[i]);if(rc<0){printf("Invalid option value: %s\n",argv[i]);return 1;}if(rc)continue;
        if(argv[i][0]=='/'||argv[i][0]=='-'){printf("Unknown option: %s\n",argv[i]);return 1;}
        if(action==2&&!bench_path)bench_path=argv[i];else path_count++;
    }
    if(!validate_config(&cfg))return 1;
    if(action==1){if(path_count||bench_path){puts("/CAL does not take a file path.");return 1;}return calibration(&cfg)?0:1;}
    if(action==2){if(!bench_path||path_count){puts("Usage: DOSFER /BENCH file [options]");return 1;}benchmark(bench_path,&cfg);free_chain_cache();vga_leave();return 0;}
    mf=fopen(MANIFEST_NAME,"w+b");if(!mf){puts("Cannot create DOSFER.$$$ in current directory.");return 1;}
    if(path_count){for(i=1;i<argc;++i){if(split_re_option(argv[i])){++i;continue;}
        if(argv[i][0]!='/'&&argv[i][0]!='-')if(!add_selection(mf,argv[i])){fclose(mf);remove(MANIFEST_NAME);return 1;}}}
    else do {printf("File or directory: ");if(!fgets(path,sizeof(path),stdin))break;path[strcspn(path,"\r\n")]=0;
        if(!add_selection(mf,path)){fclose(mf);remove(MANIFEST_NAME);return 1;}
        printf("Add another? [y/N] ");fgets(again,sizeof(again),stdin);
    } while(again[0]=='y'||again[0]=='Y');
    if(!selected_files&&!selected_dirs){usage(&cfg);fclose(mf);remove(MANIFEST_NAME);return 1;}
    est=1+selected_dirs+selected_files*2+selected_bytes/(cfg.frame_payload-28)+1;
    printf("Selected: %lu files, %lu directories, %lu bytes, approximately %lu QR frames.\n",selected_files,selected_dirs,selected_bytes,est);
    printf("Settings: QR v%u-%c mask %u, %u px modules, %u ms hold, window %u, repeats %u, redundancy %s.\n",cfg.qr_version,"LMQH"[cfg.ecc],qr_mask,cfg.module_pixels,cfg.hold_ms,cfg.window_frames,cfg.repetitions,redundancy_name(&cfg));
    puts("Preparing the first QR code...");
    rc=run_transfer(mf,&cfg);sender_cleanup();fclose(mf);remove(MANIFEST_NAME);
    if(rc){printf("Transfer complete. Session %08lX, %lu frames, %lu bytes. Returning to DOS.\n",
        completed_session,completed_frames,completed_bytes);fflush(stdout);exit_to_dos(0);return 0;}
    return 1;
}
