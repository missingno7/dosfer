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
/* spool I/O, payload construction, group preparation, playback, useful hold
 * work, idle hold time, correction preparation, replay preparation. */
static u32 dosferPlaneProfileTicks[8];
#endif

#define MANIFEST_NAME "DOSFER.$$$"
#define DISK_BUFFER 16384
#define DISK_IO_CHUNK 1024
#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(40)
#define QR40_CODEWORDS 3706
#define DOSFER_BUILD_ID "plane-stream-r26-doublebuf-noblack"

typedef struct {
    FILE *manifest, *source;
    ManifestEntry entry;
    int state, have_entry, finished;
    u32 record_id, global_index, file_offset, file_crc;
    u32 file_count, dir_count, total_bytes;
    u16 disk_len[2], disk_pos[2];
    int disk_slot, io_error;
} Producer;

static Producer producer;
/* Dual 16 KiB read-ahead is too large for DGROUP once QR/RS near tables and
 * the 320 shadow page share the segment. Keep the ring in far BSS and stage
 * stdio through a small near chunk. */
static u8 __far producer_disk[2][DISK_BUFFER];
static u8 disk_io[DISK_IO_CHUNK];
static Window __far current_window, previous_window;
static int have_previous;
/* A packed V40 matrix is 3,918 bytes. Keeping both matrices in DGROUP left
 * less than one stack frame beneath the 64 KiB segment limit, so the first full
 * QR expansion could overwrite runtime state immediately after RS returned.
 * These are long-lived working buffers, not hot scalar data: place them in
 * far storage and leave DGROUP/stack room for the encoder. */
static u8 __far qr_temp[QR_BUFFER+1], qr_code[QR_BUFFER+1];
/* Prepacked V40-L input is 2,956 data codewords. EncodePrepacked only reads
 * that prefix (ECC lands in a near temporary), so this size is exact. */
static u8 __far qr_prepack_data[2956];
static u8 __far raw_frame[MAX_QR_BYTES];
static u8 __far record_body[MAX_FRAME_PAYLOAD];
static u32 selected_files, selected_dirs, selected_bytes;
static u32 completed_session,completed_frames,completed_bytes;
static u32 last_visible_tick;
static int qr_delta_ready;
static u8 qr_mask=0;
static int encoded_qr_mask=-1;
static u16 chain_anchor=0;
static u8 chain_payload[MAX_FRAME_PAYLOAD];
/* The live PLANE backend has a fixed V40-L working set. Keep it in far BSS so
 * startup does not depend on DOS heap/MCB state. */
static u8 __far stream_record_payload[MAX_FRAME_PAYLOAD];
static u8 __far stream_plane_codewords[6][QR40_CODEWORDS];
/* Two independent copies, one per resident slot.  The streaming backend
 * ping-pongs between slot 0 and slot 1 so the previous group's symbol stays
 * on screen while the next group is being built; each slot therefore needs
 * its own correction-patch buffer so preparing group N+1 cannot clobber the
 * patches still needed to restore group N-1's plane after it goes dark. */
static u16 __far stream_correction_offsets[2][VGA_PLANE_MAX_CORRECTION_PATCHES];
static u8 __far stream_correction_xor[2][VGA_PLANE_MAX_CORRECTION_PATCHES];
static FILE *debug_trace;
static u8 __far *chain_left_codewords,*chain_right_codewords,*chain_cached_raw;
static int chain_cache_valid;
static u32 chain_cache_session,chain_cache_window,chain_cache_global;
static u16 chain_cache_rawlen,chain_cache_index;
static u8 chain_cache_mask;
static int transmit(const Window *w,const Config *cfg,u32 session,const u8 *selected,u16 chosen,u16 rescue_round,int focus_first_plane_c1);
static void flush_keys(void);
static int decision_key(void);

/* Foreground-only crash breadcrumb trail.  This is deliberately ordinary DOS
 * file I/O, never called from the timer/retrace path. */
static void trace_stage(const char *stage) {
    if(!debug_trace)return;
    fprintf(debug_trace,"%s\r\n",stage);fflush(debug_trace);
}

enum { PLANE_SLOT_FREE, PLANE_SLOT_PREPARING, PLANE_SLOT_READY, PLANE_SLOT_PLAYING };
typedef struct {
    u16 slot_start,first_window_index;
    u32 window_id,group_global,correction_restore_hash;
    u8 slot,width,valid_mask,state,prepare_stage,parity_correction_applied;
    u16 correction_patch_count;
    u16 far *correction_patch_offset;
    u8 far *correction_patch_xor;
    u32 prepare_started,ready_tick;
} PlaneGroup;
typedef struct {
    u8 headers[4][FRAME_HEADER_SIZE];
    u8 far *codewords[4];
    u8 far *zero_codewords,*correction_codewords;
} PlaneWork;
typedef struct {
    PlaneGroup slots[8];
    PlaneWork work;
    u16 page_step,next_first,total_groups,ready_count,min_ready;
    u8 head,tail,preparing;
    u32 starvation_count,max_chunk_ticks,intergroup_ticks,visible_ticks;
    u16 visible_transitions;
} PlaneQueue;
/* The live stream owns exactly one record payload.  Logical window size is
 * protocol metadata; it is never an allocation multiplier.  The historical
 * Window/spool implementation remains below only for the acknowledgement and
 * rescue UI while this streaming backend is brought up. */
typedef struct {
    PendingFrame record;
    u32 session,total_records,window_id;
    u16 window_count,window_index;
} PlaneStream;
static PlaneQueue __far plane_queue;
static u16 plane_last_min_ready;
static u32 plane_last_starvation,plane_last_max_chunk,plane_last_intergroup,plane_last_visible;
static u16 plane_last_visible_count;

