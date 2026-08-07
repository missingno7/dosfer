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
#define QR_DATA_CODEWORDS 2956
#define QR_ECC_BLOCK_BYTES (25*30)
#if QR_DATA_CODEWORDS + QR_ECC_BLOCK_BYTES > QR_BUFFER
#error V40 QR workspace must hold data plus block-major ECC scratch
#endif

static Producer producer;
static Window current_window;
static u8 qr_codewords[DOSFER_QR_CODEWORDS], qr_workspace[QR_BUFFER+1],
    raw_frame[DOSFER_MAX_FRAME_BYTES];
static SelectionStats selection;
static u32 completed_session,completed_frames,completed_bytes;
static u32 last_visible_tick;
static int qr_delta_ready;
static int encoded_qr_mask=-1;
static u8 chain_payload[MAX_FRAME_PAYLOAD];
static u8 far *rgb_codewords[VGA_RGB_CHANNELS];
static u8 far *rgb_workspace[VGA_RGB_CHANNELS];
static u8 far *rgb_parity_codewords[VGA_RGB_CHANNELS];
static u8 rgb_headers[VGA_RGB_CHANNELS][FRAME_HEADER_SIZE];
static int rgb_delta_ready;
static int rgb_encoded_mask=-1;
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

static void free_rgb_state(void) {
    int channel;
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
        if(rgb_codewords[channel])_ffree(rgb_codewords[channel]);
        if(rgb_workspace[channel])_ffree(rgb_workspace[channel]);
        if(rgb_parity_codewords[channel])_ffree(rgb_parity_codewords[channel]);
        rgb_codewords[channel]=0;
        rgb_workspace[channel]=0;
        rgb_parity_codewords[channel]=0;
    }
    rgb_delta_ready=0;
    rgb_encoded_mask=-1;
}

static int ensure_rgb_state(void) {
    int channel;
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
        if(!rgb_codewords[channel])
            rgb_codewords[channel]=(u8 far *)_fmalloc(DOSFER_QR_CODEWORDS);
        if(!rgb_workspace[channel])
            rgb_workspace[channel]=(u8 far *)_fmalloc(QR_BUFFER+1);
        if(!rgb_parity_codewords[channel])
            rgb_parity_codewords[channel]=(u8 far *)_fmalloc(DOSFER_QR_CODEWORDS);
        if(!rgb_codewords[channel]||!rgb_workspace[channel]||
           !rgb_parity_codewords[channel]) {
            free_rgb_state();
            return 0;
        }
    }
    return 1;
}

