#include <conio.h>
#include <bios.h>
#include <i86.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dosfer.h"
#include "qrcodegen.h"
#include "producer.h"
#include "sender_config.h"
#include "timing.h"
#include "vga.h"

#ifdef DOSFER_PROFILE
extern u32 dosferQrProfileTicks[6];
extern u32 dosferProtocolProfileTicks[3];
extern u32 dosferVgaProfileTicks[5];
#endif

#define MANIFEST_NAME "DOSFER.$$$"
#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(DOSFER_QR_VERSION)

static Producer producer;
static Window current_window, previous_window;
static int have_previous;
static u8 qr_codewords[DOSFER_QR_CODEWORDS], qr_workspace[QR_BUFFER+1], raw_frame[DOSFER_MAX_FRAME_BYTES];
static SelectionStats selection;
static u32 completed_session,completed_frames,completed_bytes;
static u32 last_visible_tick;
static int qr_delta_ready;
static int encoded_qr_mask=-1;
static u8 chain_payload[MAX_FRAME_PAYLOAD];
static u8 far *chain_left_codewords,*chain_right_codewords,*chain_cached_raw;
static int chain_cache_valid;
static u32 chain_cache_session,chain_cache_window,chain_cache_global;
static u16 chain_cache_rawlen,chain_cache_index;
static u8 chain_cache_mask;

static void free_chain_cache(void) {
    if(chain_left_codewords)_ffree(chain_left_codewords);
    if(chain_right_codewords)_ffree(chain_right_codewords);
    if(chain_cached_raw)_ffree(chain_cached_raw);
    chain_left_codewords=chain_right_codewords=chain_cached_raw=0;
    chain_cache_valid=0;
}
static void sender_cleanup(void) {
    producer_close(&producer);
    producer_free_window(&current_window);
    producer_free_window(&previous_window);
    free_chain_cache();
    vga_leave();
}

static int ensure_chain_cache(void) {
    if(chain_left_codewords&&chain_right_codewords&&chain_cached_raw)
        return 1;

    chain_left_codewords=(u8 far *)_fmalloc(DOSFER_QR_CODEWORDS);
    chain_right_codewords=(u8 far *)_fmalloc(DOSFER_QR_CODEWORDS);
    chain_cached_raw=(u8 far *)_fmalloc(FRAME_HEADER_SIZE);
    if(chain_left_codewords&&chain_right_codewords&&chain_cached_raw)
        return 1;

    free_chain_cache();
    return 0;
}

static int can_delta(u8 display_mask) {
    return qr_delta_ready&&display_mask==encoded_qr_mask;
}

static int qr_encode_mask(const u8 *data,u16 n,int delta_only,u8 mask) {
    int ok=qrcodegen_dosferEncodeFrameV40L(data,n,qr_codewords,qr_workspace,
        (enum qrcodegen_Mask)mask,delta_only!=0);
    if(ok)encoded_qr_mask=mask;
    return ok;
}

static int qr_encode(const u8 *data,u16 n,const Config *cfg,int delta_only) {
    return qr_encode_mask(data,n,delta_only,cfg->qr_mask);
}

static int make_prepacked_data(const Window *w,u16 i,u32 session,u16 flags,u8 mask) {
    const PendingFrame *f=&w->frames[i];
    u16 n;
    qr_workspace[0]=0x70;qr_workspace[1]=0x34;qr_workspace[2]=0x0B;qr_workspace[3]=0x88;
    n=make_frame(qr_workspace+4,FK_DATA,flags,session,w->id,f->global_index,
        i,w->count,f->stream_id,f->stream_offset,f->payload,f->payload_len);
    if(n!=2952)return 0;
    _fmemcpy(raw_frame,qr_workspace+4,FRAME_HEADER_SIZE);
    if(!qrcodegen_dosferEncodePrepackedV40L(qr_workspace,qr_codewords))return 0;
    encoded_qr_mask=mask;
    return n;
}

static int display_encoded(const Config *cfg,const char *status,int delta,u16 hold_ms) {
    u32 earliest=0;
    if(last_visible_tick&&hold_ms)
        earliest=last_visible_tick+timer_ticks_from_ms(hold_ms);
    if(!vga_show_qr_stream_at(qr_workspace,qr_codewords,cfg->invert,
            status,delta,earliest))return 0;
    qr_delta_ready=vga_delta_ready();
    last_visible_tick=vga_last_flip_tick();
    return 1;
}

