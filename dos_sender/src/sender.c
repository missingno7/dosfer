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
    int disk_slot;
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
static int chain_enabled;
static u16 chain_anchor=16;
static u8 chain_payload[MAX_FRAME_PAYLOAD];

static u16 qr_codeword_bytes(const Config *cfg) {
    return (u16)qrcodegen_dosferCodewordBytes(cfg->qr_version);
}
static int can_delta(const Config *cfg,u8 display_mask) {
    return qr_delta_ready&&display_mask==encoded_qr_mask&&
        (cfg->module_pixels==2||(cfg->qr_version==40&&cfg->module_pixels==1));
}

static void default_config(Config *c) {
    memset(c,0,sizeof(*c)); c->qr_version=15; c->ecc=1; c->module_pixels=4;
    c->repetitions=1; c->frame_payload=364; c->hold_ms=750;
    c->window_frames=32; c->speaker=1;
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
static void start_file(void) {
    producer.source=fopen(producer.entry.source,"rb"); producer.file_offset=0;
    producer.file_crc=0; producer.disk_slot=0;
    producer.disk_len[0]=producer.disk_len[1]=0;
    producer.disk_pos[0]=producer.disk_pos[1]=0;
    if(producer.source) {
        producer.disk_len[0]=(u16)fread(producer.disk[0],1,DISK_BUFFER,producer.source);
        producer.disk_len[1]=(u16)fread(producer.disk[1],1,DISK_BUFFER,producer.source);
    }
}
static u16 read_piece(u8 *out,u16 want) {
    u16 got=0,n; int s;
    while(got<want && producer.file_offset<producer.entry.size) {
        s=producer.disk_slot;
        if(producer.disk_pos[s]>=producer.disk_len[s]) {
            producer.disk_len[s]=(u16)fread(producer.disk[s],1,DISK_BUFFER,producer.source);
            producer.disk_pos[s]=0;
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
        f->payload_len=make_record(f->payload,RT_SESSION,producer.record_id++,0,body,12);
        producer.state=1; return 1;
    }
    if(producer.state==1) {
        if(fread(&producer.entry,1,sizeof(producer.entry),producer.manifest)!=sizeof(producer.entry)) {
            producer.state=5; goto again;
        }
        if(producer.entry.kind==2) {
            n=path_meta(body,&producer.entry,0); producer.dir_count++;
            f->payload_len=make_record(f->payload,RT_DIRECTORY,producer.record_id++,0,body,n);
            return 1;
        }
        n=path_meta(body,&producer.entry,1); start_file();
        if(!producer.source){printf("Cannot open %s\n",producer.entry.source);return -1;}
        producer.file_count++; producer.total_bytes+=producer.entry.size; producer.state=2;
        f->stream_id=producer.entry.file_id;
        f->payload_len=make_record(f->payload,RT_FILE_BEGIN,producer.record_id++,producer.entry.file_id,body,n);
        return 1;
    }
    if(producer.state==2) {
        if(producer.file_offset>=producer.entry.size){producer.state=3;goto again;}
        cap=(u16)(cfg->frame_payload-RECORD_HEADER_SIZE-4); off=producer.file_offset;
        put_u32(body,off); n=read_piece(body+4,cap);
        if(!n && producer.file_offset<producer.entry.size)return -1;
        f->stream_id=producer.entry.file_id;f->stream_offset=off;
        f->payload_len=make_record(f->payload,RT_FILE_DATA,producer.record_id++,producer.entry.file_id,body,(u16)(n+4));
        return 1;
    }
    if(producer.state==3) {
        put_u32(body,producer.entry.size);put_u32(body+4,producer.file_crc);
        fclose(producer.source);producer.source=0;producer.state=1;
        f->stream_id=producer.entry.file_id;
        f->payload_len=make_record(f->payload,RT_FILE_END,producer.record_id++,producer.entry.file_id,body,8);
        return 1;
    }
    if(producer.state==5) {
        put_u32(body,producer.file_count);put_u32(body+4,producer.dir_count);
        put_u32(body+8,0);put_u32(body+12,producer.total_bytes);
        f->payload_len=make_record(f->payload,RT_TRANSFER_END,producer.record_id++,0,body,16);
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
static int qr_encode_mask(const u8 *data,u16 n,const Config *cfg,int delta_only,u8 mask) {
    int ok;
    memcpy(qr_temp,data,n);
    /* AUTO evaluates all eight masks and dominated runtime on a 386. Mask 0 is
       fully standard-compliant and cut measured DOSBox encoding time sharply. */
    qrcodegen_dosferSetCodewordsOnly(delta_only!=0);
    ok=qrcodegen_encodeBinary(qr_temp,n,qr_code,(enum qrcodegen_Ecc)cfg->ecc,
        cfg->qr_version,cfg->qr_version,(enum qrcodegen_Mask)mask,0);
    if(ok)encoded_qr_mask=mask;return ok;
}
static int qr_encode(const u8 *data,u16 n,const Config *cfg,int delta_only) {
    return qr_encode_mask(data,n,cfg,delta_only,qr_mask);
}
static int show_frame(const Window *w,u16 i,const Config *cfg,u32 session,int repeated,int waiting,u16 hold_ms,u8 display_mask) {
    char a[79],b[79];u16 n;u32 elapsed;int rescue=hold_ms!=cfg->hold_ms||display_mask!=qr_mask;
    int delta=can_delta(cfg,display_mask);
    const PendingFrame *f=&w->frames[i];
    n=make_frame(raw_frame,FK_DATA,(repeated?FF_REPEATED:0)|FF_WHITENED,session,w->id,f->global_index,
        i,w->count,f->stream_id,f->stream_offset,f->payload,f->payload_len);
    if(!qr_encode_mask(raw_frame,n,cfg,delta,display_mask))return 0;
    if(cfg->module_pixels==1){if(waiting)strcpy(a,"READY - focus camera - Enter / Esc");
        else if(rescue)sprintf(a,"W%lu F%u/%u RESCUE %ums M%u",w->id+1,i+1,w->count,hold_ms,display_mask);
        else sprintf(a,"W%lu F%u/%u V%u%c %uB",w->id+1,i+1,w->count,cfg->qr_version,"LMQH"[cfg->ecc],f->payload_len);}
    else sprintf(a,"Session %08lX  Window %lu  Frame %u/%u  QR v%u %c W",
        session,w->id+1,i+1,w->count,cfg->qr_version,"LMQH"[cfg->ecc]);
    if(waiting)strcpy(b,"READY - focus camera, then press Enter to start; Esc cancels");
    else if(rescue)sprintf(b,"RESCUE hold %u ms  mask %u  Payload %u  Global %lu",hold_ms,display_mask,f->payload_len,f->global_index);
    else sprintf(b,"Hold %u ms  Payload %u  Global %lu  controls at window end",
        cfg->hold_ms,f->payload_len,f->global_index);
    if(last_visible_tick) {
        elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());
        if(elapsed<hold_ms)timer_wait_ms((u16)(hold_ms-elapsed));
    }
    if(!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,a,b,(int)(f->global_index&1),delta))return 0;
    qr_delta_ready=vga_delta_ready();
    last_visible_tick=timer_ticks();return 1;
}
static int show_chain(const Window *w,u16 i,const Config *cfg,u32 session,u16 hold_ms) {
    const PendingFrame *left=&w->frames[i],*right=&w->frames[i+1];
    u16 j,k,words,common=left->payload_len<right->payload_len?left->payload_len:right->payload_len;
    u16 n=left->payload_len>right->payload_len?left->payload_len:right->payload_len,rawlen;
    u32 elapsed,lengths=((u32)left->payload_len<<16)|right->payload_len;
    char a[79],b[79];int delta=can_delta(cfg,qr_mask);
    words=common>>2;for(k=0;k<words;++k)((u32 *)chain_payload)[k]=((const u32 far *)left->payload)[k]^((const u32 far *)right->payload)[k];
    j=(u16)(words<<2);for(;j<common;++j)chain_payload[j]=left->payload[j]^right->payload[j];
    if(left->payload_len>common)_fmemcpy(chain_payload+common,left->payload+common,left->payload_len-common);
    else if(right->payload_len>common)_fmemcpy(chain_payload+common,right->payload+common,right->payload_len-common);
    rawlen=make_frame(raw_frame,FK_CHAIN_XOR,FF_WHITENED,session,w->id,left->global_index,
        i,w->count,lengths,0,chain_payload,n);
    if(!qr_encode_mask(raw_frame,rawlen,cfg,delta,qr_mask))return 0;
    if(cfg->module_pixels==1)sprintf(a,"W%lu XOR %u-%u/%u V%u%c %uB",w->id+1,i+1,i+2,w->count,cfg->qr_version,"LMQH"[cfg->ecc],n);
    else sprintf(a,"Session %08lX  Window %lu  Recovery %u-%u/%u  QR v%u %c",
        session,w->id+1,i+1,i+2,w->count,cfg->qr_version,"LMQH"[cfg->ecc]);
    sprintf(b,"CHAIN XOR  Hold %u ms  Payload %u  Global %lu",hold_ms,n,left->global_index);
    if(last_visible_tick){elapsed=timer_elapsed_ms(last_visible_tick,timer_ticks());if(elapsed<hold_ms)timer_wait_ms((u16)(hold_ms-elapsed));}
    if(!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,
        cfg->module_pixels,cfg->invert,a,b,(int)(left->global_index&1),delta))return 0;
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
    u16 r,i,hold_ms=cfg->hold_ms;u32 adjusted;u8 display_mask=qr_mask;int factor=1;
    if(selected&&chosen) {
        if((u32)chosen*16UL<=w->count)factor=4;
        else if((u32)chosen*8UL<=w->count)factor=3;
        else if((u32)chosen*2UL<=w->count)factor=2;
        adjusted=(cfg->hold_ms<100?100UL:cfg->hold_ms)*(u32)factor;
        hold_ms=(u16)(adjusted>60000UL?60000UL:adjusted);
        display_mask=(u8)((qr_mask+1+(rescue_round?rescue_round-1:0)%7)&7);
    }
    for(r=0;r<cfg->repetitions;++r) for(i=0;i<w->count;++i)
        if(!selected || selected[i]) {
            if(!show_frame(w,i,cfg,session,r>0,0,hold_ms,display_mask))return 0;
            if(!selected&&chain_enabled&&i+1<w->count) {
                if(!show_chain(w,i,cfg,session,hold_ms))return 0;
                if(chain_anchor&&((i+1)%chain_anchor)==0)
                    if(!show_frame(w,i,cfg,session,0,0,hold_ms,display_mask))return 0;
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
    if(!vga_enter()){puts("Not enough memory for VGA buffer");return 0;}qr_delta_ready=0;
    if(!show_frame(&current_window,0,cfg,session,0,1,cfg->hold_ms,qr_mask)){vga_leave();return 0;}
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
        if(!fill_window(&current_window,cfg,session,window_id)){vga_leave();config_save(cfg,"DOSFER.CFG");
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
        strcpy(b,"+/- hold  V version  E ECC  P payload  I invert  S save  Esc done");
        vga_show_qr(qr_code,qrcodegen_getSize(qr_code),cfg->module_pixels,cfg->invert,a,b,(int)(seq&1));
        timer_wait_ms(cfg->hold_ms);seq++;
        if(!_bios_keybrd(_KEYBRD_READY))continue;key=_bios_keybrd(_KEYBRD_READ);ascii=(u8)key;scan=(u8)(key>>8);
        if(ascii==27||scan==0x01)break;if(ascii=='+'||ascii=='='||scan==0x0D||scan==0x4E){if(cfg->hold_ms>=50)cfg->hold_ms-=50;else cfg->hold_ms=0;}
        if(ascii=='-'||scan==0x0C||scan==0x4A)cfg->hold_ms+=50;
        if(ascii=='v'||ascii=='V'||scan==0x2F){cfg->qr_version=(cfg->qr_version>=40)?10:(u8)(cfg->qr_version+5);cfg->module_pixels=(u8)(400/(cfg->qr_version*4+25));if(cfg->module_pixels<1)cfg->module_pixels=1;}
        if(ascii=='e'||ascii=='E'||scan==0x12)cfg->ecc=(u8)((cfg->ecc+1)%3);
        if(ascii=='p'||ascii=='P'||scan==0x19)cfg->frame_payload=(cfg->frame_payload>=2200)?128:(u16)(cfg->frame_payload+128);
        if(ascii=='i'||ascii=='I'||scan==0x17)cfg->invert=!cfg->invert;
        if(ascii=='s'||ascii=='S'||scan==0x1F)config_save(cfg,"DOSFER.CFG");
    }
    vga_leave();config_save(cfg,"DOSFER.CFG");return 1;
}
static void benchmark(const char *path,Config *cfg) {
    static const u8 whitening_test[16]={0x9D,0x3B,0x19,0x23,0xA8,0xAC,0x39,0x89,0x3E,0xAD,0x32,0x29,0xF4,0x3D,0x3F,0xEE};
    FILE *f;u8 *b=producer.disk[0];u16 n;u32 bytes=0,crc=0,t0,t1,protocol_ms,encode_ms,build_ms,copy_ms,text_ms,frame_ms,useful;int i,encoded=0;u16 rawlen;
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
        printf("VGA BIOS status: 25 in %lu ms (%lu ms/frame)\n",text_ms,text_ms/25UL);
        frame_ms=((protocol_ms+encode_ms)>cfg->hold_ms?(protocol_ms+encode_ms):cfg->hold_ms)+build_ms/25UL+copy_ms/25UL+text_ms/25UL;
        useful=cfg->frame_payload>28?cfg->frame_payload-28:0;
        printf("Projected pipeline: %lu ms/frame, %lu raw QR B/s, %lu file-data B/s\n",
            frame_ms,frame_ms?rawlen*1000UL/frame_ms:0,frame_ms?useful*1000UL/frame_ms:0);
    }
    if((cfg->module_pixels==2||(cfg->qr_version==40&&cfg->module_pixels==1))&&vga_enter()){
        u32 first_ms,steady_ms,dirty_hash=0,full_hash=0,switch_hash=0;int delta,ok=1,redraw_ok=0,switch_ok=0,display_ok=0;u8 alt_mask=(u8)((qr_mask+1)&7);qr_delta_ready=0;
        rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,0,0,1,0,0,b,cfg->frame_payload);
        t0=timer_ticks();if(!qr_encode(raw_frame,rawlen,cfg,0)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Streaming benchmark","first frame",0,0))ok=0;t1=timer_ticks();first_ms=timer_elapsed_ms(t0,t1);qr_delta_ready=vga_delta_ready();
#ifdef DOSFER_PROFILE
        memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
#endif
        t0=timer_ticks();for(i=1;i<25&&ok;++i){rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,(u32)i,0,1,0,0,b,cfg->frame_payload);delta=qr_delta_ready;if(!qr_encode(raw_frame,rawlen,cfg,delta)||!vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Streaming benchmark","delta frame",i&1,delta))ok=0;qr_delta_ready=vga_delta_ready();}t1=timer_ticks();steady_ms=timer_elapsed_ms(t0,t1);
#ifdef DOSFER_PROFILE
        printf("  steady QR x24: pack %lu  ECC %lu  func1 %lu  data %lu  func2 %lu  mask %lu ms total\n",
            timer_elapsed_ms(0,dosferQrProfileTicks[0]),timer_elapsed_ms(0,dosferQrProfileTicks[1]),
            timer_elapsed_ms(0,dosferQrProfileTicks[2]),timer_elapsed_ms(0,dosferQrProfileTicks[3]),
            timer_elapsed_ms(0,dosferQrProfileTicks[4]),timer_elapsed_ms(0,dosferQrProfileTicks[5]));
#endif
        if(ok){dirty_hash=vga_screen_hash();display_ok=vga_display_matches();if(qr_encode(raw_frame,rawlen,cfg,0)&&vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Redraw verification","full redraw",0,0)){full_hash=vga_screen_hash();redraw_ok=dirty_hash==full_hash;
            if(qr_encode_mask(raw_frame,rawlen,cfg,0,alt_mask)&&vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Rescue verification","alternate mask",0,0)&&qr_encode_mask(raw_frame,rawlen,cfg,0,qr_mask)&&vga_show_qr_stream(qr_code,qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,cfg->module_pixels,cfg->invert,"Redraw verification","full redraw",0,0)){switch_hash=vga_screen_hash();switch_ok=switch_hash==full_hash;}}}
        if(ok){u32 du,dc,dt;vga_benchmark_delta(qr_temp,qr_codeword_bytes(cfg),cfg->qr_version*4+17,24,&du,&dc,&dt);
            printf("Delta VGA: update %lu, retrace+partial copy %lu, status %lu ms/frame\n",du/24UL,dc/24UL,dt/24UL);}
        vga_leave();if(ok){u32 fps100=steady_ms?2400000UL/steady_ms:0,displayed=cfg->window_frames,effective;
            if(chain_enabled)displayed=2UL*cfg->window_frames-1UL+(chain_anchor?(cfg->window_frames-1)/chain_anchor:0);
            effective=steady_ms?useful*24000UL/steady_ms*cfg->window_frames/displayed:0;
            printf("V%u stateful: first %lu ms, next 24 in %lu ms = %lu ms/frame (%lu.%02lu FPS)\n",cfg->qr_version,first_ms,steady_ms,steady_ms/24UL,fps100/100UL,fps100%100UL);
            printf("V%u effective schedule: %lu data / %lu displays, %lu file-data B/s\n",cfg->qr_version,(u32)cfg->window_frames,displayed,effective);
            if(chain_enabled){u32 data100=fps100*cfg->window_frames/displayed,xor100=fps100*(cfg->window_frames-1)/displayed;
                printf("Displayed rates: DATA %lu.%02lu/s, XOR %lu.%02lu/s, anchors every %u\n",data100/100UL,data100%100UL,xor100/100UL,xor100%100UL,chain_anchor);}
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
static int config_option(Config *cfg,const char *arg) {
    long v;int rc;const char *p;
    if(!stricmp(arg,"/FPS")||!stricmp(arg,"-FPS")){cfg->qr_version=12;cfg->ecc=1;cfg->module_pixels=4;cfg->frame_payload=239;cfg->hold_ms=0;cfg->window_frames=64;return 1;}
    if(!stricmp(arg,"/BULK")||!stricmp(arg,"-BULK")){cfg->qr_version=40;cfg->ecc=1;cfg->module_pixels=2;cfg->frame_payload=2283;cfg->hold_ms=0;cfg->window_frames=32;return 1;}
    if(!stricmp(arg,"/TURBO")||!stricmp(arg,"-TURBO")){cfg->qr_version=25;cfg->ecc=0;cfg->module_pixels=2;cfg->frame_payload=1225;cfg->hold_ms=0;cfg->window_frames=32;chain_enabled=1;chain_anchor=16;return 1;}
    if(!stricmp(arg,"/TURBO40")||!stricmp(arg,"-TURBO40")){cfg->qr_version=40;cfg->ecc=0;cfg->module_pixels=1;cfg->frame_payload=2905;cfg->hold_ms=0;cfg->window_frames=32;chain_enabled=1;chain_anchor=16;return 1;}
    if(!stricmp(arg,"/T25")||!stricmp(arg,"-T25")){cfg->qr_version=25;cfg->ecc=0;cfg->module_pixels=2;cfg->frame_payload=1225;cfg->hold_ms=0;cfg->window_frames=32;chain_enabled=1;return 1;}
    if(!stricmp(arg,"/T30")||!stricmp(arg,"-T30")){cfg->qr_version=30;cfg->ecc=0;cfg->module_pixels=2;cfg->frame_payload=1684;cfg->hold_ms=0;cfg->window_frames=32;chain_enabled=1;return 1;}
    if(!stricmp(arg,"/T35")||!stricmp(arg,"-T35")){cfg->qr_version=35;cfg->ecc=0;cfg->module_pixels=2;cfg->frame_payload=2255;cfg->hold_ms=0;cfg->window_frames=32;chain_enabled=1;return 1;}
    if(!stricmp(arg,"/T40")||!stricmp(arg,"-T40")){cfg->qr_version=40;cfg->ecc=0;cfg->module_pixels=2;cfg->frame_payload=2905;cfg->hold_ms=0;cfg->window_frames=32;chain_enabled=1;return 1;}
    if(!stricmp(arg,"/CHAIN")||!stricmp(arg,"-CHAIN")){chain_enabled=1;return 1;}
    if(!stricmp(arg,"/NOCHAIN")||!stricmp(arg,"-NOCHAIN")){chain_enabled=0;return 1;}
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
static int preset_option(const char *arg) {
    return !stricmp(arg,"/FPS")||!stricmp(arg,"-FPS")||
        !stricmp(arg,"/BULK")||!stricmp(arg,"-BULK")||
        !stricmp(arg,"/TURBO")||!stricmp(arg,"-TURBO")||
        !stricmp(arg,"/TURBO40")||!stricmp(arg,"-TURBO40")||
        !stricmp(arg,"/T25")||!stricmp(arg,"-T25")||
        !stricmp(arg,"/T30")||!stricmp(arg,"-T30")||
        !stricmp(arg,"/T35")||!stricmp(arg,"-T35")||
        !stricmp(arg,"/T40")||!stricmp(arg,"-T40");
}
static void usage(const Config *cfg) {
    puts("DOSfer 1.0 - optical DOS-to-Android file transfer");
    puts("DOSFER [options] file_or_directory [more paths ...]");
    puts("DOSFER /CAL [options]");
    puts("DOSFER /BENCH file [options]");
    puts("");
    puts("/FPS                  Preset: v12-M, 239 B, hold 0, 64-frame windows");
    puts("/BULK                 Preset: v40-M, 2283 B, hold 0, 32-frame windows");
    puts("/TURBO                Preset: v25-L chained, ~10 FPS on 3000-cycle 386");
    puts("/TURBO40              V40-L chained, 320x200 hardware page flipping");
    puts("/T25 /T30 /T35 /T40  ECC-L chained benchmark presets");
    puts("/CHAIN /NOCHAIN      Adjacent-frame XOR recovery (experimental)");
    puts("/ANCHOR:n            Repeat each nth data frame in chain mode; 0 disables");
    puts("/V:n or /VERSION:n   QR version 5..40");
    puts("/ECC:L|M|Q|H         QR error correction");
    puts("/SCALE:n             Module size 1..6 VGA pixels");
    puts("/PAYLOAD:n or /P:n   Record payload 96..2905 bytes");
    puts("/HOLD:ms or /SPEED:ms  Minimum frame hold 0..60000 ms");
    puts("/WINDOW:n or /W:n    Frames per acknowledged batch 4..64");
    puts("/REPEAT:n or /R:n    Full passes per batch 1..20");
    puts("/MASK:n              Fixed QR mask 0..7 (default 0; payload is whitened)");
    puts("/INVERT /NOINVERT    Select black/white polarity");
    puts("/BEEP /NOBEEP        End-of-batch sound");
    printf("Current: /V:%u /ECC:%c /SCALE:%u /PAYLOAD:%u /HOLD:%u /WINDOW:%u /REPEAT:%u /MASK:%u %s %s\n",
        cfg->qr_version,"LMQH"[cfg->ecc],cfg->module_pixels,cfg->frame_payload,
        cfg->hold_ms,cfg->window_frames,cfg->repetitions,qr_mask,
        cfg->invert?"/INVERT":"/NOINVERT",cfg->speaker?"/BEEP":"/NOBEEP");
    puts("Example: DOSFER /V:15 /ECC:M /SCALE:4 /PAYLOAD:364 /HOLD:750 /W:12 FILE.ZIP");
    puts("The first QR waits for Enter so you can focus the camera; Esc cancels.");
    puts("M rescues missing frames with adaptive hold/mask; R repeats that set.");
    puts("Enter a blank M list to clear it and replay the complete window.");
    puts("Every batch stops for Enter/R/M/B; command-line options never auto-advance.");
}
int main(int argc,char **argv) {
    Config cfg;FILE *mf;int i,rc,action=0,path_count=0;char path[PATH_BYTES],again[8];const char *bench_path=0;u32 est;
    default_config(&cfg);config_load(&cfg,"DOSFER.CFG");
    /* Apply presets first so explicit switches override them regardless of
       command-line order (e.g. /HOLD:100 ... /TURBO40). */
    for(i=1;i<argc;++i)if(preset_option(argv[i]))config_option(&cfg,argv[i]);
    for(i=1;i<argc;++i) {
        if(!stricmp(argv[i],"/?")||!stricmp(argv[i],"/HELP")||!stricmp(argv[i],"-HELP")){usage(&cfg);return 0;}
        if(!stricmp(argv[i],"/CAL")||!stricmp(argv[i],"-CAL")){if(action&&action!=1){puts("Choose either /CAL or /BENCH.");return 1;}action=1;continue;}
        if(!stricmp(argv[i],"/BENCH")||!stricmp(argv[i],"-BENCH")){if(action&&action!=2){puts("Choose either /CAL or /BENCH.");return 1;}action=2;continue;}
        if(preset_option(argv[i]))continue;
        rc=config_option(&cfg,argv[i]);if(rc<0){printf("Invalid option value: %s\n",argv[i]);return 1;}if(rc)continue;
        if(argv[i][0]=='/'||argv[i][0]=='-'){printf("Unknown option: %s\n",argv[i]);return 1;}
        if(action==2&&!bench_path)bench_path=argv[i];else path_count++;
    }
    if(action==1){if(path_count||bench_path){puts("/CAL does not take a file path.");return 1;}return calibration(&cfg)?0:1;}
    if(action==2){if(!bench_path||path_count){puts("Usage: DOSFER /BENCH file [options]");return 1;}benchmark(bench_path,&cfg);return 0;}
    mf=fopen(MANIFEST_NAME,"w+b");if(!mf){puts("Cannot create DOSFER.$$$ in current directory.");return 1;}
    if(path_count){for(i=1;i<argc;++i)if(argv[i][0]!='/'&&argv[i][0]!='-')if(!add_selection(mf,argv[i])){fclose(mf);remove(MANIFEST_NAME);return 1;}}
    else do {printf("File or directory: ");if(!fgets(path,sizeof(path),stdin))break;path[strcspn(path,"\r\n")]=0;
        if(!add_selection(mf,path)){fclose(mf);remove(MANIFEST_NAME);return 1;}
        printf("Add another? [y/N] ");fgets(again,sizeof(again),stdin);
    } while(again[0]=='y'||again[0]=='Y');
    if(!selected_files&&!selected_dirs){usage(&cfg);fclose(mf);remove(MANIFEST_NAME);return 1;}
    est=1+selected_dirs+selected_files*2+selected_bytes/(cfg.frame_payload-28)+1;
    printf("Selected: %lu files, %lu directories, %lu bytes, approximately %lu QR frames.\n",selected_files,selected_dirs,selected_bytes,est);
    printf("Settings: QR v%u-%c mask %u, %u px modules, %u ms hold, window %u, repeats %u.\n",cfg.qr_version,"LMQH"[cfg.ecc],qr_mask,cfg.module_pixels,cfg.hold_ms,cfg.window_frames,cfg.repetitions);
    puts("Preparing the first QR code...");
    rc=run_transfer(mf,&cfg);fclose(mf);remove(MANIFEST_NAME);
    if(rc){vga_leave();printf("Transfer complete. Session %08lX, %lu frames, %lu bytes. Returning to DOS.\n",
        completed_session,completed_frames,completed_bytes);fflush(stdout);exit_to_dos(0);return 0;}
    return 1;
}