static void sender_cleanup(void) {
    producer_close(&producer);
    producer_free_window(&current_window);
    free_chain_cache();
    free_rgb_state();
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

static int qr_prepare_mask(const u8 *data,u16 n,int delta_only,u8 mask) {
    int ok;

    if(delta_only) {
#ifdef DOSFER_PROFILE
        u32 t=timer_ticks(),now;
#endif
        if(!qrcodegen_dosferPackFrameV40L(data,n,qr_workspace))return 0;
#ifdef DOSFER_PROFILE
        now=timer_ticks();dosferQrProfileTicks[0]+=now-t;t=now;
#endif
        qrcodegen_dosferComputeEccBlocksV40L(qr_workspace,
            qr_workspace+QR_DATA_CODEWORDS);
#ifdef DOSFER_PROFILE
        now=timer_ticks();dosferQrProfileTicks[1]+=now-t;
#endif
        ok=vga_apply_v40l_delta(qr_workspace,
            qr_workspace+QR_DATA_CODEWORDS,qr_codewords);
    } else {
        ok=qrcodegen_dosferEncodeFrameV40L(data,n,qr_codewords,qr_workspace,
            (enum qrcodegen_Mask)mask,false);
    }
    if(ok)encoded_qr_mask=mask;
    return ok;
}

static int qr_prepare(const u8 *data,u16 n,const Config *cfg,int delta_only) {
    return qr_prepare_mask(data,n,delta_only,cfg->qr_mask);
}

static int make_prepacked_data(const Window *w,u16 i,u32 session,u16 flags,u8 mask) {
    const PendingFrame *f=&w->frames[i];
    u16 n;
    qr_workspace[0]=0x70;qr_workspace[1]=0x34;qr_workspace[2]=0x0B;qr_workspace[3]=0x88;
    n=make_frame(qr_workspace+4,FK_DATA,flags,session,w->id,f->global_index,
        i,w->count,f->stream_id,f->stream_offset,f->payload,f->payload_len);
    if(n!=2952)return 0;
#ifdef DOSFER_PROFILE
    {u32 t=timer_ticks();
#endif
    qrcodegen_dosferComputeEccBlocksV40L(qr_workspace,
        qr_workspace+QR_DATA_CODEWORDS);
#ifdef DOSFER_PROFILE
    dosferQrProfileTicks[1]+=timer_ticks()-t;}
#endif
    if(!vga_apply_v40l_delta(qr_workspace,
            qr_workspace+QR_DATA_CODEWORDS,qr_codewords))return 0;
    encoded_qr_mask=mask;
    return n;
}

static int display_encoded(const Config *cfg,const char *status,int delta,u16 hold_ms) {
    u32 earliest=0;
    if(last_visible_tick&&hold_ms)
        earliest=last_visible_tick+timer_ticks_from_ms(hold_ms);
    if(delta) {
        if(!vga_show_prepared_at(cfg->invert,status,earliest))return 0;
    } else if(!vga_show_full_qr_at(qr_workspace,qr_codewords,cfg->invert,
            status,earliest))return 0;
    qr_delta_ready=vga_delta_ready();
    last_visible_tick=vga_last_flip_tick();
    return 1;
}

static int show_frame(const Window *w,u16 i,const Config *cfg,u32 session,
        int repeated,u16 hold_ms,u8 display_mask) {
    const PendingFrame *f=&w->frames[i];
    const char *status=0;
#ifdef DOSFER_DEVTOOLS
    char status_buf[41];
#endif
    u16 n;
    int delta=can_delta(display_mask);
#ifdef DOSFER_DEVTOOLS
    int rescue=hold_ms!=cfg->hold_ms||display_mask!=cfg->qr_mask;
#endif

    if(delta&&!repeated&&chain_cache_valid&&chain_cache_session==session&&
       chain_cache_window==w->id&&chain_cache_global==f->global_index&&
       chain_cache_index==i&&chain_cache_mask==display_mask) {
        n=chain_cache_rawlen;
        _fmemcpy(raw_frame,chain_cached_raw,FRAME_HEADER_SIZE);
        if(!vga_apply_codeword_delta(chain_right_codewords,qr_codewords))return 0;
        encoded_qr_mask=display_mask;
        chain_cache_valid=0;
    } else if(delta&&f->payload_len==2904) {
        n=(u16)make_prepacked_data(w,i,session,
            (u16)((repeated?FF_REPEATED:0)|FF_WHITENED),display_mask);
        if(!n)return 0;
        /* The C2 affine shortcut needs the just-displayed DATA header.
         * Other redundancy modes do not, so avoid this copy in the common
         * non-chain path. */
        if(cfg->chain_width==2)
            _fmemcpy(raw_frame,qr_workspace+4,FRAME_HEADER_SIZE);
    } else {
        n=make_frame(raw_frame,FK_DATA,(repeated?FF_REPEATED:0)|FF_WHITENED,
            session,w->id,f->global_index,i,w->count,f->stream_id,
            f->stream_offset,f->payload,f->payload_len);
        if(!qr_prepare_mask(raw_frame,n,delta,display_mask))return 0;
    }

#ifdef DOSFER_DEVTOOLS
    if(rescue) {
        sprintf(status_buf,"W%lu F%u/%u RESCUE %ums M%u",
            w->id+1,i+1,w->count,hold_ms,display_mask);
        status=status_buf;
    } else {
        sprintf(status_buf,"TRANSFER V40L W%lu F%u/%u",
            w->id+1,i+1,w->count);
        status=status_buf;
    }
#endif

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
#ifdef DOSFER_DEVTOOLS
    char status[41];
#endif
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
        if(right_rawlen==2952&&qrcodegen_dosferEncodePrepackedV40L(
                qr_workspace,chain_right_codewords)) {
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
            chain_right_codewords,header_xor,qr_workspace);
        if(derived) {
            derived=vga_apply_codeword_delta(qr_workspace,qr_codewords);
            if(derived)encoded_qr_mask=cfg->qr_mask;
        }
    }

    if(!derived) {
        n=xor_frame_payload(left,right);
        rawlen=make_frame(raw_frame,FK_CHAIN_XOR,FF_PAIR_WHITENED,session,w->id,
            left->global_index,i,w->count,lengths,0,chain_payload,n);
        if(!qr_prepare_mask(raw_frame,rawlen,delta,cfg->qr_mask))return 0;
    }

#ifdef DOSFER_DEVTOOLS
    sprintf(status,"TRANSFER V40L XOR %u-%u/%u",i+1,i+2,w->count);
    return display_encoded(cfg,status,delta,hold_ms);
#else
    return display_encoded(cfg,0,delta,hold_ms);
#endif
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
#ifdef DOSFER_DEVTOOLS
    char status[41];
#endif
    u16 n=xor_block_payload(w,first,count),rawlen;
    int delta=can_delta(cfg->qr_mask);

    rawlen=make_frame(raw_frame,FK_BLOCK_XOR,FF_WHITENED,session,w->id,
        base->global_index,first,w->count,count,0,chain_payload,n);
    if(!qr_prepare_mask(raw_frame,rawlen,delta,cfg->qr_mask))return 0;

#ifdef DOSFER_DEVTOOLS
    sprintf(status,"TRANSFER V40L XOR %u-%u/%u",
        first+1,first+count,w->count);
    return display_encoded(cfg,status,delta,hold_ms);
#else
    return display_encoded(cfg,0,delta,hold_ms);
#endif
}