static int show_frame(const Window *w,u16 i,const Config *cfg,u32 session,
        int repeated,int waiting,u16 hold_ms,u8 display_mask) {
    const PendingFrame *f=&w->frames[i];
    char status[41];
    u16 n;
    int delta=can_delta(display_mask);
    int rescue=hold_ms!=cfg->hold_ms||display_mask!=cfg->qr_mask;

    if(delta&&!repeated&&chain_cache_valid&&chain_cache_session==session&&
       chain_cache_window==w->id&&chain_cache_global==f->global_index&&
       chain_cache_index==i&&chain_cache_mask==display_mask) {
        n=chain_cache_rawlen;
        _fmemcpy(raw_frame,chain_cached_raw,FRAME_HEADER_SIZE);
        _fmemcpy(qr_codewords,chain_right_codewords,DOSFER_QR_CODEWORDS);
        encoded_qr_mask=display_mask;
        chain_cache_valid=0;
    } else if(delta&&f->payload_len==2904) {
        n=(u16)make_prepacked_data(w,i,session,
            (u16)((repeated?FF_REPEATED:0)|FF_WHITENED),display_mask);
        if(!n)return 0;
    } else {
        n=make_frame(raw_frame,FK_DATA,(repeated?FF_REPEATED:0)|FF_WHITENED,
            session,w->id,f->global_index,i,w->count,f->stream_id,
            f->stream_offset,f->payload,f->payload_len);
        if(!qr_encode_mask(raw_frame,n,delta,display_mask))return 0;
    }

    if(waiting)
        strcpy(status,"READY - focus camera - Enter / Esc");
    else if(rescue)
        sprintf(status,"W%lu F%u/%u RESCUE %ums M%u",
            w->id+1,i+1,w->count,hold_ms,display_mask);
    else
        sprintf(status,"TRANSFER V40L W%lu F%u/%u",
            w->id+1,i+1,w->count);

    return display_encoded(cfg,status,delta,hold_ms);
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
    u16 j,n=left->payload_len>right->payload_len?left->payload_len:right->payload_len;
    u16 rawlen,right_rawlen=0;
    u32 lengths=((u32)left->payload_len<<16)|right->payload_len;
    u32 chain_crc;
    u8 left_header[FRAME_HEADER_SIZE],header_xor[FRAME_HEADER_SIZE];
    char status[41];
    int delta=can_delta(cfg->qr_mask),cached=0,derived=0;

    /* C2 has a V40-L affine shortcut: encode the right DATA frame once,
     * derive the XOR QR from the two canonical codeword streams, then cache
     * the right frame because it is the next DATA frame to display. */
    if(delta&&ensure_chain_cache()) {
        _fmemcpy(left_header,raw_frame,FRAME_HEADER_SIZE);
        _fmemcpy(chain_left_codewords,qr_codewords,DOSFER_QR_CODEWORDS);
        qr_workspace[0]=0x70;qr_workspace[1]=0x34;qr_workspace[2]=0x0B;qr_workspace[3]=0x88;
        right_rawlen=make_frame(qr_workspace+4,FK_DATA,FF_WHITENED,session,w->id,
            right->global_index,i+1,w->count,right->stream_id,right->stream_offset,
            right->payload,right->payload_len);
        _fmemcpy(chain_cached_raw,qr_workspace+4,FRAME_HEADER_SIZE);
        if(right_rawlen==2952&&qrcodegen_dosferEncodePrepackedV40L(qr_workspace,qr_codewords)) {
            encoded_qr_mask=cfg->qr_mask;
            _fmemcpy(chain_right_codewords,qr_codewords,DOSFER_QR_CODEWORDS);
            chain_cache_valid=1;
            chain_cache_session=session;
            chain_cache_window=w->id;
            chain_cache_global=right->global_index;
            chain_cache_index=(u16)(i+1);
            chain_cache_rawlen=right_rawlen;
            chain_cache_mask=cfg->qr_mask;
            cached=1;
        }
    }

    if(cached&&cfg->frame_payload==2904&&left->payload_len==2904&&
       right->payload_len==2904&&right_rawlen==2952) {
        chain_crc=read_u32be(left_header+36)^read_u32be(chain_cached_raw+36)^0xA15AD0F2UL;
        rawlen=make_frame_header_crc(raw_frame,FK_CHAIN_XOR,FF_PAIR_WHITENED,
            session,w->id,left->global_index,i,w->count,lengths,0,chain_crc,n);
        for(j=0;j<FRAME_HEADER_SIZE;++j)
            header_xor[j]=(u8)(left_header[j]^chain_cached_raw[j]^raw_frame[j]);
        derived=qrcodegen_dosferDeriveXorV40L(chain_left_codewords,
            chain_right_codewords,header_xor,qr_codewords);
        if(derived)encoded_qr_mask=cfg->qr_mask;
    }

    if(!derived) {
        n=xor_frame_payload(left,right);
        rawlen=make_frame(raw_frame,FK_CHAIN_XOR,FF_PAIR_WHITENED,session,w->id,
            left->global_index,i,w->count,lengths,0,chain_payload,n);
        if(!qr_encode_mask(raw_frame,rawlen,delta,cfg->qr_mask))return 0;
    }

    sprintf(status,"TRANSFER V40L XOR %u-%u/%u",i+1,i+2,w->count);
    return display_encoded(cfg,status,delta,hold_ms);
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
    char status[41];
    u16 n=xor_block_payload(w,first,count),rawlen;
    int delta=can_delta(cfg->qr_mask);

    rawlen=make_frame(raw_frame,FK_BLOCK_XOR,FF_WHITENED,session,w->id,
        base->global_index,first,w->count,count,0,chain_payload,n);
    if(!qr_encode_mask(raw_frame,rawlen,delta,cfg->qr_mask))return 0;

    sprintf(status,"TRANSFER V40L XOR %u-%u/%u",
        first+1,first+count,w->count);
    return display_encoded(cfg,status,delta,hold_ms);
}

