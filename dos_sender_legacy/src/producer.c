#include <dos.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "producer.h"

#define MANIFEST_NAME "DOSFER.$$$"

static u8 far record_body[MAX_FRAME_PAYLOAD];

static const char *base_name(const char *p) {
    const char *b=p,*q;
    for(q=p;*q;++q)
        if(*q=='\\'||*q=='/'||*q==':')b=q+1;
    return *b?b:"ROOT";
}

static void slash_to_forward(char *p) {
    while(*p){if(*p=='\\')*p='/';++p;}
}

static int manifest_write(FILE *f,u8 kind,const char *src,const char *rel,
        const struct find_t *d,u32 fid,SelectionStats *stats) {
    ManifestEntry e;
    if(strlen(src)>=PATH_BYTES||strlen(rel)>=PATH_BYTES) {
        printf("Path too long (max %u): %s\n",PATH_BYTES-1,src);
        return 0;
    }
    memset(&e,0,sizeof(e));
    e.kind=kind;
    e.attributes=d->attrib;
    e.dos_date=d->wr_date;
    e.dos_time=d->wr_time;
    e.size=d->size;
    e.file_id=fid;
    strcpy(e.source,src);
    strcpy(e.relative,rel);
    slash_to_forward(e.relative);
    if(fwrite(&e,1,sizeof(e),f)!=sizeof(e))return 0;
    if(kind==1){++stats->files;stats->bytes+=e.size;}
    else ++stats->dirs;
    return 1;
}

static int scan_path(FILE *mf,const char *path,const char *rel,SelectionStats *stats) {
    struct find_t d,child;
    char spec[PATH_BYTES],src[PATH_BYTES],dst[PATH_BYTES];
    unsigned rc;

    if(_dos_findfirst(path,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&d)) {
        printf("Not found: %s\n",path);
        return 0;
    }
    if(!(d.attrib&_A_SUBDIR))
        return manifest_write(mf,1,path,rel,&d,stats->files+1,stats);

    if(!manifest_write(mf,2,path,rel,&d,0,stats))return 0;
    if(strlen(path)+5>=PATH_BYTES)return 0;
    sprintf(spec,"%s\\*.*",path);
    rc=_dos_findfirst(spec,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&child);
    while(!rc) {
        if(strcmp(child.name,".")&&strcmp(child.name,"..")&&stricmp(child.name,MANIFEST_NAME)) {
            if(strlen(path)+strlen(child.name)+2>=PATH_BYTES||
               strlen(rel)+strlen(child.name)+2>=PATH_BYTES)return 0;
            sprintf(src,"%s\\%s",path,child.name);
            sprintf(dst,"%s/%s",rel,child.name);
            if(!scan_path(mf,src,dst,stats))return 0;
        }
        rc=_dos_findnext(&child);
    }
    return 1;
}

int manifest_add_selection(FILE *mf,const char *path,SelectionStats *stats) {
    char clean[PATH_BYTES];
    size_t n=strlen(path);
    struct find_t d;
    if(!n||n>=PATH_BYTES||!stats)return 0;
    strcpy(clean,path);
    while(n>1&&(clean[n-1]=='\\'||clean[n-1]=='/'))clean[--n]=0;
    if(_dos_findfirst(clean,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&d)) {
        printf("Cannot select %s\n",clean);
        return 0;
    }
    return scan_path(mf,clean,base_name(clean),stats);
}

void producer_close(Producer *p) {
    if(p&&p->source){fclose(p->source);p->source=0;}
}

void producer_free_window(Window *w) {
    u16 i;
    if(!w)return;
    for(i=0;i<MAX_WINDOW;++i)if(w->frames[i].payload){
        _ffree(w->frames[i].payload);
        w->frames[i].payload=0;
        w->frames[i].payload_capacity=0;
        w->frames[i].payload_len=0;
    }
    w->count=0;
}

void producer_init(Producer *p,FILE *mf) {
    memset(p,0,sizeof(*p));
    p->manifest=mf;
    rewind(mf);
}