enum { RGB_ITEM_DATA=1, RGB_ITEM_CHAIN=2, RGB_ITEM_BLOCK=3, RGB_ITEM_EOW=4 };
typedef struct {
    u8 kind;
    u8 repeated;
    u8 group_xor;
    u16 first;
    u16 count;
    u16 stride;
} RgbItem;

typedef struct {
    u16 batches;
    u16 first[VGA_RGB_CHANNELS];
    u16 count[VGA_RGB_CHANNELS];
    u8 header_xor[VGA_RGB_CHANNELS][FRAME_HEADER_SIZE];
} RgbParityAccumulator;

static int rgb_item_same(const RgbItem *a,const RgbItem *b) {
    return a->kind==b->kind&&a->repeated==b->repeated&&
        a->group_xor==b->group_xor&&a->first==b->first&&
        a->count==b->count&&a->stride==b->stride;
}

static u16 xor_strided_payload(const Window *w,u16 first,u16 count,u16 stride) {
    u16 i,j,k,words,n=0;
    if(!stride)stride=1;
    for(i=0;i<count;++i) {
        const PendingFrame *f=&w->frames[first+i*stride];
        if(f->payload_len>n)n=f->payload_len;
    }
    memset(chain_payload,0,n);
    for(i=0;i<count;++i) {
        const PendingFrame *f=&w->frames[first+i*stride];
        words=(u16)(f->payload_len>>2);
        for(k=0;k<words;++k)
            ((u32 *)chain_payload)[k]^=((const u32 far *)f->payload)[k];
        j=(u16)(words<<2);
        for(;j<f->payload_len;++j)chain_payload[j]^=f->payload[j];
    }
    return n;
}

static u16 make_rgb_item_frame(const Window *w,const RgbItem *item,u32 session) {
    const PendingFrame *base;
    u16 n,flags,stride;
    u32 lengths;

    if(!w||!item||item->first>=w->count)return 0;
    base=&w->frames[item->first];
    if(item->kind==RGB_ITEM_DATA) {
        return make_frame(raw_frame,FK_DATA,
            (u16)(FF_WHITENED|(item->repeated?FF_REPEATED:0)),
            session,w->id,base->global_index,item->first,w->count,
            base->stream_id,base->stream_offset,base->payload,base->payload_len);
    }
    if(item->kind==RGB_ITEM_CHAIN) {
        const PendingFrame *right;
        if(item->first+1>=w->count)return 0;
        right=&w->frames[item->first+1];
        n=xor_frame_payload(base,right);
        lengths=((u32)base->payload_len<<16)|right->payload_len;
        return make_frame(raw_frame,FK_CHAIN_XOR,FF_PAIR_WHITENED,
            session,w->id,base->global_index,item->first,w->count,
            lengths,0,chain_payload,n);
    }
    if(item->kind==RGB_ITEM_BLOCK) {
        stride=item->stride?item->stride:1;
        if(!item->count||item->first+(item->count-1)*stride>=w->count)return 0;
        n=xor_strided_payload(w,item->first,item->count,stride);
        flags=item->group_xor?FF_GROUP_XOR_WHITENED:FF_WHITENED;
        return make_frame(raw_frame,FK_BLOCK_XOR,flags,session,w->id,
            base->global_index,item->first,w->count,item->count,
            item->group_xor?stride:0,chain_payload,n);
    }
    if(item->kind==RGB_ITEM_EOW) {
        return make_frame(raw_frame,FK_END_WINDOW,0,session,w->id,
            w->frames[w->count-1].global_index,0,w->count,0,0,0,0);
    }
    return 0;
}

static int rgb_prepare_items(const Window *w,const RgbItem items[VGA_RGB_CHANNELS],
        const Config *cfg,u32 session,u8 display_mask,int delta) {
    const u8 *data[VGA_RGB_CHANNELS],*ecc[VGA_RGB_CHANNELS];
    u8 *current[VGA_RGB_CHANNELS];
    int channel,source;
    u16 rawlen;

    (void)cfg;
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
        source=-1;
        if(channel>0&&rgb_item_same(&items[channel],&items[0]))source=0;
        else if(channel>1&&rgb_item_same(&items[channel],&items[1]))source=1;
        if(source>=0) {
            _fmemcpy(rgb_headers[channel],rgb_headers[source],FRAME_HEADER_SIZE);
            _fmemcpy(rgb_workspace[channel],rgb_workspace[source],QR_BUFFER+1);
            if(!delta)
                _fmemcpy(rgb_codewords[channel],rgb_codewords[source],DOSFER_QR_CODEWORDS);
            continue;
        }
        rawlen=make_rgb_item_frame(w,&items[channel],session);
        if(!rawlen)return 0;
        _fmemcpy(rgb_headers[channel],raw_frame,FRAME_HEADER_SIZE);
        if(delta) {
            if(!qrcodegen_dosferPackFrameV40L(raw_frame,rawlen,
                    rgb_workspace[channel]))return 0;
            qrcodegen_dosferComputeEccBlocksV40L(rgb_workspace[channel],
                rgb_workspace[channel]+QR_DATA_CODEWORDS);
        } else if(!qrcodegen_dosferEncodeFrameV40L(raw_frame,rawlen,
                rgb_codewords[channel],rgb_workspace[channel],
                (enum qrcodegen_Mask)display_mask,false))return 0;
    }
    if(delta) {
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
            data[channel]=rgb_workspace[channel];
            ecc[channel]=rgb_workspace[channel]+QR_DATA_CODEWORDS;
            current[channel]=rgb_codewords[channel];
        }
        if(!vga_apply_v40l_delta3(data,ecc,current))return 0;
    }
    rgb_encoded_mask=display_mask;
    return 1;
}