static void producer_close(void) {
    if(producer.source){fclose(producer.source);producer.source=0;}
}
static void window_close_spool(Window *w) {
    if(w->spool){fclose(w->spool);w->spool=0;}
    if(w->spool_name[0]){remove(w->spool_name);w->spool_name[0]=0;}
}
static void free_window(Window *w) {
    u16 i;
    window_close_spool(w);
    for(i=0;i<MAX_WINDOW;++i)if(w->frames[i].payload&&!w->frames[i].spooled){
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
    free_chain_cache();vga_leave();if(debug_trace){fclose(debug_trace);debug_trace=0;}
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
       cfg->redundancy>MAX_WINDOW||cfg->plane_width>4||
       (cfg->chain_width&&(cfg->chain_width<2||cfg->chain_width>MAX_WINDOW||
        (cfg->chain_width&1)||cfg->chain_width/2>=cfg->window_frames))||!data_bytes) {
        puts("Invalid sender configuration values.");return 0;
    }
    if(!cfg->plane_width) {
        puts("DOSFER production transfer requires /RE:PLANE3 or /RE:PLANE4.");return 0;
    }
    if(cfg->plane_width && (cfg->qr_version!=40||cfg->ecc!=0||cfg->module_pixels!=1||
        cfg->frame_payload!=2904)) {
        puts("/RE:PLANE3 and /RE:PLANE4 require V40-L, /SCALE:1 and /PAYLOAD:2904.");return 0;
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
    c->window_frames=32; c->speaker=1;c->plane_width=4;
}
static u16 redundancy_group(const Config *c) {
    return (c->chain_width||c->plane_width)?0:c->redundancy;
}
static const char *redundancy_name(const Config *c) {
    static char name[12];
    if(c->plane_width)sprintf(name,"PLANE%u",c->plane_width);
    else if(c->chain_width)sprintf(name,"C%u",c->chain_width);
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
static u16 disk_fill_slot(int s) {
    u16 got=0,n;
    while(got<DISK_BUFFER) {
        u16 chunk=(u16)(DISK_BUFFER-got);
        if(chunk>DISK_IO_CHUNK)chunk=DISK_IO_CHUNK;
        n=(u16)fread(disk_io,1,chunk,producer.source);
        if(!n)break;
        _fmemcpy(producer_disk[s]+got,disk_io,n);
        got=(u16)(got+n);
        if(n<chunk)break;
    }
    return got;
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
        producer.disk_len[0]=disk_fill_slot(0);
        producer.disk_len[1]=disk_fill_slot(1);
        if(ferror(producer.source))producer.io_error=1;
    }
}
static u16 read_piece(u8 far *out,u16 want) {
    u16 got=0,n; int s;
    while(got<want && producer.file_offset<producer.entry.size) {
        s=producer.disk_slot;
        if(producer.disk_pos[s]>=producer.disk_len[s]) {
            producer.disk_len[s]=disk_fill_slot(s);
            producer.disk_pos[s]=0;
            if(ferror(producer.source))producer.io_error=1;
            if(!producer.disk_len[s]) break;
        }
        n=(u16)(producer.disk_len[s]-producer.disk_pos[s]);
        if(n>want-got)n=(u16)(want-got);
        _fmemcpy(out+got,producer_disk[s]+producer.disk_pos[s],n);
        producer.disk_pos[s]+=n; got+=n; producer.file_offset+=n;
        if(producer.disk_pos[s]>=producer.disk_len[s]) producer.disk_slot^=1;
    }
    producer.file_crc=crc32_update(producer.file_crc,out,got); return got;
}
static u16 path_meta(u8 far *b,const ManifestEntry *e,int with_size) {
    u16 n=(u16)strlen(e->relative),p=0;
    b[p++]=e->attributes;b[p++]=0;put_u16(b+p,e->dos_date);p+=2;
    put_u16(b+p,e->dos_time);p+=2;
    if(with_size){put_u32(b+p,e->size);p+=4;}
    put_u16(b+p,n);p+=2;_fmemcpy(b+p,e->relative,n);return (u16)(p+n);
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
        put_u32(body,(u32)time(NULL)); put_u16(body+4,6); _fmemcpy(body+6,"DOSFER",6);
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
/* PLANE windows retain metadata in RAM and exact variable-length records in
 * a sequential DOS spool.  This deliberately avoids two 128x2904 payload
 * arrays in conventional memory while keeping current/previous replay exact. */
static int fill_window_spooled(Window *w,const Config *cfg,u32 session,u32 id) {
    PendingFrame work;PendingFrame *dst;int rc;u16 len;long off;
    memset(&work,0,sizeof(work));window_close_spool(w);w->count=0;w->id=id;
    sprintf(w->spool_name,"DFW%u.$$$",(unsigned)(id&1));remove(w->spool_name);
    if(!(w->spool=fopen(w->spool_name,"w+b"))){puts("Cannot create PLANE replay spool.");return 0;}
    while(w->count<cfg->window_frames) {
        rc=producer_next(&work,cfg,session);
        if(rc<0){if(work.payload)_ffree(work.payload);return 0;}
        if(!rc)break;
        off=ftell(w->spool);len=work.payload_len;
        /* DOS stdio must receive a near buffer; chain_payload is the existing
           fixed-size near staging area, so this adds no DGROUP pressure. */
        _fmemcpy(chain_payload,work.payload,len);
        if(off<0||fwrite(&len,1,sizeof(len),w->spool)!=sizeof(len)||
           fwrite(chain_payload,1,len,w->spool)!=len){
            puts("PLANE replay spool write failed.");if(work.payload)_ffree(work.payload);return 0;
        }
        dst=&w->frames[w->count++];memset(dst,0,sizeof(*dst));
        dst->payload_len=len;dst->stream_id=work.stream_id;dst->stream_offset=work.stream_offset;
        dst->global_index=work.global_index;dst->spool_offset=(u32)off;dst->spooled=1;
    }
    if(work.payload)_ffree(work.payload);
    if(ferror(w->spool)||!w->count){window_close_spool(w);return 0;}
    fflush(w->spool);return 1;
}
static int window_read_payload(const Window *w,u16 index,u8 far *out) {
    const PendingFrame *f;
    u16 len;
#ifdef DOSFER_PROFILE
    u32 t=timer_ticks();
#endif
    if(index>=w->count)return 0;f=&w->frames[index];
    if(!f->spooled){if(f->payload)_fmemcpy(out,f->payload,f->payload_len);return f->payload!=0;}
    if(!w->spool||fseek(w->spool,(long)f->spool_offset,SEEK_SET)||
       fread(&len,1,sizeof(len),w->spool)!=sizeof(len)||len!=f->payload_len||
       fread(chain_payload,1,len,w->spool)!=len)return 0;
    if(out!=(u8 far *)chain_payload)_fmemcpy(out,chain_payload,len);
#ifdef DOSFER_PROFILE
    dosferPlaneProfileTicks[0]+=timer_ticks()-t;
#endif
    return 1;
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
    int ok,v40=cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1;
    _fmemcpy(qr_temp,data,n);
    /* AUTO evaluates all eight masks and dominated runtime on a 386. Mask 0 is
       fully standard-compliant and cut measured DOSBox encoding time sharply. */
    if(v40) {
        /* PLANE frames are fixed-length V40-L records. Use the prepacked
           entry point so the Watcom build never enters the high-level
           segment-packing path that can stall before RS completion. */
        if(n!=2952){trace_stage("V40 LENGTH FAIL");return 0;}
        qr_prepack_data[0]=0x70;qr_prepack_data[1]=0x34;qr_prepack_data[2]=0x0B;qr_prepack_data[3]=0x88;
        _fmemcpy(qr_prepack_data+4,qr_temp,n);
        trace_stage("V40 PREPACK BEGIN");
        ok=qrcodegen_dosferEncodePrepackedV40L(qr_prepack_data,qr_temp);
        trace_stage("V40 PREPACK END");
    } else {
        qrcodegen_dosferSetCodewordsOnly(delta_only!=0);
        ok=qrcodegen_encodeBinary(qr_temp,n,qr_code,(enum qrcodegen_Ecc)cfg->ecc,
            cfg->qr_version,cfg->qr_version,(enum qrcodegen_Mask)mask,0);
    }
    if(ok&&v40&&!delta_only)
        ok=qrcodegen_dosferBuildMatrixV40L(qr_temp,qr_code,(enum qrcodegen_Mask)mask);
    if(ok)encoded_qr_mask=mask;return ok;
}
static int qr_encode(const u8 *data,u16 n,const Config *cfg,int delta_only) {
    return qr_encode_mask(data,n,cfg,delta_only,qr_mask);
}
static int make_prepacked_data(const Window *w,u16 i,const Config *cfg,u32 session) {
    const PendingFrame *f=&w->frames[i];u16 n;
    n=make_frame(raw_frame,FK_DATA,FF_WHITENED,session,w->id,f->global_index,
        i,w->count,f->stream_id,f->stream_offset,f->payload,f->payload_len);
    if(n!=2952)return 0;
    qr_prepack_data[0]=0x70;qr_prepack_data[1]=0x34;qr_prepack_data[2]=0x0B;qr_prepack_data[3]=0x88;
    _fmemcpy(qr_prepack_data+4,raw_frame,n);
    if(!qrcodegen_dosferEncodePrepackedV40L(qr_prepack_data,qr_temp))return 0;
    encoded_qr_mask=qr_mask;return n;
}
static int benchmark_xor3_symbol(const Config *cfg,const u8 *payload,u8 a,u8 b,u8 c,
        int *codeword_ok,int *raster_ok,int *color_plane_enable_ok) {
    u16 cw=qr_codeword_bytes(cfg),rawlen,j;u8 indices[3];int k;
    indices[0]=a;indices[1]=b;indices[2]=c;
    *codeword_ok=*raster_ok=*color_plane_enable_ok=0;
    for(k=0;k<3;++k) {
        rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,904UL+indices[k],indices[k],4,0,
            (u32)(4+indices[k])*cfg->frame_payload,payload,cfg->frame_payload);
        if(!k)_fmemcpy(qr_code+4,raw_frame,rawlen);
        else for(j=0;j<rawlen;++j)qr_code[4+j]^=raw_frame[j];
    }
    if(!qr_encode(qr_code+4,rawlen,cfg,0))return 0;
    *codeword_ok=1;
    for(j=0;j<cw;++j)if(qr_temp[j]!=(u8)(producer_disk[0][(u32)a*cw+j]^producer_disk[0][(u32)b*cw+j]^producer_disk[0][(u32)c*cw+j])){
        *codeword_ok=0;break;
    }
    *raster_ok=vga_verify_plane_xor3(qr_code,177,cfg->invert,
        (u8)((1U<<a)|(1U<<b)|(1U<<c)),color_plane_enable_ok);
    return 1;
}
/* Phase-1 PLANE_CODED proof: four ordinary, unwhitened wire frames plus a
 * canonical coefficient-0F parity frame.  The payload XOR vanishes from the
 * correction, leaving exactly the affine QR prefix/header delta handled by
 * qrcodegen_dosferDeriveXor4V40L(). */
static int benchmark_plane4_parity(const Config *cfg,int *codeword_ok,
        int *direct_ok,u16 *changed_codewords,int *raster_ok,int *restored_ok,
        int *color_plane_enable_ok) {
    static const u8 coeffs[4]={1,2,4,8};
    u8 headers[4][FRAME_HEADER_SIZE],header_delta[FRAME_HEADER_SIZE];
    u16 cw=qr_codeword_bytes(cfg),rawlen,j,bi;u32 br,bu,bp;
    int verified;
    *codeword_ok=*direct_ok=*raster_ok=*restored_ok=*color_plane_enable_ok=0;
    *changed_codewords=0;
    memset(chain_payload,0,cfg->frame_payload);
    qrcodegen_dosferSetCodewordsOnly(0);
    for(bi=0;bi<4;++bi) {
        for(j=0;j<cfg->frame_payload;++j)record_body[j]=(u8)(0x31U+bi*53U+j*17U);
        for(j=0;j<cfg->frame_payload;++j)chain_payload[j]^=record_body[j];
        rawlen=make_plane_frame(raw_frame,0x6A67C69DUL,3,700UL,0,4,4,
            coeffs[bi],record_body,cfg->frame_payload);
        if(rawlen!=(u16)(FRAME_HEADER_SIZE+cfg->frame_payload) ||
           !qr_encode(raw_frame,rawlen,cfg,0))goto done;
        if(!bi)_fmemcpy(producer_disk[1]+4096,qr_code,QR_BUFFER+1);
        _fmemcpy(producer_disk[0]+(u32)bi*cw,qr_temp,cw);
        _fmemcpy(headers[bi],raw_frame,FRAME_HEADER_SIZE);
        qrcodegen_dosferSetCodewordsOnly(1);
    }
    qrcodegen_dosferSetCodewordsOnly(0);
    rawlen=make_plane_frame(raw_frame,0x6A67C69DUL,3,700UL,0,4,4,0x0F,
        chain_payload,cfg->frame_payload);
    if(!rawlen||!qr_encode(raw_frame,rawlen,cfg,0))goto done;
    _fmemcpy(producer_disk[1],qr_temp,cw);
    _fmemcpy(producer_disk[1]+8192,qr_code,QR_BUFFER+1);
    for(j=0;j<FRAME_HEADER_SIZE;++j)
        header_delta[j]=(u8)(raw_frame[j]^headers[0][j]^headers[1][j]^headers[2][j]^headers[3][j]);
    if(!ensure_chain_cache())goto done;
    qrcodegen_dosferDeriveXor4V40L(producer_disk[0],producer_disk[0]+cw,
        producer_disk[0]+(u32)cw*2,producer_disk[0]+(u32)cw*3,
        header_delta,qr_temp);
    *codeword_ok=!_fmemcmp(qr_temp,producer_disk[1],cw);
    if(!*codeword_ok)goto done;
    /* Form a canonical raster for Q(delta) only as an oracle input.  The
     * actual parity path will synthesize this compact correction directly. */
    _fmemset(raw_frame,0,rawlen);_fmemcpy(raw_frame,header_delta,FRAME_HEADER_SIZE);
    if(!qr_encode(raw_frame,rawlen,cfg,0))goto done;
    _fmemcpy(producer_disk[1]+12288,qr_code,QR_BUFFER+1);
    if(!vga_benchmark_plane_batch4(producer_disk[1]+4096,producer_disk[0],cw,
        177,cfg->invert,&br,&bu,&bp,&verified)||!verified)goto done;
    _fmemset(raw_frame,0,rawlen);
    if(!qr_encode(raw_frame,rawlen,cfg,0))goto done;
    _fmemcpy(chain_left_codewords,qr_temp,cw);
    qrcodegen_dosferHeaderCorrectionV40L(header_delta,chain_right_codewords);
    *direct_ok=vga_verify_affine_correction(qr_code,chain_left_codewords,
        chain_right_codewords,producer_disk[1]+12288,cw,177,cfg->invert,changed_codewords);
    if(!*direct_ok)goto done;
    *raster_ok=vga_verify_plane_xor4_correction(producer_disk[1]+8192,producer_disk[1]+12288,177,cfg->invert,
        restored_ok,color_plane_enable_ok);
done:
    qrcodegen_dosferSetCodewordsOnly(0);
    return *codeword_ok;
}
static int benchmark_plane_startup_delta(const Config *cfg) {
    u16 n,j;u32 delta_hash,full_hash;
    /* Mirror the first group: fixed 2,904-byte payloads with short records
     * at the beginning and zero padding afterwards. */
    _fmemset(record_body,0,cfg->frame_payload);for(j=0;j<48;++j)record_body[j]=(u8)(0x31+j);
    n=make_plane_frame(raw_frame,0x6A67C69DUL,1,100,0,32,3,1,record_body,cfg->frame_payload);
    if(!n||!qr_encode(raw_frame,n,cfg,0)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),177,1,cfg->invert,"Plane delta oracle","C1 full",0,0))return 0;
    qr_delta_ready=vga_delta_ready();
    _fmemset(record_body,0,cfg->frame_payload);for(j=0;j<48;++j)record_body[j]=(u8)(0x91+j);
    n=make_plane_frame(raw_frame,0x6A67C69DUL,1,100,0,32,3,2,record_body,cfg->frame_payload);
    if(!n||!qr_encode(raw_frame,n,cfg,1)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),177,1,cfg->invert,"Plane delta oracle","C2 delta",1,1))return 0;
    delta_hash=vga_screen_hash();
    if(!qr_encode(raw_frame,n,cfg,0)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),177,1,cfg->invert,"Plane delta oracle","C2 full",1,0))return 0;
    full_hash=vga_screen_hash();qr_delta_ready=vga_delta_ready();return delta_hash==full_hash;
}
static int show_frame(const Window *w,u16 i,const Config *cfg,u32 session,int repeated,int waiting,u16 hold_ms,u8 display_mask) {
    char a[79],b[79];u16 n;u32 elapsed;int rescue=hold_ms!=cfg->hold_ms||display_mask!=qr_mask;
    int delta=can_delta(cfg,display_mask);
    const PendingFrame *f=&w->frames[i];
    const u8 far *payload=f->payload;
    if(f->spooled) {if(!window_read_payload(w,i,record_body))return 0;payload=record_body;}
    if(delta&&!repeated&&chain_cache_valid&&chain_cache_session==session&&
       chain_cache_window==w->id&&chain_cache_global==f->global_index&&
       chain_cache_index==i&&chain_cache_mask==display_mask) {
        n=chain_cache_rawlen;
        _fmemcpy(raw_frame,chain_cached_raw,FRAME_HEADER_SIZE);
        _fmemcpy(qr_temp,chain_right_codewords,qr_codeword_bytes(cfg));
        encoded_qr_mask=display_mask;
        chain_cache_valid=0;
    } else {
        if(delta&&!f->spooled&&!repeated&&display_mask==qr_mask&&cfg->qr_version==40&&cfg->ecc==0&&
           cfg->module_pixels==1&&f->payload_len==2904) {
            n=(u16)make_prepacked_data(w,i,cfg,session);if(!n)return 0;
        } else {
            n=make_frame(raw_frame,FK_DATA,(repeated?FF_REPEATED:0)|FF_WHITENED,session,w->id,f->global_index,
                i,w->count,f->stream_id,f->stream_offset,payload,f->payload_len);
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
        right_rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,session,w->id,right->global_index,
            i+1,w->count,right->stream_id,right->stream_offset,right->payload,right->payload_len);
        _fmemcpy(chain_cached_raw,raw_frame,FRAME_HEADER_SIZE);
        if(right_rawlen==2952) {
            qr_prepack_data[0]=0x70;qr_prepack_data[1]=0x34;qr_prepack_data[2]=0x0B;qr_prepack_data[3]=0x88;
            _fmemcpy(qr_prepack_data+4,raw_frame,right_rawlen);
            if(qrcodegen_dosferEncodePrepackedV40L(qr_prepack_data,qr_temp)) {
                encoded_qr_mask=qr_mask;
                _fmemcpy(chain_right_codewords,qr_temp,qr_codeword_bytes(cfg));
                chain_cache_valid=1;chain_cache_session=session;chain_cache_window=w->id;
                chain_cache_global=right->global_index;chain_cache_index=(u16)(i+1);
                chain_cache_rawlen=right_rawlen;chain_cache_mask=qr_mask;cached=1;
            }
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
static u16 make_plane_symbol(const Window *w,u16 first,u8 width,u8 coefficient,
        const Config *cfg,u32 session) {
    const PendingFrame *base=&w->frames[first];u16 j,k;
    _fmemset(record_body,0,cfg->frame_payload);
    if(coefficient==1||coefficient==2||coefficient==4||coefficient==8) {
        k=coefficient==1?0:coefficient==2?1:coefficient==4?2:3;
        if(k>=width||!window_read_payload(w,(u16)(first+k),record_body))return 0;
    } else {
        for(k=0;k<width;++k) {
            if(!window_read_payload(w,(u16)(first+k),(u8 far *)chain_payload))return 0;
            for(j=0;j<w->frames[first+k].payload_len;++j)record_body[j]^=chain_payload[j];
        }
    }
    return make_plane_frame(raw_frame,session,w->id,base->global_index,first,w->count,
        width,coefficient,record_body,cfg->frame_payload);
}
/* Canonical fallback retained for individual rescue/diagnostic symbols. The
 * complete live PLANE scheduler below never calls this after group playback
 * begins. */
static int show_plane_symbol(const Window *w,u16 first,u8 width,u8 coefficient,
        const Config *cfg,u32 session,u16 hold_ms) {
    const PendingFrame *base=&w->frames[first];char a[79],b[79];u16 rawlen;
    u32 elapsed;int delta=can_delta(cfg,qr_mask);
    rawlen=make_plane_symbol(w,first,width,coefficient,cfg,session);
    if(!rawlen||!qr_encode_mask(raw_frame,rawlen,cfg,delta,qr_mask))return 0;
    sprintf(a,"PLANE%u G%u C%X",width,(unsigned)(first/width)+1,coefficient);
    sprintf(b,"Hold %u  fixed payload %u",hold_ms,cfg->frame_payload);
    if(last_visible_tick){elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());if(elapsed<hold_ms)timer_wait_ms((u16)(hold_ms-elapsed));}
    if(!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),177,1,cfg->invert,a,b,
        (int)(base->global_index&1),delta))return 0;
    qr_delta_ready=vga_delta_ready();last_visible_tick=timer_ticks();return 1;
}
static void plane_queue_release(PlaneQueue *q) {
    u8 i;for(i=0;i<8;++i) {
        if(q->slots[i].correction_patch_offset)_ffree(q->slots[i].correction_patch_offset);
        if(q->slots[i].correction_patch_xor)_ffree(q->slots[i].correction_patch_xor);
    }
    for(i=0;i<4;++i)if(q->work.codewords[i])_ffree(q->work.codewords[i]);
    if(q->work.zero_codewords)_ffree(q->work.zero_codewords);
    if(q->work.correction_codewords)_ffree(q->work.correction_codewords);
    memset(q,0,sizeof(*q));
}
static int plane_queue_init(PlaneQueue *q,const Config *cfg,u16 page_step,u16 group_count) {
    u8 i;u16 cw=qr_codeword_bytes(cfg);memset(q,0,sizeof(*q));
    q->page_step=page_step;q->total_groups=group_count;q->preparing=0xFF;q->min_ready=8;
    for(i=0;i<8;++i) {
        q->slots[i].slot=i;q->slots[i].slot_start=(u16)((u16)i*page_step);
        if(cfg->plane_width==4&&
           (!(q->slots[i].correction_patch_offset=(u16 far *)_fmalloc((u32)VGA_PLANE_MAX_CORRECTION_PATCHES*sizeof(u16)))||
            !(q->slots[i].correction_patch_xor=(u8 far *)_fmalloc(VGA_PLANE_MAX_CORRECTION_PATCHES)))) {
            plane_queue_release(q);return 0;
        }
    }
    for(i=0;i<4;++i)if(!(q->work.codewords[i]=(u8 far *)_fmalloc(cw))){plane_queue_release(q);return 0;}
    if(!(q->work.zero_codewords=(u8 far *)_fmalloc(cw))||
       !(q->work.correction_codewords=(u8 far *)_fmalloc(cw))){plane_queue_release(q);return 0;}
    return 1;
}
static int plane_queue_claim(PlaneQueue *q,const Window *w,const Config *cfg) {
    PlaneGroup *g;
    if(q->next_first+(u16)cfg->plane_width>w->count||q->slots[q->tail].state!=PLANE_SLOT_FREE)return 0;
    g=&q->slots[q->tail];g->first_window_index=q->next_first;g->window_id=w->id;
    g->group_global=w->frames[q->next_first].global_index;g->width=cfg->plane_width;
    g->valid_mask=0;g->state=PLANE_SLOT_PREPARING;g->prepare_stage=0;g->parity_correction_applied=0;
    g->prepare_started=timer_ticks();q->preparing=q->tail;q->tail=(u8)((q->tail+1)&7);
    q->next_first=(u16)(q->next_first+cfg->plane_width);return 1;
}
/* One bounded producer chunk.  QR encoding itself is presently the longest
 * indivisible chunk; the eight-slot queue absorbs it and records its cost. */
