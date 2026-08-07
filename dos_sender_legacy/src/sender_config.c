#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sender_config.h"

static const char *option_value(const char *arg,const char *name) {
    size_t n=strlen(name);
    const char *p;
    if(arg[0]!='/'&&arg[0]!='-')return 0;
    p=arg+1;
    if(strnicmp(p,name,n)||(p[n]!=':'&&p[n]!='='))return 0;
    return p+n+1;
}

static int option_number(const char *arg,const char *a,const char *b,
        long lo,long hi,long *out) {
    const char *p=option_value(arg,a);
    char *end;
    long v;
    if(!p&&b)p=option_value(arg,b);
    if(!p)return 0;
    v=strtol(p,&end,10);
    if(!*p||*end||v<lo||v>hi)return -1;
    *out=v;
    return 1;
}

void config_defaults(Config *c) {
    memset(c,0,sizeof(*c));
    c->repetitions=1;
    c->frame_payload=2904;
    c->hold_ms=0;
    c->window_frames=64;
    c->speaker=1;
    c->redundancy=7;
    c->qr_mask=0;
    c->chain_anchor=0;
    c->video_mode=VIDEO_320_60;
}

int config_validate(const Config *cfg) {
    if(!cfg||
       cfg->frame_payload<96||cfg->frame_payload>MAX_FRAME_PAYLOAD||
       cfg->window_frames<4||cfg->window_frames>MAX_WINDOW||
       cfg->repetitions<1||cfg->repetitions>20||
       cfg->redundancy>MAX_WINDOW||cfg->qr_mask>7||
       cfg->chain_anchor>MAX_WINDOW||
       (cfg->chain_width&&(cfg->chain_width<2||cfg->chain_width>MAX_WINDOW||
        (cfg->chain_width&1)||cfg->chain_width/2>=cfg->window_frames))||
       (cfg->video_mode!=VIDEO_320_60&&cfg->video_mode!=VIDEO_320_70)) {
        puts("Invalid sender configuration values.");
        return 0;
    }

    /* V40-L + ECI 3 has exactly 2956 data codewords.  The fixed four-byte
     * ECI/byte prefix leaves 2952 transport bytes = 48 header + 2904 payload. */
    if((u32)FRAME_HEADER_SIZE+cfg->frame_payload>2952UL) {
        printf("Payload %u exceeds fixed V40-L maximum of %u bytes.\n",
            cfg->frame_payload,MAX_FRAME_PAYLOAD);
        return 0;
    }
    return 1;
}

u16 config_redundancy_group(const Config *c) {
    return c->chain_width?0:c->redundancy;
}

const char *config_redundancy_name(const Config *c) {
    static char name[12];
    if(c->chain_width)sprintf(name,"C%u",c->chain_width);
    else if(c->redundancy)sprintf(name,"%u",c->redundancy);
    else strcpy(name,"OFF");
    return name;
}

const char *config_video_name(const Config *c) {
    return c->video_mode==VIDEO_320_70?"320_70":"320_60";
}

int config_parse_re(Config *cfg,const char *p) {
    char *end;
    long v;
    if(!p||!*p)return -1;
    if(toupper(*p)=='C') {
        v=strtol(p+1,&end,10);
        if(!p[1]||*end||v<2||v>MAX_WINDOW||(v&1))return -1;
        cfg->chain_width=(u8)v;
        cfg->redundancy=0;
        return 1;
    }
    v=strtol(p,&end,10);
    if(*end||v<0||v>MAX_WINDOW)return -1;
    cfg->redundancy=(u8)v;
    cfg->chain_width=0;
    return 1;
}

int config_parse_video(Config *cfg,const char *p) {
    if(!p||!*p)return -1;
    if(!stricmp(p,"320_60")||!stricmp(p,"320X200_60")||!stricmp(p,"320-60")) {
        cfg->video_mode=VIDEO_320_60;
        return 1;
    }
    if(!stricmp(p,"320_70")||!stricmp(p,"320X200_70")||!stricmp(p,"320-70")) {
        cfg->video_mode=VIDEO_320_70;
        return 1;
    }
    return -1;
}