static int display_rgb_items(const Window *w,const RgbItem items[VGA_RGB_CHANNELS],
        const Config *cfg,u32 session,u16 hold_ms,u8 display_mask,
        const char *status) {
    const u8 *qr[VGA_RGB_CHANNELS];
    u8 *current[VGA_RGB_CHANNELS];
    u32 earliest=0;
    int channel;
    int delta=rgb_delta_ready&&display_mask==rgb_encoded_mask;

    if(!rgb_prepare_items(w,items,cfg,session,display_mask,delta))return 0;
    if(last_visible_tick&&hold_ms)
        earliest=last_visible_tick+timer_ticks_from_ms(hold_ms);
    if(delta) {
        if(!vga_show_prepared3_at(cfg->invert,status,earliest))return 0;
    } else {
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
            qr[channel]=rgb_workspace[channel];
            current[channel]=rgb_codewords[channel];
        }
        if(!vga_show_full_qr3_at(qr,current,cfg->invert,status,earliest))return 0;
    }
    rgb_delta_ready=vga_delta_ready();
    last_visible_tick=vga_last_flip_tick();
    return 1;
}

static int display_rgb_codewords(const Config *cfg,u8 *const next[VGA_RGB_CHANNELS],
        u16 hold_ms,const char *status) {
    u8 *current[VGA_RGB_CHANNELS];
    const u8 *source[VGA_RGB_CHANNELS];
    u32 earliest=0;
    int channel;

    if(!rgb_delta_ready)return 0;
    for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
        source[channel]=next[channel];
        current[channel]=rgb_codewords[channel];
    }
    if(!vga_apply_codeword_delta3(source,current))return 0;
    if(last_visible_tick&&hold_ms)
        earliest=last_visible_tick+timer_ticks_from_ms(hold_ms);
    if(!vga_show_prepared3_at(cfg->invert,status,earliest))return 0;
    last_visible_tick=vga_last_flip_tick();
    return 1;
}

static void xor_codeword_stream(u8 far *dest,const u8 far *source) {
    u16 i;
    for(i=0;i+4<=DOSFER_QR_CODEWORDS;i+=4)
        *(u32 far *)(dest+i)^=*(const u32 far *)(source+i);
    for(;i<DOSFER_QR_CODEWORDS;++i)dest[i]^=source[i];
}

static void reset_rgb_parity_accumulator(RgbParityAccumulator *acc) {
    memset(acc,0,sizeof(*acc));
}

static int accumulate_rgb_data(const RgbItem batch[VGA_RGB_CHANNELS],u16 actual,
        RgbParityAccumulator *acc) {
    u16 channel,i;
    for(channel=0;channel<actual;++channel) {
        if(acc->count[channel]) {
            if(batch[channel].first!=acc->first[channel]+acc->count[channel]*3)
                return 0;
            xor_codeword_stream(rgb_parity_codewords[channel],rgb_codewords[channel]);
            for(i=0;i<FRAME_HEADER_SIZE;++i)
                acc->header_xor[channel][i]^=rgb_headers[channel][i];
        } else {
            acc->first[channel]=batch[channel].first;
            _fmemcpy(rgb_parity_codewords[channel],rgb_codewords[channel],
                DOSFER_QR_CODEWORDS);
            memcpy(acc->header_xor[channel],rgb_headers[channel],FRAME_HEADER_SIZE);
        }
        ++acc->count[channel];
    }
    ++acc->batches;
    return 1;
}

static int rgb_parity_same_length(const Window *w,u16 first,u16 count,u16 stride) {
    u16 i,length=w->frames[first].payload_len;
    for(i=1;i<count;++i)
        if(w->frames[first+i*stride].payload_len!=length)return 0;
    return 1;
}