static int plane_producer_step(PlaneQueue *q,const Window *w,const Config *cfg,u32 session) {
    static const u8 coeffs[4]={1,2,4,8};PlaneGroup *g;PlaneWork *pw=&q->work;
    u8 header_delta[FRAME_HEADER_SIZE],parity;u16 rawlen,cw=qr_codeword_bytes(cfg),j;u32 t=timer_ticks();
    if(q->preparing==0xFF) {if(!plane_queue_claim(q,w,cfg))return 0;}
    g=&q->slots[q->preparing];
    if(g->prepare_stage<g->width) {
        u8 p=g->prepare_stage;
#ifdef DOSFER_PROFILE
        u32 payload_t=timer_ticks();
#endif
        rawlen=make_plane_symbol(w,g->first_window_index,g->width,coeffs[p],cfg,session);
#ifdef DOSFER_PROFILE
        dosferPlaneProfileTicks[1]+=timer_ticks()-payload_t;
#endif
        if(!rawlen||!qr_encode_mask(raw_frame,rawlen,cfg,p!=0,qr_mask))return -1;
        _fmemcpy(pw->codewords[p],qr_temp,cw);_fmemcpy(pw->headers[p],raw_frame,FRAME_HEADER_SIZE);
        if(!vga_plane_store_qr(qr_code,qr_temp,cw,177,cfg->invert,p,g->slot,p!=0))return -1;
        g->valid_mask|=(u8)(1U<<p);++g->prepare_stage;
    } else {
        parity=g->width==3?7:15;
        rawlen=make_plane_symbol(w,g->first_window_index,g->width,parity,cfg,session);if(!rawlen)return -1;
        if(g->width==4) {
#ifdef DOSFER_PROFILE
            u32 correction_t=timer_ticks();
#endif
            for(j=0;j<FRAME_HEADER_SIZE;++j)header_delta[j]=(u8)(raw_frame[j]^pw->headers[0][j]^pw->headers[1][j]^pw->headers[2][j]^pw->headers[3][j]);
            if(!qrcodegen_dosferHeaderCorrectionV40L(header_delta,pw->correction_codewords))return -1;
            _fmemset(raw_frame,0,rawlen);if(!qr_encode_mask(raw_frame,rawlen,cfg,0,qr_mask))return -1;
            _fmemcpy(pw->zero_codewords,qr_temp,cw);
            if(!vga_plane_prepare_correction(qr_code,pw->zero_codewords,pw->correction_codewords,
                                              cw,177,cfg->invert,g->correction_patch_offset,
                                              g->correction_patch_xor,&g->correction_patch_count))return -1;
#ifdef DOSFER_PROFILE
            dosferPlaneProfileTicks[6]+=timer_ticks()-correction_t;
#endif
        }
        g->state=PLANE_SLOT_READY;g->ready_tick=timer_ticks();q->preparing=0xFF;++q->ready_count;
#ifdef DOSFER_PROFILE
        dosferPlaneProfileTicks[2]+=g->ready_tick-g->prepare_started;
#endif
        if(q->ready_count<q->min_ready)q->min_ready=q->ready_count;
    }
    t=timer_ticks()-t;if(t>q->max_chunk_ticks)q->max_chunk_ticks=t;
    return 1;
}
static int plane_produce_until_ready(PlaneQueue *q,const Window *w,const Config *cfg,u32 session,u16 target) {
    int rc;while(q->ready_count<target&&(q->preparing!=0xFF||q->next_first+(u16)cfg->plane_width<=w->count)) {
        rc=plane_producer_step(q,w,cfg,session);if(rc<=0)return rc;
    }return 1;
}
static int plane_hold_work(PlaneQueue *q,const Window *w,const Config *cfg,u32 session,u32 deadline) {
    int rc;u32 t;while((long)(timer_ticks()-deadline)<0) {
        t=timer_ticks();rc=plane_producer_step(q,w,cfg,session);if(rc<0)return 0;
#ifdef DOSFER_PROFILE
        if(rc)dosferPlaneProfileTicks[4]+=timer_ticks()-t;
        else dosferPlaneProfileTicks[5]+=timer_ticks()-t;
#endif
        if(!rc) { /* no free slot or all work complete: leave the CPU idle */ }
    }return 1;
}
static int plane_show_group_symbol(PlaneGroup *g,u8 symbol) {
    u8 mask=g->width==3?(symbol==3?7:(u8)(1U<<symbol)):(symbol==4?15:(u8)(1U<<symbol));u32 prior=last_visible_tick,now;
    if(mask==15) {
        if(!vga_plane_apply_correction(g->slot,3,g->correction_patch_offset,
           g->correction_patch_xor,g->correction_patch_count,&g->correction_restore_hash))return 0;
        g->parity_correction_applied=1;
    }
    if(!vga_plane_show_mask(g->slot_start,mask))return 0;
    now=timer_ticks();if(prior){plane_queue.visible_ticks+=now-prior;++plane_queue.visible_transitions;}
    last_visible_tick=now;return 1;
}
static int plane_queue_play(const Window *w,const Config *cfg,u32 session,int pause_after_first_c1) {
    PlaneQueue *q=&plane_queue;PlaneGroup *g;u8 symbol=0,count;u32 deadline;int key,rc;
#ifdef DOSFER_PROFILE
    u32 playback_t=timer_ticks();
#endif
    /* Do not leave the display blank while a second group is encoded.  The
       first group is complete and its C1 is the real transport symbol. */
    if((rc=plane_produce_until_ready(q,w,cfg,session,1))<=0||!q->ready_count)return rc;
    g=&q->slots[q->head];g->state=PLANE_SLOT_PLAYING;--q->ready_count;
    if(q->ready_count<q->min_ready)q->min_ready=q->ready_count;count=(u8)(g->width+1);
    if(!plane_show_group_symbol(g,0))return 0;
    if(pause_after_first_c1){
        /* C1 remains resident during camera focus, so use that time to build
           the remaining queue instead of waiting for input with the producer
           idle. */
        flush_keys();
        for(;;) {
            if(_bios_keybrd(_KEYBRD_READY)) {
                key=decision_key();
                if(key==13||key==27)break;
            }
            rc=plane_producer_step(q,w,cfg,session);
            if(rc<0)return 0;
            if(!rc)timer_wait_ms(1);
        }
        if(key==27)return -1;
    }
    for(;;) {
        deadline=last_visible_tick+(u32)cfg->hold_ms*74UL+((u32)cfg->hold_ms*574UL+999UL)/1000UL;
        if(!plane_hold_work(q,w,cfg,session,deadline))return 0;
        if(++symbol<count) {if(!plane_show_group_symbol(g,symbol))return 0;continue;}
        /* Keep parity visible if the producer genuinely falls behind. */
        if(!q->ready_count&&q->next_first+(u16)cfg->plane_width<=w->count) {
            u32 gap_start=timer_ticks();++q->starvation_count;
            while(!q->ready_count) {rc=plane_producer_step(q,w,cfg,session);if(rc<0)return 0;if(!rc)break;}
            q->intergroup_ticks+=timer_ticks()-gap_start;
        }
        if(q->ready_count) {
            PlaneGroup *old=g;
            /* Switch away from CF before modifying its resident plane.  The
             * previous order XORed plane 3 back while mask 0F was visible,
             * producing the observed corrupted-background blink. */
            g=&q->slots[q->head=(u8)((q->head+1)&7)];g->state=PLANE_SLOT_PLAYING;--q->ready_count;
            if(q->ready_count<q->min_ready)q->min_ready=q->ready_count;symbol=0;count=(u8)(g->width+1);
            if(!plane_show_group_symbol(g,0))return 0;
            if(old->parity_correction_applied) {
                if(!vga_plane_restore_correction(old->slot,3,old->correction_patch_offset,
                   old->correction_patch_xor,old->correction_patch_count,old->correction_restore_hash))return 0;
                old->parity_correction_applied=0;
            }
            old->state=PLANE_SLOT_FREE;
            continue;
        }
        /* Last parity has no successor slot. It is restored before leaving
         * the resident backend, where no PLANE symbol remains selected. */
        if(g->parity_correction_applied) {
            if(!vga_plane_restore_correction(g->slot,3,g->correction_patch_offset,
               g->correction_patch_xor,g->correction_patch_count,g->correction_restore_hash))return 0;
            g->parity_correction_applied=0;
        }
        g->state=PLANE_SLOT_FREE;q->head=(u8)((q->head+1)&7);break;
    }
#ifdef DOSFER_PROFILE
    dosferPlaneProfileTicks[3]+=timer_ticks()-playback_t;
#endif
    return 1;
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
static int transmit(const Window *w,const Config *cfg,u32 session,const u8 *selected,u16 chosen,u16 rescue_round,int focus_first_plane_c1) {
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
        /* A selective rescue is intentionally ordinary DATA: arbitrary
         * individual records do not form a resident PLANE basis group. */
        if(!selected&&cfg->plane_width) {
            u16 plane_step=0,full=(u16)(w->count/cfg->plane_width);int plane_result;
            if(!vga_plane_begin(&plane_step)||!plane_queue_init(&plane_queue,cfg,plane_step,full)) {
                plane_queue_release(&plane_queue);vga_plane_end();return 0;
            }
            /* The queue owns screen_320's delta state; generic rendering is
             * forbidden until all resident groups have been released. */
            qr_delta_ready=0;
            plane_result=plane_queue_play(w,cfg,session,focus_first_plane_c1&&r==0);
            plane_last_min_ready=plane_queue.min_ready;plane_last_starvation=plane_queue.starvation_count;
            plane_last_max_chunk=plane_queue.max_chunk_ticks;plane_last_intergroup=plane_queue.intergroup_ticks;
            plane_last_visible=plane_queue.visible_ticks;plane_last_visible_count=plane_queue.visible_transitions;
            vga_plane_end();plane_queue_release(&plane_queue);
            if(plane_result<0)return -1;
            if(!plane_result)return 0;
            for(i=(u16)(full*cfg->plane_width);i<w->count;++i)
                if(!show_frame(w,i,cfg,session,r>0,0,hold_ms,display_mask))return 0;
            continue;
        }
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
/* A PLANE logical window is metadata, not an array of payloads.  This pass
 * determines each advertised windowCount before records are generated. */
static int stream_count_records(FILE *mf,const Config *cfg,u32 *out) {
    ManifestEntry e;u32 total=2,cap,add;
    if(!mf||!out)return 0;cap=(u32)cfg->frame_payload-RECORD_HEADER_SIZE-4;if(!cap)return 0;
    rewind(mf);while(fread(&e,1,sizeof(e),mf)==sizeof(e)) {
        if(e.kind==2)add=1;else if(e.kind==1)add=2+(e.size+(cap-1))/cap;
        else {rewind(mf);return 0;}
        if(total>0xFFFFFFFFUL-add){rewind(mf);return 0;}total+=add;
    }
    if(ferror(mf)){rewind(mf);return 0;}rewind(mf);*out=total;return 1;
}
static void stream_open(PlaneStream *s,const Config *cfg,u32 session,u32 total) {
    memset(s,0,sizeof(*s));s->record.payload=stream_record_payload;
    s->record.payload_capacity=MAX_FRAME_PAYLOAD;s->session=session;s->total_records=total;
    s->window_count=(u16)(total>cfg->window_frames?cfg->window_frames:total);
}
static int stream_take(PlaneStream *s,const Config *cfg) {
    int rc;if(!s->window_count||s->window_index>=s->window_count)return 0;
    rc=producer_next(&s->record,cfg,s->session);
    if(rc<=0)return rc;
    return s->record.global_index<s->total_records?1:-1;
}
static void stream_commit(PlaneStream *s,const Config *cfg) {
    u32 remaining;if(++s->window_index<s->window_count)return;
    ++s->window_id;s->window_index=0;remaining=s->total_records-producer.global_index;
    s->window_count=(u16)(remaining>cfg->window_frames?cfg->window_frames:remaining);
}
static void stream_close(PlaneStream *s) {
    memset(s,0,sizeof(*s));
}
/* Restore info for a slot whose symbol has already been switched away from.
 * Deferring the restore until the successor slot is on screen means the XOR
 * that removes the correction never touches a currently displayed plane. */
typedef struct {
    int pending;
    u8 slot;
    u16 far *patch_offset;
    u8 far *patch_xor;
    u16 patch_count;
    u32 restore_hash;
} PendingCorrection;
static int stream_plane_work_open(PlaneGroup *g,PlaneWork *pw,const Config *cfg) {
    u8 i;u16 cw=qr_codeword_bytes(cfg);memset(g,0,sizeof(*g));memset(pw,0,sizeof(*pw));
    if(cw>QR40_CODEWORDS)return 0;
    g->slot=0;g->slot_start=0;
    for(i=0;i<4;++i)pw->codewords[i]=stream_plane_codewords[i];
    pw->zero_codewords=stream_plane_codewords[4];
    pw->correction_codewords=stream_plane_codewords[5];
    return 1;
}
static void stream_plane_work_close(PlaneGroup *g,PlaneWork *pw) {
    memset(g,0,sizeof(*g));memset(pw,0,sizeof(*pw));
}
static int stream_plane_prepare(PlaneStream *s,PlaneGroup *g,PlaneWork *pw,
                                const Config *cfg,u8 slot,u16 page_step) {
    static const u8 coeff[4]={1,2,4,8};u8 p,parity,header_delta[FRAME_HEADER_SIZE];
    u16 j,rawlen,cw=qr_codeword_bytes(cfg),window_count=s->window_count;
    if(window_count-s->window_index<cfg->plane_width)return 0;
    memset(chain_payload,0,cfg->frame_payload);g->width=cfg->plane_width;g->first_window_index=s->window_index;
    g->window_id=s->window_id;g->valid_mask=0;g->parity_correction_applied=0;g->correction_patch_count=0;
    /* Build into the slot that is currently off screen.  The previously
     * shown group keeps displaying from its own slot while this one is
     * assembled, so there is nothing to blank during preparation. */
    g->slot=slot;g->slot_start=(u16)slot*page_step;
    if(cfg->plane_width==4) {
        g->correction_patch_offset=stream_correction_offsets[slot];
        g->correction_patch_xor=stream_correction_xor[slot];
    }
    for(p=0;p<g->width;++p) {
        int rc;trace_stage(p==0?"P0 take":p==1?"P1 take":p==2?"P2 take":"P3 take");
        rc=stream_take(s,cfg);if(rc<=0)return rc;if(!p)g->group_global=s->record.global_index;
        _fmemset(record_body,0,cfg->frame_payload);_fmemcpy(record_body,s->record.payload,s->record.payload_len);
        for(j=0;j<s->record.payload_len;++j)chain_payload[j]^=s->record.payload[j];
        rawlen=make_plane_frame(raw_frame,s->session,g->window_id,g->group_global,g->first_window_index,
            window_count,g->width,coeff[p],record_body,cfg->frame_payload);
        trace_stage(p==0?"P0 frame":p==1?"P1 frame":p==2?"P2 frame":"P3 frame");
        trace_stage(p==0?"P0 QR BEGIN":p==1?"P1 QR BEGIN":p==2?"P2 QR BEGIN":"P3 QR BEGIN");
        if(!rawlen||!qr_encode_mask(raw_frame,rawlen,cfg,p!=0,qr_mask))return -1;
        trace_stage(p==0?"P0 QR":p==1?"P1 QR":p==2?"P2 QR":"P3 QR");
        _fmemcpy(pw->codewords[p],qr_temp,cw);_fmemcpy(pw->headers[p],raw_frame,FRAME_HEADER_SIZE);
        trace_stage(p==0?"P0 STORE BEGIN":p==1?"P1 STORE BEGIN":p==2?"P2 STORE BEGIN":"P3 STORE BEGIN");
        if(!vga_plane_store_qr(qr_code,pw->codewords[p],cw,177,cfg->invert,p,g->slot,p!=0))return -1;
        trace_stage(p==0?"P0 stored":p==1?"P1 stored":p==2?"P2 stored":"P3 stored");
        g->valid_mask|=(u8)(1U<<p);stream_commit(s,cfg);
    }
    parity=g->width==3?7:15;
    rawlen=make_plane_frame(raw_frame,s->session,g->window_id,g->group_global,g->first_window_index,
        window_count,g->width,parity,(const u8 far *)chain_payload,cfg->frame_payload);
    if(!rawlen)return -1;
    trace_stage("Parity frame");if(g->width==4) {
        for(j=0;j<FRAME_HEADER_SIZE;++j)header_delta[j]=(u8)(raw_frame[j]^pw->headers[0][j]^pw->headers[1][j]^pw->headers[2][j]^pw->headers[3][j]);
        if(!qrcodegen_dosferHeaderCorrectionV40L(header_delta,pw->correction_codewords))return -1;
        _fmemset(raw_frame,0,rawlen);if(!qr_encode_mask(raw_frame,rawlen,cfg,0,qr_mask))return -1;
        _fmemcpy(pw->zero_codewords,qr_temp,cw);
        trace_stage("Parity correction");
        if(!vga_plane_prepare_correction(qr_code,pw->zero_codewords,pw->correction_codewords,cw,177,cfg->invert,
            g->correction_patch_offset,g->correction_patch_xor,&g->correction_patch_count))return -1;
    }
    g->state=PLANE_SLOT_READY;return 1;
}
static int stream_plane_play(PlaneGroup *g,const Config *cfg,int first_already_visible,
                             PendingCorrection *pending) {
    u8 symbol=first_already_visible?1:0,count=(u8)(g->width+1),mask;
    while(symbol<count) {
        if(symbol){u32 elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());if(elapsed<cfg->hold_ms)timer_wait_ms((u16)(cfg->hold_ms-elapsed));}
        mask=g->width==3?(symbol==3?7:(u8)(1U<<symbol)):(symbol==4?15:(u8)(1U<<symbol));
        if(mask==15) {if(!vga_plane_apply_correction(g->slot,3,g->correction_patch_offset,g->correction_patch_xor,g->correction_patch_count,&g->correction_restore_hash))return 0;g->parity_correction_applied=1;}
        if(!vga_plane_show_mask(g->slot_start,mask))return 0;last_visible_tick=timer_ticks();
        if(!symbol&&pending->pending) {
            /* The previous group's slot just left the screen: XOR its
             * correction back out now, with nothing selected there to see
             * it. This never blanks the display, unlike the earlier
             * approach that disabled all planes while a full new group was
             * being encoded and stored. */
            if(!vga_plane_restore_correction(pending->slot,3,pending->patch_offset,
               pending->patch_xor,pending->patch_count,pending->restore_hash))return 0;
            pending->pending=0;
        }
        ++symbol;
    }
    if(g->parity_correction_applied) {
        pending->pending=1;pending->slot=g->slot;
        pending->patch_offset=g->correction_patch_offset;pending->patch_xor=g->correction_patch_xor;
        pending->patch_count=g->correction_patch_count;pending->restore_hash=g->correction_restore_hash;
        g->parity_correction_applied=0;
    }
    g->state=PLANE_SLOT_FREE;return 1;
}
static int stream_show_tail(PlaneStream *s,const Config *cfg) {
    u16 n=make_frame(raw_frame,FK_DATA,FF_WHITENED,s->session,s->window_id,s->record.global_index,
        s->window_index,s->window_count,s->record.stream_id,s->record.stream_offset,s->record.payload,s->record.payload_len);
    if(!qr_encode_mask(raw_frame,n,cfg,0,qr_mask)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),177,1,cfg->invert,
        "TRANSFER V40L TAIL","",(int)(s->record.global_index&1),0))return 0;
    last_visible_tick=timer_ticks();stream_commit(s,cfg);return 1;
}
static int run_plane_stream_transfer(FILE *mf,Config *cfg) {
    PlaneStream s;PlaneGroup group;PlaneWork work;PendingCorrection pending;
    u32 session=(timer_ticks()^(u32)time(NULL)^selected_bytes)|1UL;
    u16 page_step;int rc,first=1,plane_active=0;u8 next_slot=0;
    remove("DFTRACE.TXT");debug_trace=fopen("DFTRACE.TXT","wt");trace_stage(DOSFER_BUILD_ID);trace_stage("S0 start");
    if(!stream_count_records(mf,cfg,&s.total_records)){puts("Cannot count transfer records.");return 0;}
    trace_stage("S1 counted");producer_open(mf);stream_open(&s,cfg,session,s.total_records);vga_use_320(1);
    /* Print the focus prompt while still in text mode. Anything written to
       the console after vga_enter() switches to mode 0Dh lands straight in
       the resident QR's video memory (BIOS teletype does not know about our
       manual CRTC/plane bookkeeping), corrupting the very symbol it is
       supposed to be a caption for. */
    puts("C1 ready: focus camera, then press Enter (Esc cancels).");
    if(!vga_enter()){puts("Not enough memory for VGA buffer.");return 0;}
    trace_stage("S2 VGA mode");
    if(!vga_plane_begin(&page_step)||!stream_plane_work_open(&group,&work,cfg)){vga_plane_end();stream_close(&s);return 0;}
    trace_stage("S3 work ready");
    plane_active=1;qr_delta_ready=0;last_visible_tick=0;next_slot=0;memset(&pending,0,sizeof(pending));
    while(s.window_count) {
        if(s.window_count-s.window_index>=cfg->plane_width) {
            /* Build into whichever slot is currently off screen (0/1 ping-
               pong) so the previous group's symbol stays resident and
               visible the whole time this group is being assembled. */
            rc=stream_plane_prepare(&s,&group,&work,cfg,next_slot,page_step);if(rc<=0)goto done;
            next_slot=(u8)(next_slot^1);
            if(first) {
                int key;
                /* Drop stale input before presenting the completed group.
                   The producer never owns this interactive wait. */
                flush_keys();trace_stage("FIRST GROUP READY");
                trace_stage("FOCUS SHOW BEGIN");
                if(!vga_plane_show_mask(group.slot_start,0x01)){rc=0;goto done;}
                last_visible_tick=timer_ticks();trace_stage("FOCUS SHOW END");
                trace_stage("FOCUS WAIT BEGIN");
                do{key=decision_key();}while(key!=13&&key!=27);
                trace_stage(key==13?"FOCUS WAIT END ENTER":"FOCUS WAIT END ESC");
                if(key==27){rc=-2;goto done;}
                rc=stream_plane_play(&group,cfg,1,&pending);if(rc<=0)goto done;
                first=0;continue;
            }
            rc=stream_plane_play(&group,cfg,0,&pending);if(rc<=0)goto done;continue;
        }
        if(plane_active){
            /* Leaving the resident backend entirely: apply any outstanding
               restore first, since vga_plane_end() repurposes this VRAM. */
            if(pending.pending) {
                (void)vga_plane_restore_correction(pending.slot,3,pending.patch_offset,
                    pending.patch_xor,pending.patch_count,pending.restore_hash);
                pending.pending=0;
            }
            vga_plane_end();plane_active=0;qr_delta_ready=0;
        }
        rc=stream_take(&s,cfg);if(rc<=0)goto done;if(!stream_show_tail(&s,cfg)){rc=0;goto done;}
        if(s.window_count&&s.window_count-s.window_index>=cfg->plane_width) {
            if(!vga_plane_begin(&page_step)){rc=0;goto done;}plane_active=1;qr_delta_ready=0;next_slot=0;
        }
    }
    if(producer_next(&s.record,cfg,session)!=0){puts("Manifest count mismatch.");rc=0;goto done;}
    completed_session=session;completed_frames=producer.global_index;completed_bytes=producer.total_bytes;rc=1;
done:
    /* The final group's correction (if any) has no successor slot to switch
       to first; the transfer is over, so restoring it here is harmless. */
    if(pending.pending) {
        (void)vga_plane_restore_correction(pending.slot,3,pending.patch_offset,
            pending.patch_xor,pending.patch_count,pending.restore_hash);
        pending.pending=0;
    }
    if(plane_active)vga_plane_end();stream_plane_work_close(&group,&work);stream_close(&s);vga_leave();
    /* Negative values are cancellation/internal-failure sentinels, never a
       completed transfer.  main() treats any nonzero result as success. */
    return rc>0?1:0;
}
static int run_transfer(FILE *mf,Config *cfg) {
    u32 session=(timer_ticks()^(u32)time(NULL)^selected_bytes)|1UL,window_id=0;
    int key,have_selection=0,focus_plane_c1=0;u16 chosen=0,rescue_round=0;u8 selected[MAX_WINDOW];char line[80];
    if(cfg->plane_width)return run_plane_stream_transfer(mf,cfg);
    vga_use_320(1);producer_open(mf);
    if(!fill_window_spooled(&current_window,cfg,session,window_id))return 0;
    if(cfg->chain_width==2&&cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1)ensure_chain_cache();
    if(!vga_enter()){puts("Not enough memory for VGA buffer");return 0;}qr_delta_ready=0;
    focus_plane_c1=cfg->plane_width&&current_window.count>=cfg->plane_width;
    if(!focus_plane_c1) {
        if(!show_frame(&current_window,0,cfg,session,0,1,cfg->hold_ms,qr_mask)){
            vga_leave();puts("Could not build the first QR; reduce /WINDOW or /PAYLOAD.");return 0;}
        flush_keys();do{key=decision_key();}while(key!=13&&key!=27);
        if(key==27){vga_leave();return 0;}
    }
    for(;;) {
        key=transmit(&current_window,cfg,session,0,0,0,focus_plane_c1);
        focus_plane_c1=0;
        if(key<0){vga_leave();return 0;}
        if(!key){vga_leave();puts("QR settings cannot fit the frame; reduce payload or increase version.");return 0;}
wait_ack:
        /* A window is uninterrupted; controls are accepted only after this
           fresh prompt is visible. */
        flush_keys();
        show_eow(&current_window,cfg,session,have_selection);
        key=decision_key();
        if(key=='r'||key=='R')goto replay;
        if(key=='b'||key=='B'){if(have_previous){transmit(&previous_window,cfg,session,0,0,0,0);goto wait_ack;}goto wait_ack;}
        if(key=='m'||key=='M'){
            vga_leave();printf("Missing frames in window %lu (example 1,3-5; blank = all): ",current_window.id+1);
            if(!fgets(line,sizeof(line),stdin))line[0]=0;
            if(blank_line(line)){have_selection=0;chosen=0;rescue_round=0;memset(selected,0,sizeof(selected));}
            else {
                chosen=parse_ranges(line,selected,current_window.count);
                if(!chosen){puts("No valid frame numbers entered. Press a key.");getch();if(!vga_enter())return 0;qr_delta_ready=0;goto wait_ack;}
                have_selection=1;rescue_round=1;
            }
            if(!vga_enter())return 0;qr_delta_ready=0;transmit(&current_window,cfg,session,have_selection?selected:0,chosen,rescue_round,0);goto wait_ack;
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
        if(!fill_window_spooled(&current_window,cfg,session,window_id)){vga_leave();
            completed_session=session;completed_frames=producer.global_index;completed_bytes=producer.total_bytes;return 1;}
        continue;
replay:
        if(have_selection)rescue_round++;
        transmit(&current_window,cfg,session,have_selection?selected:0,chosen,rescue_round,0);goto wait_ack;
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
    FILE *f;u8 far *b=producer_disk[0];u16 n;u32 bytes=0,crc=0,t0,t1,protocol_ms,encode_ms,build_ms,copy_ms,text_ms,frame_ms,useful;int i,encoded=0;u16 rawlen;
    {u8 record_test[100];u16 exact,refused;memset(record_test,0xCC,sizeof(record_test));
     exact=make_record(record_test+2,96,RT_SESSION,1,0,record_body,72);
     refused=make_record(record_test+2,96,RT_FILE_BEGIN,2,1,record_body,73);
     printf("Checked record builder: %s\n",exact==96&&!refused&&record_test[0]==0xCC&&record_test[1]==0xCC&&record_test[98]==0xCC&&record_test[99]==0xCC?"PASS":"FAIL");}
    t0=timer_ticks();timer_wait_ms(100);t1=timer_ticks();
    printf("PIT pacing self-test: requested 100 ms, measured %lu ms\n",
        timer_elapsed_ms(t0,t1));
    vga_use_320(cfg->qr_version==40&&cfg->module_pixels==1);f=fopen(path,"rb");if(!f){printf("Cannot open %s\n",path);return;}
    t0=timer_ticks();while((n=(u16)fread(disk_io,1,sizeof(disk_io),f))!=0){crc=crc32_update(crc,disk_io,n);bytes+=n;}t1=timer_ticks();fclose(f);
    printf("Disk+CRC: %lu bytes in %lu ms = %lu B/s, CRC %08lX\n",bytes,timer_elapsed_ms(t0,t1),timer_elapsed_ms(t0,t1)?bytes*1000UL/timer_elapsed_ms(t0,t1):0,crc);
    _fmemset(b,0,16);make_frame(raw_frame,FK_DATA,FF_WHITENED,0x6A67C69DUL,0,4,4,32,1,4510,b,16);
    printf("Payload whitening self-test: %s\n",_fmemcmp(raw_frame+48,whitening_test,16)?"FAIL":"PASS");
    _fmemset(b,0xA5,cfg->frame_payload);
    t0=timer_ticks();for(i=0;i<25;i++)rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,(u32)i,0,1,0,0,b,cfg->frame_payload);t1=timer_ticks();
    protocol_ms=timer_elapsed_ms(t0,t1)/25UL;
    printf("Config: QR v%u-%c, frame payload %u, raw QR bytes %u, scale %u\n",cfg->qr_version,"LMQH"[cfg->ecc],cfg->frame_payload,rawlen,cfg->module_pixels);
    printf("Protocol framing+CRC: 25 in %lu ms (%lu ms/frame)\n",timer_elapsed_ms(t0,t1),protocol_ms);
    if(cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1){
        u16 cw=qr_codeword_bytes(cfg),matrix_bytes=QR_BUFFER+1,j,diff_index=0;
        int code_ok=0,matrix_ok=0;u8 diff_fast=0,diff_canonical=0;
        qrcodegen_dosferSetCodewordsOnly(1);qrcodegen_dosferSetAlignedFast(1);_fmemcpy(qr_temp,raw_frame,rawlen);
        if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0)){
            _fmemcpy(producer_disk[1],qr_temp,cw);qrcodegen_dosferSetAlignedFast(0);_fmemcpy(qr_temp,raw_frame,rawlen);
            if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0)){
                code_ok=!_fmemcmp(producer_disk[1],qr_temp,cw);
                if(!code_ok)for(j=0;j<cw;++j)if(producer_disk[1][j]!=qr_temp[j]){
                    diff_index=j;diff_fast=producer_disk[1][j];diff_canonical=qr_temp[j];break;}
            }}
        qrcodegen_dosferSetCodewordsOnly(0);qrcodegen_dosferSetAlignedFast(1);_fmemcpy(qr_temp,raw_frame,rawlen);
        if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0)){
            _fmemcpy(producer_disk[1],qr_code,matrix_bytes);qrcodegen_dosferSetAlignedFast(0);_fmemcpy(qr_temp,raw_frame,rawlen);
            if(qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0))matrix_ok=!_fmemcmp(producer_disk[1],qr_code,matrix_bytes);}
        qrcodegen_dosferSetAlignedFast(1);qrcodegen_dosferSetCodewordsOnly(0);
        printf("Aligned V40 oracle: codewords %s, matrix %s\n",code_ok?"MATCH":"FAIL",matrix_ok?"MATCH":"FAIL");
        if(!code_ok)printf("  first codeword difference %u: fast %02X canonical %02X\n",
            diff_index,diff_fast,diff_canonical);
        if(!code_ok||!matrix_ok)return;
        {u8 far *raw_a=producer_disk[1],*raw_x=producer_disk[0]+4096;
         u8 *raw_b=raw_frame;
         u8 far *enc_a=producer_disk[0]+8192,*enc_b=producer_disk[1]+4096;u8 header_xor[48];u16 j;
         int payload_ok=1,derive_ok=0;u32 lengths=((u32)cfg->frame_payload<<16)|cfg->frame_payload;
         _fmemset(record_body,0x3C,cfg->frame_payload);for(j=0;j<cfg->frame_payload;++j)chain_payload[j]=(u8)(b[j]^record_body[j]);
         make_frame(raw_a,FK_DATA,FF_WHITENED,0x6A67C69DUL,2,100,0,2,0,0,b,cfg->frame_payload);
         make_frame(raw_b,FK_DATA,FF_WHITENED,0x6A67C69DUL,2,101,1,2,0,0,record_body,cfg->frame_payload);
         make_frame(raw_x,FK_CHAIN_XOR,FF_PAIR_WHITENED,0x6A67C69DUL,2,100,0,2,lengths,0,chain_payload,cfg->frame_payload);
         for(j=48;j<rawlen;++j)if(raw_x[j]!=(u8)(raw_a[j]^raw_b[j])){payload_ok=0;break;}
         qrcodegen_dosferSetCodewordsOnly(1);_fmemcpy(qr_temp,raw_a,rawlen);qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0);_fmemcpy(enc_a,qr_temp,cw);
         _fmemcpy(qr_temp,raw_b,rawlen);qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0);_fmemcpy(enc_b,qr_temp,cw);
         _fmemcpy(qr_temp,raw_x,rawlen);qrcodegen_encodeBinaryAligned(qr_temp,rawlen,qr_code,qrcodegen_Ecc_LOW,40,40,(enum qrcodegen_Mask)qr_mask,0);
         for(j=0;j<48;++j)header_xor[j]=(u8)(raw_a[j]^raw_b[j]^raw_x[j]);
         qrcodegen_dosferDeriveXorV40L(enc_a,enc_b,header_xor,qr_code);derive_ok=!_fmemcmp(qr_temp,qr_code,cw);
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
        if(ok){u32 du,dc,dt,ps,pb,pv;u16 step;int planes_ok;vga_benchmark_delta(qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,24,&du,&dc,&dt);
            printf("Delta VGA: update %lu, retrace+partial copy %lu, status %lu ms/frame\n",du/24UL,dc/24UL,dt/24UL);
            vga_benchmark_planes(&ps,&pb,&pv,&step,&planes_ok);
            printf("Mode 0Dh frame store: %s, 32 x 8K in %lu ms; CRTC step %u\n",planes_ok?"READBACK MATCH":"FAILED",ps,step);
            printf("Mode 0Dh playback: 24 x 32 changes %lu ms burst, %lu ms vblank\n",pb,pv);}
        if(ok&&cfg->qr_version==40&&cfg->ecc==0&&cfg->module_pixels==1) {
            u16 cw=qr_codeword_bytes(cfg),bi;u32 br,bu,bp,ber=0,bt0;int bok=1,bverify=0,cok,rok,aok;
            /* Four complete DATA QRs occupy 14,824 bytes here.  This is
             * benchmark-only staging memory; the actual renderer test reads
             * only from VGA after each plane has been written. */
            for(bi=0;bi<cfg->frame_payload;++bi)record_body[bi]=(u8)(0x5A^(bi*29U));
            for(bi=0;bi<4&&bok;++bi) {
                rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,900UL+bi,bi,4,0,
                    (u32)bi*cfg->frame_payload,record_body,cfg->frame_payload);
                if(!qr_encode(raw_frame,rawlen,cfg,bi!=0))bok=0;
                else {if(!bi)_fmemcpy(producer_disk[1],qr_code,QR_BUFFER+1);
                    _fmemcpy(producer_disk[0]+(u32)bi*cw,qr_temp,cw);}
            }
            if(bok&&vga_benchmark_plane_batch4(producer_disk[1],producer_disk[0],cw,177,
                                                cfg->invert,&br,&bu,&bp,&bverify))
                printf("Mode 0Dh batch4: %s, render %lu + upload %lu ms; 24 x 4 playback %lu ms\n",
                    bverify?"READBACK MATCH":"FAILED",br,bu,bp);
            else puts("Mode 0Dh batch4: FAILED");
            bt0=timer_ticks();
            for(bi=0;bi<4&&bok;++bi) {
                rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,904UL+bi,bi,4,0,
                    (u32)(4+bi)*cfg->frame_payload,record_body,cfg->frame_payload);
                if(!qr_encode(raw_frame,rawlen,cfg,1))bok=0;
                else _fmemcpy(producer_disk[0]+(u32)bi*cw,qr_temp,cw);
            }
            ber=timer_elapsed_ms(bt0,timer_ticks());
            if(bok&&vga_benchmark_plane_batch4_steady(producer_disk[0],cw,&br,&bu,&bp,&bverify))
                printf("Mode 0Dh warm batch4: QR %lu + render %lu + upload %lu ms; 24 x 4 playback %lu ms\n",
                    ber,br,bu,bp);
            else puts("Mode 0Dh warm batch4: FAILED");
            if(bok&&benchmark_xor3_symbol(cfg,record_body,0,1,2,&cok,&rok,&aok))
                printf("Mode 0Dh XOR ABC: codewords %s, raster %s, Color Plane Enable %s\n",
                    cok?"MATCH":"FAIL",rok?"MATCH":"FAIL",aok?"MATCH":"FAIL");
            else puts("Mode 0Dh XOR ABC: FAILED");
            if(bok&&benchmark_xor3_symbol(cfg,record_body,0,1,3,&cok,&rok,&aok))
                printf("Mode 0Dh XOR ABD: codewords %s, raster %s, Color Plane Enable %s\n",
                    cok?"MATCH":"FAIL",rok?"MATCH":"FAIL",aok?"MATCH":"FAIL");
            else puts("Mode 0Dh XOR ABD: FAILED");
            if(bok&&benchmark_xor3_symbol(cfg,record_body,0,2,3,&cok,&rok,&aok))
                printf("Mode 0Dh XOR ACD: codewords %s, raster %s, Color Plane Enable %s\n",
                    cok?"MATCH":"FAIL",rok?"MATCH":"FAIL",aok?"MATCH":"FAIL");
            else puts("Mode 0Dh XOR ACD: FAILED");
            if(bok&&benchmark_xor3_symbol(cfg,record_body,1,2,3,&cok,&rok,&aok))
                printf("Mode 0Dh XOR BCD: codewords %s, raster %s, Color Plane Enable %s\n",
                    cok?"MATCH":"FAIL",rok?"MATCH":"FAIL",aok?"MATCH":"FAIL");
            else puts("Mode 0Dh XOR BCD: FAILED");
            {u16 pchanged=0;int pdirect=0;
            if(bok&&benchmark_plane4_parity(cfg,&cok,&pdirect,&pchanged,&rok,&aok,&display_ok))
                printf("PLANE 4+1 affine parity: codewords %s, sparse raster %s (%u codewords), VGA raster %s, restore %s, Color Plane Enable %s\n",
                    cok?"MATCH":"FAIL",pdirect?"MATCH":"FAIL",pchanged,rok?"MATCH":"FAIL",aok?"MATCH":"FAIL",display_ok?"MATCH":"FAIL");
            else puts("PLANE 4+1 affine parity: FAILED");
            }
            qr_delta_ready=0;
        }
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
            else if(scfg.plane_width)display_count+=(u16)((data_count/scfg.plane_width));
            qr_delta_ready=0;last_visible_tick=0;
            /* PLANE benchmarks must enter through the same resident backend
             * as a transfer; a generic seed QR would invalidate the timing. */
            if(!scfg.plane_width&&
               !show_frame(&current_window,0,&scfg,0x6A67C69DUL,0,0,0,qr_mask))schedule_ok=0;