static int source_matches_manifest(const Producer *p) {
    struct find_t d;
    if(_dos_findfirst(p->entry.source,
       _A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_ARCH,&d))return 0;
    return !(d.attrib&_A_SUBDIR)&&d.size==p->entry.size&&
        d.wr_date==p->entry.dos_date&&d.wr_time==p->entry.dos_time;
}

static void start_file(Producer *p) {
    p->source=fopen(p->entry.source,"rb");
    p->file_offset=0;
    p->file_crc=0;
    p->disk_slot=0;
    p->io_error=0;
    p->disk_len[0]=p->disk_len[1]=0;
    p->disk_pos[0]=p->disk_pos[1]=0;
    if(p->source&&!source_matches_manifest(p)) {
        printf("File changed after selection: %s\n",p->entry.source);
        producer_close(p);
        return;
    }
    if(p->source) {
        p->disk_len[0]=(u16)fread(p->disk[0],1,DOSFER_DISK_BUFFER,p->source);
        p->disk_len[1]=(u16)fread(p->disk[1],1,DOSFER_DISK_BUFFER,p->source);
        if(ferror(p->source))p->io_error=1;
    }
}

static u16 read_piece(Producer *p,u8 *out,u16 want) {
    u16 got=0,n;
    int slot;
    while(got<want&&p->file_offset<p->entry.size) {
        slot=p->disk_slot;
        if(p->disk_pos[slot]>=p->disk_len[slot]) {
            p->disk_len[slot]=(u16)fread(p->disk[slot],1,DOSFER_DISK_BUFFER,p->source);
            p->disk_pos[slot]=0;
            if(ferror(p->source))p->io_error=1;
            if(!p->disk_len[slot])break;
        }
        n=(u16)(p->disk_len[slot]-p->disk_pos[slot]);
        if(n>want-got)n=(u16)(want-got);
        memcpy(out+got,p->disk[slot]+p->disk_pos[slot],n);
        p->disk_pos[slot]+=n;
        got+=n;
        p->file_offset+=n;
        if(p->disk_pos[slot]>=p->disk_len[slot])p->disk_slot^=1;
    }
    p->file_crc=crc32_update(p->file_crc,out,got);
    return got;
}

static u16 path_meta(u8 *b,const ManifestEntry *e,int with_size) {
    u16 n=(u16)strlen(e->relative),pos=0;
    b[pos++]=e->attributes;
    b[pos++]=0;
    put_u16(b+pos,e->dos_date);pos+=2;
    put_u16(b+pos,e->dos_time);pos+=2;
    if(with_size){put_u32(b+pos,e->size);pos+=4;}
    put_u16(b+pos,n);pos+=2;
    memcpy(b+pos,e->relative,n);
    return (u16)(pos+n);
}