static int finalize_rgb_parity_channel(const Window *w,u16 channel,
        const Config *cfg,u32 session,const RgbParityAccumulator *acc) {
    const PendingFrame *base;
    u8 header_correction[FRAME_HEADER_SIZE];
    u16 i,n,rawlen,count=acc->count[channel],first=acc->first[channel];
    int affine;

    if(!count)return 0;
    base=&w->frames[first];
    n=xor_strided_payload(w,first,count,3);
    rawlen=make_frame(raw_frame,FK_BLOCK_XOR,FF_GROUP_XOR_WHITENED,
        session,w->id,base->global_index,first,w->count,count,3,
        chain_payload,n);
    if(!rawlen)return 0;
    affine=rgb_parity_same_length(w,first,count,3)&&
        (count!=2||rawlen==2952);
    if(affine) {
        for(i=0;i<FRAME_HEADER_SIZE;++i)
            header_correction[i]=(u8)(acc->header_xor[channel][i]^raw_frame[i]);
        if(qrcodegen_dosferCorrectXorV40L(rgb_parity_codewords[channel],count,
                header_correction,rgb_parity_codewords[channel]))return 1;
    }
    return qrcodegen_dosferEncodeFrameV40L(raw_frame,rawlen,
        rgb_parity_codewords[channel],rgb_workspace[channel],
        (enum qrcodegen_Mask)cfg->qr_mask,true);
}

static int flush_rgb_channel_parity(const Window *w,const Config *cfg,u32 session,
        u16 hold_ms,u8 display_mask,RgbParityAccumulator *acc) {
    RgbItem items[VGA_RGB_CHANNELS];
    u8 *next[VGA_RGB_CHANNELS];
    int channel,last=-1;
#ifdef DOSFER_DEVTOOLS
    char status[41];
    sprintf(status,"RGB3 PARITY stride3 x%u",acc->batches);
#endif
    if(!acc->batches)return 1;
    if(rgb_delta_ready&&display_mask==rgb_encoded_mask) {
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)if(acc->count[channel]) {
            if(!finalize_rgb_parity_channel(w,(u16)channel,cfg,session,acc))return 0;
            next[channel]=rgb_parity_codewords[channel];last=channel;
        }
        if(last<0)return 0;
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            if(!acc->count[channel])next[channel]=next[last];
#ifdef DOSFER_DEVTOOLS
        if(!display_rgb_codewords(cfg,next,hold_ms,status))return 0;
#else
        if(!display_rgb_codewords(cfg,next,hold_ms,0))return 0;
#endif
    } else {
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)if(acc->count[channel]) {
            items[channel].kind=RGB_ITEM_BLOCK;items[channel].repeated=0;
            items[channel].group_xor=1;items[channel].first=acc->first[channel];
            items[channel].count=acc->count[channel];items[channel].stride=3;
            last=channel;
        }
        if(last<0)return 0;
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel)
            if(!acc->count[channel])items[channel]=items[last];
#ifdef DOSFER_DEVTOOLS
        if(!display_rgb_items(w,items,cfg,session,hold_ms,display_mask,status))return 0;
#else
        if(!display_rgb_items(w,items,cfg,session,hold_ms,display_mask,0))return 0;
#endif
    }
    reset_rgb_parity_accumulator(acc);
    return 1;
}

static int show_rgb_batch(const Window *w,RgbItem batch[VGA_RGB_CHANNELS],u16 used,
        const Config *cfg,u32 session,u16 hold_ms,u8 display_mask,
        const char *status) {
    u16 channel;
    if(!used)return 1;
    for(channel=used;channel<VGA_RGB_CHANNELS;++channel)
        batch[channel]=batch[used-1];
    return display_rgb_items(w,batch,cfg,session,hold_ms,display_mask,status);
}

static int transmit_rgb3(const Window *w,const Config *cfg,u32 session,
        const u8 *selected,u16 chosen,u16 rescue_round,u16 first_data) {
    RgbItem batch[VGA_RGB_CHANNELS];
    RgbParityAccumulator parity;
    u16 r,i,used,first,count,group=config_redundancy_group(cfg);
    u16 half=(u16)(cfg->chain_width/2);
    u16 hold_ms=cfg->hold_ms;
    u32 adjusted;
    u8 display_mask=cfg->qr_mask;
    int factor=1;
#ifdef DOSFER_DEVTOOLS
    char status[41];
#endif

    if(selected&&chosen) {
        if((u32)chosen*16UL<=w->count)factor=4;
        else if((u32)chosen*8UL<=w->count)factor=3;
        else if((u32)chosen*2UL<=w->count)factor=2;
        adjusted=(cfg->hold_ms<100?100UL:cfg->hold_ms)*(u32)factor;
        hold_ms=(u16)(adjusted>60000UL?60000UL:adjusted);
        display_mask=(u8)((cfg->qr_mask+1+(rescue_round?rescue_round-1:0)%7)&7);
    }

    for(r=0;r<cfg->repetitions;++r) {
        used=0;reset_rgb_parity_accumulator(&parity);
        for(i=r==0?first_data:0;i<w->count;++i) {
            if(selected&&!selected[i])continue;
            batch[used].kind=RGB_ITEM_DATA;
            batch[used].repeated=(u8)(r>0);
            batch[used].group_xor=0;
            batch[used].first=i;
            batch[used].count=1;
            batch[used].stride=1;
            ++used;
            if(used==VGA_RGB_CHANNELS) {
#ifdef DOSFER_DEVTOOLS
                sprintf(status,"RGB3 DATA %u-%u/%u",batch[0].first+1,
                    batch[2].first+1,w->count);
                if(!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,status))return 0;
#else
                if(!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,0))return 0;
#endif
                if(!selected&&!cfg->chain_width&&group==3) {
                    if(!accumulate_rgb_data(batch,used,&parity))return 0;
                    if(parity.batches==3&&!flush_rgb_channel_parity(w,cfg,session,
                            hold_ms,display_mask,&parity))return 0;
                }
                used=0;
            }
        }
        if(used) {
            u16 actual=used;
#ifdef DOSFER_DEVTOOLS
            sprintf(status,"RGB3 DATA tail %u/%u",batch[0].first+1,w->count);
            if(!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,status))return 0;