#ifdef DOSFER_PROFILE
            memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
            memset(dosferProtocolProfileTicks,0,sizeof(dosferProtocolProfileTicks));
            memset(dosferVgaProfileTicks,0,sizeof(dosferVgaProfileTicks));
            memset(dosferPlaneProfileTicks,0,sizeof(dosferPlaneProfileTicks));
            memset(dosferPlaneVgaProfileTicks,0,sizeof(dosferPlaneVgaProfileTicks));
#endif
            t0=timer_ticks();
            if(scfg.plane_width) {
                if(!transmit(&current_window,&scfg,0x6A67C69DUL,0,0,0,0))schedule_ok=0;
            } else for(si=0;si<data_count&&schedule_ok;++si) {
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
                if(scfg.plane_width)printf("V40 %s resident schedule (prepare + register playback): %u DATA / %u displays in %lu ms (%lu.%02lu FPS)\n",
                    redundancy_name(cfg),data_count,display_count,schedule_ms,fps100/100UL,fps100%100UL);
                else printf("V40 %s schedule: %u DATA / %u displays in %lu ms (%lu.%02lu FPS)\n",
                    redundancy_name(cfg),data_count,display_count,schedule_ms,fps100/100UL,fps100%100UL);
#ifdef DOSFER_PROFILE
                if(scfg.plane_width) {
                    u16 pg=(u16)(data_count/scfg.plane_width);
                    u32 prep_ms=timer_elapsed_ms(0,dosferPlaneProfileTicks[2]);
                    u32 play_ms=timer_elapsed_ms(0,dosferPlaneProfileTicks[3]);
                    u32 useful_ms=timer_elapsed_ms(0,dosferPlaneProfileTicks[4]);
                    printf("  PLANE groups: spool %lu, payload %lu, prepare %lu (%lu/group), correction %lu ms\n",
                        timer_elapsed_ms(0,dosferPlaneProfileTicks[0]),timer_elapsed_ms(0,dosferPlaneProfileTicks[1]),
                        prep_ms,pg?prep_ms/pg:0,timer_elapsed_ms(0,dosferPlaneProfileTicks[6]));
                    printf("  PLANE timing: register/play overhead %lu ms/group, visible configured %u / actual %lu ms/symbol, useful hold work %lu, idle %lu ms\n",
                        pg?play_ms/pg:0,scfg.hold_ms,
                        plane_last_visible_count?timer_elapsed_ms(0,plane_last_visible)/plane_last_visible_count:0,
                        useful_ms,timer_elapsed_ms(0,dosferPlaneProfileTicks[5]));
                    printf("  PLANE queue: READY minimum %u, starvation %lu, inter-group gap %lu ms, longest producer chunk %lu ms, sustained %lu.%02lu symbols/s, useful %lu B/s\n",
                        plane_last_min_ready,plane_last_starvation,timer_elapsed_ms(0,plane_last_intergroup),
                        timer_elapsed_ms(0,plane_last_max_chunk),
                        fps100/100UL,fps100%100UL,
                        schedule_ms?(u32)data_count*scfg.frame_payload*1000UL/schedule_ms:0);
                    printf("  PLANE VGA: raster %lu, upload %lu, readback %lu, select+retrace %lu, correction apply %lu, restore %lu ms\n",
                        timer_elapsed_ms(0,dosferPlaneVgaProfileTicks[0]),timer_elapsed_ms(0,dosferPlaneVgaProfileTicks[1]),
                        timer_elapsed_ms(0,dosferPlaneVgaProfileTicks[2]),timer_elapsed_ms(0,dosferPlaneVgaProfileTicks[3]),
                        timer_elapsed_ms(0,dosferPlaneVgaProfileTicks[4]),timer_elapsed_ms(0,dosferPlaneVgaProfileTicks[5]));
                }
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
            if(cfg->plane_width) displayed+=(cfg->window_frames/cfg->plane_width);
            else if(cfg->chain_width)
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
    if(!stricmp(p,"PLANE3")){cfg->plane_width=3;cfg->chain_width=cfg->redundancy=0;return 1;}
    if(!stricmp(p,"PLANE4")){cfg->plane_width=4;cfg->chain_width=cfg->redundancy=0;return 1;}
    if(toupper(*p)=='C') {
        v=strtol(p+1,&end,10);
        if(!p[1]||*end||v<2||v>MAX_WINDOW||(v&1))return -1;
        cfg->chain_width=(u8)v;cfg->redundancy=cfg->plane_width=0;return 1;
    }
    v=strtol(p,&end,10);
    if(*end||v<0||v>MAX_WINDOW)return -1;
    cfg->redundancy=(u8)v;cfg->chain_width=cfg->plane_width=0;return 1;
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
    puts("Production backend: V40-L Mode 0Dh PLANE4, 320x200, 2904-byte payload");
    puts("/RE:PLANE3|PLANE4   Resident VGA bitplane group width (default PLANE4)");
    puts("/HOLD:ms or /SPEED:ms  Minimum frame hold 0..60000 ms");
    puts("/WINDOW:n or /W:n    Logical protocol window 4..128 (constant-memory stream)");
    puts("/REPEAT:n or /R:n    Full passes per batch 1..20");
    puts("/MASK:n              Fixed QR mask 0..7 (default 0; payload is whitened)");
    puts("/INVERT /NOINVERT    Select black/white polarity");
    puts("/BEEP /NOBEEP        End-of-batch sound");
    printf("Current: /V:%u /ECC:%c /SCALE:%u /PAYLOAD:%u /HOLD:%u /WINDOW:%u /REPEAT:%u /MASK:%u /RE:%s %s %s\n",
        cfg->qr_version,"LMQH"[cfg->ecc],cfg->module_pixels,cfg->frame_payload,
        cfg->hold_ms,cfg->window_frames,cfg->repetitions,qr_mask,redundancy_name(cfg),
        cfg->invert?"/INVERT":"/NOINVERT",cfg->speaker?"/BEEP":"/NOBEEP");
    puts("Example: DOSFER /HOLD:50 /W:128 /RE:PLANE4 FILE.ZIP");
    puts("The real resident C1 waits for Enter so you can focus the camera; Esc cancels.");
    puts("PLANE transfer streams forward after Enter; replay/rescue spool controls are being rebuilt.");
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
    printf("Build ID: %s (SHA-256 is recorded in build\\DOSFER.SHA256).\n",DOSFER_BUILD_ID);
    puts("Preparing the first QR code...");
    rc=run_transfer(mf,&cfg);sender_cleanup();fclose(mf);remove(MANIFEST_NAME);
    if(rc){printf("Transfer complete. Session %08lX, %lu frames, %lu bytes. Returning to DOS.\n",
        completed_session,completed_frames,completed_bytes);fflush(stdout);exit_to_dos(0);return 0;}
    return 1;
}