int config_parse_option(Config *cfg,const char *arg) {
    long v;
    int rc;
    const char *p;

    p=option_value(arg,"RE");if(p)return config_parse_re(cfg,p);
    p=option_value(arg,"VIDEO");if(p)return config_parse_video(cfg,p);
    rc=option_number(arg,"PAYLOAD","P",96,MAX_FRAME_PAYLOAD,&v);
    if(rc){if(rc>0)cfg->frame_payload=(u16)v;return rc;}
    rc=option_number(arg,"HOLD",0,0,60000,&v);
    if(rc){if(rc>0)cfg->hold_ms=(u16)v;return rc;}
    rc=option_number(arg,"WINDOW","W",4,MAX_WINDOW,&v);
    if(rc){if(rc>0)cfg->window_frames=(u16)v;return rc;}
    rc=option_number(arg,"REPEAT","R",1,20,&v);
    if(rc){if(rc>0)cfg->repetitions=(u8)v;return rc;}
    rc=option_number(arg,"MASK",0,0,7,&v);
    if(rc){if(rc>0)cfg->qr_mask=(u8)v;return rc;}
    rc=option_number(arg,"ANCHOR",0,0,MAX_WINDOW,&v);
    if(rc){if(rc>0)cfg->chain_anchor=(u16)v;return rc;}
    if(!stricmp(arg,"/INVERT")||!stricmp(arg,"-INVERT")){cfg->invert=1;return 1;}
    if(!stricmp(arg,"/NOINVERT")||!stricmp(arg,"-NOINVERT")){cfg->invert=0;return 1;}
    if(!stricmp(arg,"/BEEP")||!stricmp(arg,"-BEEP")){cfg->speaker=1;return 1;}
    if(!stricmp(arg,"/NOBEEP")||!stricmp(arg,"-NOBEEP")){cfg->speaker=0;return 1;}
    return 0;
}

int config_is_split_re(const char *arg) {
    return !stricmp(arg,"/RE")||!stricmp(arg,"-RE");
}

int config_is_split_video(const char *arg) {
    return !stricmp(arg,"/VIDEO")||!stricmp(arg,"-VIDEO");
}

void config_print_usage(const Config *cfg) {
    puts("DOSfer legacy V40-L - optimized 16-bit DOS-to-Android sender");
    puts("DOSFER [options] file_or_directory [more paths ...]");
#ifdef DOSFER_DEVTOOLS
    puts("DOSFER /CAL [options]");
    puts("DOSFER /BENCH file [options]");
#endif
    puts("");
    puts("Fixed renderer: QR Version 40, 177x177 modules, 1 pixel/module, VGA 320x200.");
    puts("Default: V40-L, /VIDEO:320_60, payload 2904, hold 0, window 64, /RE:7");
    puts("/VIDEO:320_60|320_70  CRT refresh mode (default 320_60)");
    puts("/PAYLOAD:n or /P:n    Frame payload 96..2904 bytes for fixed V40-L");
    puts("/RE:n or /RE n        n DATA + 1 XOR parity; 0 disables (default 7)");
    puts("/RE:Ck or /RE Ck      Overlapping chain parity C2..C64 (even k)");
    puts("/ANCHOR:n             Repeat every nth DATA frame in chain mode");
    puts("/HOLD:ms              Minimum visible frame hold 0..60000 ms");
    puts("/WINDOW:n /W:n        Frames per acknowledged window 4..64");
    puts("/REPEAT:n /R:n        Complete passes per window 1..20");
    puts("/MASK:n               Fixed QR mask 0..7 (default 0)");
    puts("/INVERT /NOINVERT     Black/white polarity");
    puts("/BEEP /NOBEEP         End-of-window sound");
    printf("Current: V40-L /PAYLOAD:%u /HOLD:%u /WINDOW:%u /REPEAT:%u /MASK:%u /RE:%s /VIDEO:%s %s %s\n",
        cfg->frame_payload,cfg->hold_ms,cfg->window_frames,
        cfg->repetitions,cfg->qr_mask,config_redundancy_name(cfg),config_video_name(cfg),
        cfg->invert?"/INVERT":"/NOINVERT",cfg->speaker?"/BEEP":"/NOBEEP");
    puts("Example: DOSFER /VIDEO:320_60 /HOLD:50 /RE:C2 FILE.ZIP");
    puts("The first QR waits for Enter. Window-end controls: Enter/R/M/Esc.");
}