#else
            if(!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,0))return 0;
#endif
            if(!selected&&!cfg->chain_width&&group==3) {
                if(!accumulate_rgb_data(batch,actual,&parity))return 0;
            }
        }
        if(selected)continue;
        if(!cfg->chain_width&&group==3) {
            if(!flush_rgb_channel_parity(w,cfg,session,hold_ms,display_mask,&parity))return 0;
            continue;
        }

        /* Optional anchors remain DATA frames and are therefore batched
         * separately from all parity symbols. */
        if(cfg->chain_width&&cfg->chain_anchor) {
            used=0;
            for(i=(u16)(cfg->chain_anchor-1);i<w->count;i=(u16)(i+cfg->chain_anchor)) {
                batch[used].kind=RGB_ITEM_DATA;batch[used].repeated=1;
                batch[used].group_xor=0;batch[used].first=i;
                batch[used].count=1;batch[used].stride=1;++used;
                if(used==VGA_RGB_CHANNELS) {
                    if(!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,0))return 0;
                    used=0;
                }
            }
            if(used&&!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,0))return 0;
        }

        /* Parity is a separate homogeneous RGB phase: never DATA/DATA/PARITY. */
        used=0;
        if(cfg->chain_width) {
            for(i=(u16)(half-1);i<w->count;i=(u16)(i+half)) {
                first=(u16)(i+1-half);
                if(first+half>=w->count)continue;
                count=(u16)(w->count-first);
                if(count>cfg->chain_width)count=cfg->chain_width;
                batch[used].kind=cfg->chain_width==2?RGB_ITEM_CHAIN:RGB_ITEM_BLOCK;
                batch[used].repeated=0;batch[used].group_xor=0;
                batch[used].first=first;batch[used].count=count;
                batch[used].stride=1;++used;
                if(used==VGA_RGB_CHANNELS) {
                    if(!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,0))return 0;
                    used=0;
                }
            }
        } else if(group) {
            for(first=0;first<w->count;first=(u16)(first+group)) {
                count=(u16)(w->count-first);if(count>group)count=group;
                batch[used].kind=RGB_ITEM_BLOCK;batch[used].repeated=0;
                batch[used].group_xor=0;batch[used].first=first;
                batch[used].count=count;batch[used].stride=1;++used;
                if(used==VGA_RGB_CHANNELS) {
                    if(!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,0))return 0;
                    used=0;
                }
            }
        }
        if(used&&!show_rgb_batch(w,batch,used,cfg,session,hold_ms,display_mask,0))return 0;
    }
    return 1;
}
static void flush_keys(void) {
    while(_bios_keybrd(_KEYBRD_READY))
        _bios_keybrd(_KEYBRD_READ);
}

static int decision_key(void) {
    u16 key=_bios_keybrd(_KEYBRD_READ);
    u8 ascii=(u8)key;
    u8 scan=(u8)(key>>8);

    if(scan==0x13)return 'R';
    if(scan==0x32)return 'M';
    if(scan==0x01)return 27;
    if(scan==0x1C)return 13;
    return ascii;
}