static int producer_next(Producer *p,PendingFrame *f,const Config *cfg,u32 session) {
    u8 far *body=record_body,*payload=f->payload;
    u16 payload_capacity=f->payload_capacity,n,cap;
    u32 off;
    (void)session;

    if(payload_capacity<cfg->frame_payload) {
        u8 far *larger=(u8 far *)_fmalloc(cfg->frame_payload);
        if(!larger){puts("Not enough DOS memory for transfer window");return -1;}
        if(payload)_ffree(payload);
        payload=larger;
        payload_capacity=cfg->frame_payload;
    }
    memset(f,0,sizeof(*f));
    f->payload=payload;
    f->payload_capacity=payload_capacity;
    f->global_index=p->global_index++;

again:
    if(p->state==0) {
        put_u32(body,(u32)time(NULL));
        put_u16(body+4,6);
        memcpy(body+6,"DOSFER",6);
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_SESSION,
            p->record_id++,0,body,12);
        if(!f->payload_len){puts("Session record does not fit configured payload.");return -1;}
        p->state=1;
        return 1;
    }
    if(p->state==1) {
        if(fread(&p->entry,1,sizeof(p->entry),p->manifest)!=sizeof(p->entry)) {
            p->state=5;
            goto again;
        }
        if(p->entry.kind==2) {
            n=path_meta(body,&p->entry,0);
            ++p->dir_count;
            f->payload_len=make_record(f->payload,f->payload_capacity,RT_DIRECTORY,
                p->record_id++,0,body,n);
            if(!f->payload_len){
                printf("Directory metadata does not fit /PAYLOAD:%u: %s\n",
                    cfg->frame_payload,p->entry.relative);return -1;
            }
            return 1;
        }
        n=path_meta(body,&p->entry,1);
        start_file(p);
        if(!p->source){printf("Cannot open %s\n",p->entry.source);return -1;}
        ++p->file_count;
        p->total_bytes+=p->entry.size;
        p->state=2;
        f->stream_id=p->entry.file_id;
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_FILE_BEGIN,
            p->record_id++,p->entry.file_id,body,n);
        if(!f->payload_len){
            printf("File metadata does not fit /PAYLOAD:%u: %s\n",
                cfg->frame_payload,p->entry.relative);return -1;
        }
        return 1;
    }
    if(p->state==2) {
        if(p->file_offset>=p->entry.size){p->state=3;goto again;}
        cap=(u16)(cfg->frame_payload-RECORD_HEADER_SIZE-4);
        off=p->file_offset;
        put_u32(body,off);
        n=read_piece(p,body+4,cap);
        if((!n||p->io_error)&&p->file_offset<p->entry.size) {
            printf("Read failed or file changed during transfer: %s\n",p->entry.source);
            return -1;
        }
        f->stream_id=p->entry.file_id;
        f->stream_offset=off;
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_FILE_DATA,
            p->record_id++,p->entry.file_id,body,(u16)(n+4));
        if(!f->payload_len){puts("Internal error: file data record exceeds payload.");return -1;}
        return 1;
    }
    if(p->state==3) {
        if(!source_matches_manifest(p)) {
            printf("File changed during transfer: %s\n",p->entry.source);
            return -1;
        }
        put_u32(body,p->entry.size);
        put_u32(body+4,p->file_crc);
        producer_close(p);
        p->state=1;
        f->stream_id=p->entry.file_id;
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_FILE_END,
            p->record_id++,p->entry.file_id,body,8);
        if(!f->payload_len){puts("File-end record does not fit configured payload.");return -1;}
        return 1;
    }
    if(p->state==5) {
        put_u32(body,p->file_count);
        put_u32(body+4,p->dir_count);
        put_u32(body+8,0);
        put_u32(body+12,p->total_bytes);
        f->payload_len=make_record(f->payload,f->payload_capacity,RT_TRANSFER_END,
            p->record_id++,0,body,16);
        if(!f->payload_len){puts("Transfer-end record does not fit configured payload.");return -1;}
        p->state=6;
        return 1;
    }
    p->finished=1;
    --p->global_index;
    return 0;
}

int producer_fill_window(Producer *p,Window *w,const Config *cfg,u32 session,u32 id) {
    int rc;
    w->count=0;
    w->id=id;
    while(w->count<cfg->window_frames) {
        rc=producer_next(p,&w->frames[w->count],cfg,session);
        if(rc<0)return 0;
        if(!rc)break;
        ++w->count;
    }
    return w->count>0;
}

int producer_reserve_window(Window *w,const Config *cfg) {
    u16 i;
    for(i=0;i<cfg->window_frames;++i)if(w->frames[i].payload_capacity<cfg->frame_payload) {
        u8 far *payload=(u8 far *)_fmalloc(cfg->frame_payload);
        if(!payload) {
            printf("Not enough DOS memory for two %u-frame replay windows; reduce /WINDOW.\n",
                cfg->window_frames);
            return 0;
        }
        if(w->frames[i].payload)_ffree(w->frames[i].payload);
        w->frames[i].payload=payload;
        w->frames[i].payload_capacity=cfg->frame_payload;
    }
    return 1;
}