static void flush_keys(void) {
    while(_bios_keybrd(_KEYBRD_READY))
        _bios_keybrd(_KEYBRD_READ);
}

static int decision_key(void) {
    u16 key=_bios_keybrd(_KEYBRD_READ);
    u8 ascii=(u8)key;
    u8 scan=(u8)(key>>8);

    if(scan==0x0D||scan==0x4E)return '+';
    if(scan==0x0C||scan==0x4A)return '-';
    if(scan==0x13)return 'R';
    if(scan==0x32)return 'M';
    if(scan==0x30)return 'B';
    if(scan==0x01)return 27;
    if(scan==0x1C)return 13;
    return ascii;
}

static int transmit(const Window *w,const Config *cfg,u32 session,
        const u8 *selected,u16 chosen,u16 rescue_round) {
    u16 r,i,first,count;
    u16 group=config_redundancy_group(cfg);
    u16 half=(u16)(cfg->chain_width/2);
    u16 hold_ms=cfg->hold_ms;
    u32 adjusted;
    u8 display_mask=cfg->qr_mask;
    int factor=1;

    if(selected&&chosen) {
        if((u32)chosen*16UL<=w->count)factor=4;
        else if((u32)chosen*8UL<=w->count)factor=3;
        else if((u32)chosen*2UL<=w->count)factor=2;

        adjusted=(cfg->hold_ms<100?100UL:cfg->hold_ms)*(u32)factor;
        hold_ms=(u16)(adjusted>60000UL?60000UL:adjusted);
        display_mask=(u8)((cfg->qr_mask+1+(rescue_round?rescue_round-1:0)%7)&7);
    }

    for(r=0;r<cfg->repetitions;++r) {
        first=0;
        count=0;
        for(i=0;i<w->count;++i) {
            if(selected&& !selected[i])continue;
            if(!show_frame(w,i,cfg,session,r>0,0,hold_ms,display_mask))return 0;

            if(!selected&&half&&((i+1)%half)==0) {
                first=(u16)(i+1-half);
                if(first+half<w->count) {
                    count=(u16)(w->count-first);
                    if(count>cfg->chain_width)count=cfg->chain_width;
                    if(cfg->chain_width==2) {
                        if(!show_chain(w,first,cfg,session,hold_ms))return 0;
                    } else if(!show_block_parity(w,first,count,cfg,session,hold_ms)) {
                        return 0;
                    }
                    if(cfg->chain_anchor&&((i+1)%cfg->chain_anchor)==0) {
                        if(!show_frame(w,i,cfg,session,0,0,hold_ms,display_mask))return 0;
                    }
                }
            }

            if(!selected&&group) {
                if(!count)first=i;
                ++count;
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
    char status[41];
    u16 n;
    int delta;
    (void)filtered;

    n=make_frame(raw_frame,FK_END_WINDOW,0,session,w->id,
        w->frames[w->count-1].global_index,0,w->count,0,0,0,0);
    delta=can_delta(cfg->qr_mask);
    if(qr_encode(raw_frame,n,cfg,delta)) {
        sprintf(status,"W%lu DONE  Enter R M B +/- Esc",w->id+1);
        display_encoded(cfg,status,delta,cfg->hold_ms);
    }
    if(cfg->speaker)speaker_beep();
}

static u16 parse_ranges(char *s,u8 *selected,u16 count) {
    char *p=s;
    long a,b,i;
    u16 chosen=0;

    memset(selected,0,MAX_WINDOW);
    while(*p) {
        while(*p==' '||*p==',')++p;
        if(!*p)break;

        a=strtol(p,&p,10);
        b=a;
        if(*p=='-') {
            ++p;
            b=strtol(p,&p,10);
        }
        if(a<1)a=1;
        if(b>count)b=count;
        for(i=a;i<=b;++i) {
            if(!selected[i-1]) {
                selected[i-1]=1;
                ++chosen;
            }
        }
        while(*p&&*p!=',')++p;
    }
    return chosen;
}

static int blank_line(const char *s) {
    while(*s) {
        if(*s!=' '&&*s!='\t'&&*s!='\r'&&*s!='\n')return 0;
        ++s;
    }
    return 1;
}

static void exit_to_dos(u8 status) {
    union REGS r;
    memset(&r,0,sizeof(r));
    r.h.ah=0x4C;
    r.h.al=status;
    int86(0x21,&r,&r);
}

static int enter_transfer_vga(const Config *cfg) {
    if(!vga_enter(cfg->video_mode)) {
        puts("Could not enter VGA 320x200 mode");
        return 0;
    }
    qr_delta_ready=0;
    last_visible_tick=0;
    return 1;
}

static int run_transfer(FILE *mf,Config *cfg) {
    u32 session=(timer_ticks()^(u32)time(NULL)^selection.bytes)|1UL;
    u32 window_id=0;
    int key,have_selection=0;
    u16 chosen=0,rescue_round=0;
    u8 selected[MAX_WINDOW];
    char line[80];

    producer_init(&producer,mf);
    if(!producer_fill_window(&producer,&current_window,cfg,session,window_id))return 0;

    /* Reserve the next window before entering graphics mode. This prevents a
       large /WINDOW setting from failing only after the first batch is sent. */
    if(!producer.finished&&!producer_reserve_window(&previous_window,cfg))return 0;
    if(cfg->chain_width==2)ensure_chain_cache();
    if(!enter_transfer_vga(cfg))return 0;

    if(!show_frame(&current_window,0,cfg,session,0,1,cfg->hold_ms,cfg->qr_mask)) {
        vga_leave();
        puts("Could not build the first V40-L QR; check /PAYLOAD.");
        return 0;
    }

    flush_keys();
    do {
        key=decision_key();
    } while(key!=13&&key!=27);
    if(key==27) {
        vga_leave();
        return 0;
    }

    for(;;) {
        if(!transmit(&current_window,cfg,session,0,0,0)) {
            vga_leave();
            puts("QR frame does not fit fixed V40-L; reduce /PAYLOAD.");
            return 0;
        }

wait_ack:
        /* A window is uninterrupted; controls are accepted only after this
           fresh prompt is visible. */
        flush_keys();
        show_eow(&current_window,cfg,session,have_selection);
        key=decision_key();

        if(key=='r'||key=='R')goto replay;
        if(key=='b'||key=='B') {
            if(have_previous)transmit(&previous_window,cfg,session,0,0,0);
            goto wait_ack;
        }
        if(key=='m'||key=='M') {
            vga_leave();
            printf("Missing frames in window %lu (example 1,3-5; blank = all): ",
                current_window.id+1);
            if(!fgets(line,sizeof(line),stdin))line[0]=0;

            if(blank_line(line)) {
                have_selection=0;
                chosen=0;
                rescue_round=0;
                memset(selected,0,sizeof(selected));
            } else {
                chosen=parse_ranges(line,selected,current_window.count);
                if(!chosen) {
                    puts("No valid frame numbers entered. Press a key.");
                    getch();
                    if(!enter_transfer_vga(cfg))return 0;
                    goto wait_ack;
                }
                have_selection=1;
                rescue_round=1;
            }

            if(!enter_transfer_vga(cfg))return 0;
            transmit(&current_window,cfg,session,
                have_selection?selected:0,chosen,rescue_round);
            goto wait_ack;
        }
        if(key=='+'||key=='=') {
            if(cfg->hold_ms>=50)cfg->hold_ms-=50;
            else cfg->hold_ms=0;
            goto replay;
        }
        if(key=='-') {
            if(cfg->hold_ms<=59950)cfg->hold_ms+=50;
            else cfg->hold_ms=60000;
            goto replay;
        }
        if(key==27) {
            vga_leave();
            puts("PAUSED. C cancels; any other key resumes.");
            key=getch();
            if(key=='c'||key=='C')return 0;
            if(!enter_transfer_vga(cfg))return 0;
            goto wait_ack;
        }
        if(key!=13)goto wait_ack;

        {
            Window swap=previous_window;
            previous_window=current_window;
            current_window=swap;
        }
        have_previous=1;
        have_selection=0;
        chosen=0;
        rescue_round=0;
        ++window_id;
        if(!producer_fill_window(&producer,&current_window,cfg,session,window_id)) {
            vga_leave();
            completed_session=session;
            completed_frames=producer.global_index;
            completed_bytes=producer.total_bytes;
            return 1;
        }
        continue;

replay:
        if(have_selection)++rescue_round;
        transmit(&current_window,cfg,session,
            have_selection?selected:0,chosen,rescue_round);
        goto wait_ack;
    }
}

static int calibration(Config *cfg) {
    u32 session=(timer_ticks()^(u32)time(NULL))|1UL,seq=0;
    u16 key,n,i;
    u8 ascii,scan;
    u8 payload[256];
    char status[41];

    if(!vga_enter(cfg->video_mode))return 0;
    last_visible_tick=0;
    qr_delta_ready=0;
    encoded_qr_mask=-1;
    for(;;) {
        n=cfg->frame_payload;
        if(n>sizeof(payload))n=sizeof(payload);
        memcpy(payload,"CAL1",4);
        put_u32(payload+4,seq);
        put_u16(payload+8,cfg->hold_ms);
        for(i=10;i<n;++i)payload[i]=(u8)(i*31+seq);

        n=make_frame(raw_frame,FK_CALIBRATION,0,session,0,seq,
            (u16)(seq&0xFFFF),100,0,0,payload,n);
        {
            int delta=can_delta(cfg->qr_mask);
            if(!qr_encode(raw_frame,n,cfg,delta)){vga_leave();return 0;}
            sprintf(status,"CAL V40L %s hold %u",
                config_video_name(cfg),cfg->hold_ms);
            if(!display_encoded(cfg,status,delta,cfg->hold_ms)){vga_leave();return 0;}
        }
        ++seq;

        if(!_bios_keybrd(_KEYBRD_READY))continue;
        key=_bios_keybrd(_KEYBRD_READ);
        ascii=(u8)key;
        scan=(u8)(key>>8);
        if(ascii==27||scan==0x01)break;
        if(ascii=='+'||ascii=='='||scan==0x0D||scan==0x4E) {
            if(cfg->hold_ms>=50)cfg->hold_ms-=50;else cfg->hold_ms=0;
        }
        if(ascii=='-'||scan==0x0C||scan==0x4A)cfg->hold_ms+=50;
        if(ascii=='i'||ascii=='I'||scan==0x17)cfg->invert=!cfg->invert;
    }
    vga_leave();
    return 1;
}

static void benchmark(const char *path,Config *cfg) {
    static const u8 whitening_test[16]={
        0x9D,0x3B,0x19,0x23,0xA8,0xAC,0x39,0x89,
        0x3E,0xAD,0x32,0x29,0xF4,0x3D,0x3F,0xEE
    };
    FILE *f;
    u8 *b=producer.disk[0];
    u16 n,rawlen,map_bits=0;
    u32 bytes=0,crc=0,t0,t1,elapsed,protocol_ms,encode_ms;
    u32 build_ms=0,copy_ms=0,text_ms=0,first_ms=0,steady_ms=0;
    u32 useful,dirty_hash=0,full_hash=0;
    int i,encoded=0,ok=1,display_ok=0,redraw_ok=0;

    {u8 record_test[100],record_input[80];u16 exact,refused;
        memset(record_test,0xCC,sizeof(record_test));
        memset(record_input,0x5A,sizeof(record_input));
        exact=make_record(record_test+2,96,RT_SESSION,1,0,record_input,72);
        refused=make_record(record_test+2,96,RT_FILE_BEGIN,2,1,record_input,73);
        printf("Record builder: %s\n",
            exact==96&&!refused&&record_test[0]==0xCC&&record_test[1]==0xCC&&
            record_test[98]==0xCC&&record_test[99]==0xCC?"PASS":"FAIL");
    }

    t0=timer_ticks();timer_wait_ms(100);t1=timer_ticks();
    printf("PIT wait 100 ms: measured %lu ms\n",timer_elapsed_ms(t0,t1));

    f=fopen(path,"rb");
    if(!f){printf("Cannot open %s\n",path);return;}
    t0=timer_ticks();
    while((n=(u16)fread(b,1,sizeof(producer.disk[0]),f))!=0){
        crc=crc32_update(crc,b,n);bytes+=n;
    }
    t1=timer_ticks();fclose(f);
    elapsed=timer_elapsed_ms(t0,t1);
    printf("Disk+CRC: %lu bytes in %lu ms = %lu B/s, CRC %08lX\n",
        bytes,elapsed,elapsed?bytes*1000UL/elapsed:0,crc);

    memset(b,0,16);
    make_frame(raw_frame,FK_DATA,FF_WHITENED,0x6A67C69DUL,0,4,4,32,1,4510,b,16);
    printf("Whitening vector: %s\n",
        memcmp(raw_frame+FRAME_HEADER_SIZE,whitening_test,16)?"FAIL":"PASS");

    memset(b,0xA5,cfg->frame_payload);
    t0=timer_ticks();
    for(i=0;i<25;++i)
        rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,(u32)i,
            0,1,0,0,b,cfg->frame_payload);
    t1=timer_ticks();
    elapsed=timer_elapsed_ms(t0,t1);
    protocol_ms=elapsed/25UL;
    printf("Config: fixed V40-L, payload %u, video %s, mask %u\n",
        cfg->frame_payload,config_video_name(cfg),cfg->qr_mask);
    printf("Protocol x25: %lu ms (%lu ms/frame)\n",elapsed,protocol_ms);

#ifdef DOSFER_PROFILE
    memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
#endif
    t0=timer_ticks();
    for(i=0;i<25;++i)if(qr_encode(raw_frame,rawlen,cfg,0))++encoded;
    t1=timer_ticks();
    if(encoded!=25){printf("QR encode: FAILED (%d/25)\n",encoded);return;}
    elapsed=timer_elapsed_ms(t0,t1);
    encode_ms=elapsed/25UL;
    printf("Full V40 encode x25: %lu ms (%lu ms/frame)\n",elapsed,encode_ms);
#ifdef DOSFER_PROFILE
    printf("  QR profile: pack %lu  ECC %lu  func1 %lu  data %lu  func2 %lu  mask %lu ms\n",
        timer_elapsed_ms(0,dosferQrProfileTicks[0]),timer_elapsed_ms(0,dosferQrProfileTicks[1]),
        timer_elapsed_ms(0,dosferQrProfileTicks[2]),timer_elapsed_ms(0,dosferQrProfileTicks[3]),
        timer_elapsed_ms(0,dosferQrProfileTicks[4]),timer_elapsed_ms(0,dosferQrProfileTicks[5]));
#endif

    if(!vga_enter(cfg->video_mode)){puts("VGA benchmark unavailable.");return;}
    {
        u32 hz100=vga_measure_refresh_hz100(60);
        printf("Measured VGA refresh: %lu.%02lu Hz\n",hz100/100UL,hz100%100UL);
    }
    vga_benchmark_qr(qr_workspace,25,&build_ms,&copy_ms,&text_ms);
    printf("VGA full build x25: %lu ms (%lu ms/frame)\n",build_ms,build_ms/25UL);
    printf("VGA flip/copy x25: %lu ms (%lu ms/frame)\n",copy_ms,copy_ms/25UL);
    printf("VGA status x25: %lu ms (%lu ms/frame)\n",text_ms,text_ms/25UL);

    qr_delta_ready=0;
    rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,0,0,1,0,0,b,cfg->frame_payload);
    t0=timer_ticks();
    if(!qr_encode(raw_frame,rawlen,cfg,0)||
       !vga_show_qr_stream(qr_workspace,qr_codewords,cfg->invert,
            "BENCH first V40 frame",0))ok=0;
    t1=timer_ticks();
    first_ms=timer_elapsed_ms(t0,t1);
    qr_delta_ready=vga_delta_ready();
    vga_delta_stats(&map_bits);

#ifdef DOSFER_PROFILE
    memset(dosferQrProfileTicks,0,sizeof(dosferQrProfileTicks));
    memset(dosferProtocolProfileTicks,0,sizeof(dosferProtocolProfileTicks));
    memset(dosferVgaProfileTicks,0,sizeof(dosferVgaProfileTicks));
#endif
    t0=timer_ticks();
    for(i=1;i<25&&ok;++i){
        rawlen=make_frame(raw_frame,FK_DATA,FF_WHITENED,1,0,(u32)i,0,1,0,0,
            b,cfg->frame_payload);
        if(!qr_encode(raw_frame,rawlen,cfg,qr_delta_ready)||
           !vga_show_qr_stream(qr_workspace,qr_codewords,cfg->invert,
                "BENCH delta V40 frame",qr_delta_ready))ok=0;
        qr_delta_ready=vga_delta_ready();
    }
    t1=timer_ticks();
    steady_ms=timer_elapsed_ms(t0,t1);

    if(ok){
        dirty_hash=vga_screen_hash();
        display_ok=vga_display_matches();
        if(qr_encode(raw_frame,rawlen,cfg,0)&&
           vga_show_qr_stream(qr_workspace,qr_codewords,cfg->invert,
                "BENCH canonical redraw",0)) {
            full_hash=vga_screen_hash();
            redraw_ok=dirty_hash==full_hash;
        }
        printf("Stateful renderer: map %u bits, first %lu ms, next 24 %lu ms = %lu ms/frame\n",
            map_bits,first_ms,steady_ms,steady_ms/24UL);
        printf("Stateful raster: %s (%08lX/%08lX), VGA page: %s\n",
            redraw_ok?"MATCH":"FAIL",dirty_hash,full_hash,display_ok?"MATCH":"FAIL");
        {u32 du,dc,dt;
            vga_benchmark_delta(qr_codewords,24,&du,&dc,&dt);
            printf("Delta microbench x24: update %lu ms, flip %lu ms, status %lu ms\n",du,dc,dt);
        }
#ifdef DOSFER_PROFILE
        printf("  steady protocol: header %lu  payload+CRC %lu  header CRC %lu ms\n",
            timer_elapsed_ms(0,dosferProtocolProfileTicks[0]),timer_elapsed_ms(0,dosferProtocolProfileTicks[1]),
            timer_elapsed_ms(0,dosferProtocolProfileTicks[2]));
        printf("  steady QR: pack %lu  ECC %lu  func1 %lu  data %lu  func2 %lu  mask %lu ms\n",
            timer_elapsed_ms(0,dosferQrProfileTicks[0]),timer_elapsed_ms(0,dosferQrProfileTicks[1]),
            timer_elapsed_ms(0,dosferQrProfileTicks[2]),timer_elapsed_ms(0,dosferQrProfileTicks[3]),
            timer_elapsed_ms(0,dosferQrProfileTicks[4]),timer_elapsed_ms(0,dosferQrProfileTicks[5]));
        printf("  steady VGA: render %lu  upload %lu  retrace %lu  flip %lu  status %lu ms\n",
            timer_elapsed_ms(0,dosferVgaProfileTicks[0]),timer_elapsed_ms(0,dosferVgaProfileTicks[1]),
            timer_elapsed_ms(0,dosferVgaProfileTicks[2]),timer_elapsed_ms(0,dosferVgaProfileTicks[3]),
            timer_elapsed_ms(0,dosferVgaProfileTicks[4]));
#endif
    }
    vga_leave();

    useful=cfg->frame_payload>28?cfg->frame_payload-28:0;
    if(ok&&steady_ms) {
        u32 fps100=2400000UL/steady_ms;
        u32 file_rate=useful*24UL*1000UL/steady_ms;
        printf("Steady V40: %lu.%02lu frames/s, approx %lu file-data B/s\n",
            fps100/100UL,fps100%100UL,file_rate);
    }
}

int main(int argc,char **argv) {
    Config cfg;FILE *mf;int i,rc,action=0,path_count=0;char path[PATH_BYTES],again[8];const char *bench_path=0;u32 est;
    config_defaults(&cfg);
    for(i=1;i<argc;++i) {
        if(!stricmp(argv[i],"/?")||!stricmp(argv[i],"/HELP")||!stricmp(argv[i],"-HELP")){config_print_usage(&cfg);return 0;}
        if(!stricmp(argv[i],"/CAL")||!stricmp(argv[i],"-CAL")){if(action&&action!=1){puts("Choose either /CAL or /BENCH.");return 1;}action=1;continue;}
        if(!stricmp(argv[i],"/BENCH")||!stricmp(argv[i],"-BENCH")){if(action&&action!=2){puts("Choose either /CAL or /BENCH.");return 1;}action=2;continue;}
        if(config_is_split_re(argv[i])) {
            if(i+1>=argc||config_parse_re(&cfg,argv[++i])<0){puts("Invalid /RE value.");return 1;}
            continue;
        }
        if(config_is_split_video(argv[i])) {
            if(i+1>=argc||config_parse_video(&cfg,argv[++i])<0){puts("Invalid /VIDEO value.");return 1;}
            continue;
        }
        rc=config_parse_option(&cfg,argv[i]);if(rc<0){printf("Invalid option value: %s\n",argv[i]);return 1;}if(rc)continue;
        if(argv[i][0]=='/'||argv[i][0]=='-'){printf("Unknown option: %s\n",argv[i]);return 1;}
        if(action==2&&!bench_path)bench_path=argv[i];else path_count++;
    }
    if(!config_validate(&cfg))return 1;
    if(action==1){if(path_count||bench_path){puts("/CAL does not take a file path.");return 1;}return calibration(&cfg)?0:1;}
    if(action==2){if(!bench_path||path_count){puts("Usage: DOSFER /BENCH file [options]");return 1;}benchmark(bench_path,&cfg);free_chain_cache();vga_leave();return 0;}
    mf=fopen(MANIFEST_NAME,"w+b");if(!mf){puts("Cannot create DOSFER.$$$ in current directory.");return 1;}
    if(path_count){for(i=1;i<argc;++i){if(config_is_split_re(argv[i])||config_is_split_video(argv[i])){++i;continue;}
        if(argv[i][0]!='/'&&argv[i][0]!='-')if(!manifest_add_selection(mf,argv[i],&selection)){fclose(mf);remove(MANIFEST_NAME);return 1;}}}
    else do {printf("File or directory: ");if(!fgets(path,sizeof(path),stdin))break;path[strcspn(path,"\r\n")]=0;
        if(!manifest_add_selection(mf,path,&selection)){fclose(mf);remove(MANIFEST_NAME);return 1;}
        printf("Add another? [y/N] ");fgets(again,sizeof(again),stdin);
    } while(again[0]=='y'||again[0]=='Y');
    if(!selection.files&&!selection.dirs){config_print_usage(&cfg);fclose(mf);remove(MANIFEST_NAME);return 1;}
    est=1+selection.dirs+selection.files*2+selection.bytes/(cfg.frame_payload-28)+1;
    printf("Selected: %lu files, %lu directories, %lu bytes, approximately %lu QR frames.\n",selection.files,selection.dirs,selection.bytes,est);
    printf("Settings: V40-L mask %u, video %s, %u ms hold, window %u, repeats %u, redundancy %s.\n",
        cfg.qr_mask,config_video_name(&cfg),cfg.hold_ms,cfg.window_frames,
        cfg.repetitions,config_redundancy_name(&cfg));
    puts("Preparing the first QR code...");
    rc=run_transfer(mf,&cfg);sender_cleanup();fclose(mf);remove(MANIFEST_NAME);
    if(rc){printf("Transfer complete. Session %08lX, %lu frames, %lu bytes. Returning to DOS.\n",
        completed_session,completed_frames,completed_bytes);fflush(stdout);exit_to_dos(0);return 0;}
    return 1;
}