static int transmit(const Window *w,const Config *cfg,u32 session,
        const u8 *selected,u16 chosen,u16 rescue_round,u16 first_data) {
    u16 r,i,first,count;
    if(cfg->rgb3)return transmit_rgb3(w,cfg,session,selected,chosen,rescue_round,first_data);
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
            if(!show_frame(w,i,cfg,session,r>0,hold_ms,display_mask))return 0;

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
                        if(!show_frame(w,i,cfg,session,0,hold_ms,display_mask))return 0;
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

static void show_eow(const Window *w,const Config *cfg,u32 session) {
    char status[41];
    u16 n;
    int delta;
    sprintf(status,"W%lu DONE  Enter R M Esc",w->id+1);
    if(cfg->rgb3) {
        RgbItem items[VGA_RGB_CHANNELS];
        int channel;
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
            items[channel].kind=RGB_ITEM_EOW;items[channel].repeated=0;
            items[channel].group_xor=0;items[channel].first=0;
            items[channel].count=0;items[channel].stride=1;
        }
        display_rgb_items(w,items,cfg,session,cfg->hold_ms,cfg->qr_mask,status);
    } else {
        n=make_frame(raw_frame,FK_END_WINDOW,0,session,w->id,
            w->frames[w->count-1].global_index,0,w->count,0,0,0,0);
        delta=can_delta(cfg->qr_mask);
        if(qr_prepare(raw_frame,n,cfg,delta))
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
    if(cfg->rgb3&&!ensure_rgb_state()) {
        puts("Not enough DOS memory for RGB3 state; use /BW.");
        return 0;
    }
    if(!vga_enter(cfg->video_mode,cfg->rgb3)) {
        puts("Could not enter planar EGA/VGA 320x200 mode");
        return 0;
    }
    qr_delta_ready=0;
    encoded_qr_mask=-1;
    rgb_delta_ready=0;
    rgb_encoded_mask=-1;
    last_visible_tick=0;
    return 1;
}

static int run_transfer(FILE *mf,const Config *cfg) {
    u32 session=(timer_ticks()^(u32)time(NULL)^selection.bytes)|1UL;
    u32 window_id=0;
    int key,have_selection=0;
    u16 chosen=0,rescue_round=0;
    /* The initial RGB image is ordinary DATA, not an out-of-band control
     * symbol. It contains logical frames 1/2/3; the streamed schedule then
     * resumes at frame 3 in red (zero-based index 2), giving R3/G4/B5. */
    u16 first_data=0;
    u8 selected[MAX_WINDOW];
    char line[80];

    producer_init(&producer,mf);
    if(!producer_fill_window(&producer,&current_window,cfg,session,window_id))return 0;

    /* Only the current acknowledged window is retained. Its payload buffers
       are recycled in place after Enter commits the window, allowing the
       default 66-frame window without paying for a second replay window. */
    if(cfg->chain_width==2)ensure_chain_cache();
    if(!enter_transfer_vga(cfg))return 0;
    if(cfg->rgb3) {
        RgbItem initial_data[VGA_RGB_CHANNELS];
        int channel;
        for(channel=0;channel<VGA_RGB_CHANNELS;++channel) {
            initial_data[channel].kind=RGB_ITEM_DATA;
            initial_data[channel].repeated=0;
            initial_data[channel].group_xor=0;
            initial_data[channel].first=(u16)channel;
            initial_data[channel].count=1;
            initial_data[channel].stride=1;
        }
        if(!display_rgb_items(&current_window,initial_data,cfg,session,
                cfg->hold_ms,cfg->qr_mask,"DATA 1-3 - Enter / Esc")) {
            vga_leave();
            puts("Could not build the first RGB3 V40-L QR; check /PAYLOAD or use /BW.");
            return 0;
        }
    } else if(!show_frame(&current_window,0,cfg,session,0,cfg->hold_ms,cfg->qr_mask)) {
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
    if(cfg->rgb3)first_data=(u16)(VGA_RGB_CHANNELS-1);

    for(;;) {
        if(!transmit(&current_window,cfg,session,0,0,0,first_data)) {
            vga_leave();
            puts("QR frame does not fit fixed V40-L; reduce /PAYLOAD.");
            return 0;
        }
        first_data=0;

wait_ack:
        /* A window is uninterrupted; controls are accepted only after this
           fresh prompt is visible. */
        flush_keys();
        show_eow(&current_window,cfg,session);
        key=decision_key();

        if(key=='r'||key=='R')goto replay;
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
                have_selection?selected:0,chosen,rescue_round,0);
            goto wait_ack;
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

        /* Enter commits the current window. Keep its END_WINDOW QR visible
           while producer_fill_window() recycles the same payload buffers for
           the next window; no previous-window copy is retained. */
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
            have_selection?selected:0,chosen,rescue_round,0);
        goto wait_ack;
    }
}

#ifdef DOSFER_DEVTOOLS
static int calibration(Config *cfg) {
    u32 session=(timer_ticks()^(u32)time(NULL))|1UL,seq=0;
    u16 key,n,i;
    u8 ascii,scan;
    u8 payload[256];
    char status[41];

    if(!vga_enter(cfg->video_mode,0))return 0;
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
            if(!qr_prepare(raw_frame,n,cfg,delta)){vga_leave();return 0;}
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
    for(i=0;i<25;++i)if(qr_prepare(raw_frame,rawlen,cfg,0))++encoded;
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

    if(!vga_enter(cfg->video_mode,0)){puts("VGA benchmark unavailable.");return;}
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
    if(!qr_prepare(raw_frame,rawlen,cfg,0)||
       !vga_show_full_qr_at(qr_workspace,qr_codewords,cfg->invert,
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
        if(!qr_prepare(raw_frame,rawlen,cfg,qr_delta_ready)||
           !vga_show_prepared_at(cfg->invert,"BENCH delta V40 frame",0))ok=0;
        qr_delta_ready=vga_delta_ready();
    }
    t1=timer_ticks();
    steady_ms=timer_elapsed_ms(t0,t1);

    if(ok){
        dirty_hash=vga_screen_hash();
        display_ok=vga_display_matches();
        if(qr_prepare(raw_frame,rawlen,cfg,0)&&
           vga_show_full_qr_at(qr_workspace,qr_codewords,cfg->invert,
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
#endif /* DOSFER_DEVTOOLS */

int main(int argc,char **argv) {
    Config cfg;
    FILE *mf;
    int i,rc,path_count=0;
    char path[PATH_BYTES],again[8];
    u32 est;
#ifdef DOSFER_DEVTOOLS
    int action=0;
    const char *bench_path=0;
#endif

    config_defaults(&cfg);

    for(i=1;i<argc;++i) {
        if(!stricmp(argv[i],"/?")||!stricmp(argv[i],"/HELP")||!stricmp(argv[i],"-HELP")){
            config_print_usage(&cfg);
            return 0;
        }
#ifdef DOSFER_DEVTOOLS
        if(!stricmp(argv[i],"/CAL")||!stricmp(argv[i],"-CAL")){
            if(action&&action!=1){puts("Choose either /CAL or /BENCH.");return 1;}
            action=1;
            continue;
        }
        if(!stricmp(argv[i],"/BENCH")||!stricmp(argv[i],"-BENCH")){
            if(action&&action!=2){puts("Choose either /CAL or /BENCH.");return 1;}
            action=2;
            continue;
        }
#endif
        if(config_is_split_re(argv[i])) {
            if(i+1>=argc||config_parse_re(&cfg,argv[++i])<0){
                puts("Invalid /RE value.");
                return 1;
            }
            continue;
        }
        if(config_is_split_video(argv[i])) {
            if(i+1>=argc||config_parse_video(&cfg,argv[++i])<0){
                puts("Invalid /VIDEO value.");
                return 1;
            }
            continue;
        }

        rc=config_parse_option(&cfg,argv[i]);
        if(rc<0){printf("Invalid option value: %s\n",argv[i]);return 1;}
        if(rc)continue;

        if(argv[i][0]=='/'||argv[i][0]=='-'){
            printf("Unknown option: %s\n",argv[i]);
            return 1;
        }

#ifdef DOSFER_DEVTOOLS
        if(action==2&&!bench_path)bench_path=argv[i];
        else
#endif
            ++path_count;
    }

    if(!config_validate(&cfg))return 1;

#ifdef DOSFER_DEVTOOLS
    if(action==1){
        if(path_count||bench_path){puts("/CAL does not take a file path.");return 1;}
        return calibration(&cfg)?0:1;
    }
    if(action==2){
        if(!bench_path||path_count){puts("Usage: DOSFER /BENCH file [options]");return 1;}
        benchmark(bench_path,&cfg);
        free_chain_cache();
        free_rgb_state();
        vga_leave();
        return 0;
    }
#endif

    mf=fopen(MANIFEST_NAME,"w+b");
    if(!mf){
        puts("Cannot create DOSFER.$$$ in current directory.");
        return 1;
    }

    if(path_count){
        for(i=1;i<argc;++i){
            if(config_is_split_re(argv[i])||config_is_split_video(argv[i])){
                ++i;
                continue;
            }
            if(argv[i][0]!='/'&&argv[i][0]!='-'){
                if(!manifest_add_selection(mf,argv[i],&selection)){
                    fclose(mf);
                    remove(MANIFEST_NAME);
                    return 1;
                }
            }
        }
    } else {
        do {
            printf("File or directory: ");
            if(!fgets(path,sizeof(path),stdin))break;
            path[strcspn(path,"\r\n")]=0;
            if(!manifest_add_selection(mf,path,&selection)){
                fclose(mf);
                remove(MANIFEST_NAME);
                return 1;
            }
            printf("Add another? [y/N] ");
            fgets(again,sizeof(again),stdin);
        } while(again[0]=='y'||again[0]=='Y');
    }

    if(!selection.files&&!selection.dirs){
        config_print_usage(&cfg);
        fclose(mf);
        remove(MANIFEST_NAME);
        return 1;
    }

    est=1+selection.dirs+selection.files*2+selection.bytes/(cfg.frame_payload-28)+1;
    printf("Selected: %lu files, %lu directories, %lu bytes, approximately %lu QR frames.\n",
        selection.files,selection.dirs,selection.bytes,est);
    printf("Settings: %s V40-L mask %u, video %s, %u ms physical hold, window %u, repeats %u, redundancy %s.\n",
        cfg.rgb3?"RGB3":"BW",cfg.qr_mask,config_video_name(&cfg),cfg.hold_ms,
        cfg.window_frames,cfg.repetitions,config_redundancy_name(&cfg));
    puts("Preparing the first QR code...");

    rc=run_transfer(mf,&cfg);
    sender_cleanup();
    fclose(mf);
    remove(MANIFEST_NAME);

    if(rc){
        printf("Transfer complete. Session %08lX, %lu frames, %lu bytes. Returning to DOS.\n",
            completed_session,completed_frames,completed_bytes);
        fflush(stdout);
        exit_to_dos(0);
        return 0;
    }
    return 1;
}
