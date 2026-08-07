#include <conio.h>
#include <dos.h>
#include <i86.h>
#include "timing.h"

#define BIOS_TICKS_PER_DAY 0x1800B0UL
#define TIMER_TICKS_PER_BIOS 4096UL
#define TIMER_TICKS_PER_SECOND 74574UL

static u32 last_bios_tick,timer_total;
static u16 last_phase;
static u8 timer_initialized,pending_bios_tick;

/* OpenWatcom's far dereference uses DS in the large memory model. Preserve it
 * explicitly because the optimized QR assembly expects DS to remain DGROUP. */
#ifdef DOSFER_HOST_TEST
/* Host syntax/oracle builds do not execute the DOS timer path, but providing a
 * definition keeps the complete release/dev/profile source set compilable by
 * normal C compilers that do not understand Open Watcom's #pragma aux. */
static u32 bios_ticks(void) { return 0; }
#else
static u32 bios_ticks(void);
#pragma aux bios_ticks = \
    "push ds" \
    "push bx" \
    "mov ax,0040h" \
    "mov ds,ax" \
    "mov bx,006ch" \
    "mov ax,[bx]" \
    "mov dx,2[bx]" \
    "pop bx" \
    "pop ds" \
    value [dx ax] modify [ax dx];
#endif
u32 timer_ticks(void) {
    u32 before,after,count_value,phase,bios_delta,delta;
    u16 count;u8 status,lo,hi,mode;
    do {
        before=bios_ticks();
        /* 8254 read-back 11000010b atomically latches status and count for
         * counter 0. Reading port 40h then returns status, LSB and MSB. */
        outp(0x43,0xC2);status=(u8)inp(0x40);
        lo=(u8)inp(0x40);hi=(u8)inp(0x40);count=(u16)(lo|((u16)hi<<8));
        after=bios_ticks();
    } while(before!=after);
    count_value=count?(u32)count:65536UL;
    mode=(u8)((status>>1)&7);
    if(mode==3||mode==7) {
        /* Mode 3 decrements by two in each half-cycle. OUT distinguishes
         * the otherwise identical high and low counter sequences. */
        phase=(65536UL-count_value)>>1;
        if(!(status&0x80))phase+=32768UL;
    } else if(mode==2||mode==6)phase=65536UL-count_value;
    else phase=0;
    phase>>=4;
    if(!timer_initialized) {
        last_bios_tick=after;last_phase=(u16)phase;timer_initialized=1;
        return 0;
    }
    bios_delta=after>=last_bios_tick?after-last_bios_tick:
        (BIOS_TICKS_PER_DAY-last_bios_tick)+after;
    /* The PIT can reload just before IRQ0 advances the BIOS tick. Count that
     * wrap immediately, then consume the delayed BIOS increment next time. */
    if(bios_delta&&pending_bios_tick){bios_delta--;pending_bios_tick=0;}
    if(phase>=(u32)last_phase)
        delta=bios_delta*TIMER_TICKS_PER_BIOS+phase-last_phase;
    else if(bios_delta)
        delta=(bios_delta-1)*TIMER_TICKS_PER_BIOS+
            TIMER_TICKS_PER_BIOS-last_phase+phase;
    else {
        delta=TIMER_TICKS_PER_BIOS-last_phase+phase;
        pending_bios_tick=1;
    }
    timer_total+=delta;last_bios_tick=after;last_phase=(u16)phase;
    return timer_total;
}
u32 timer_ticks_from_ms(u16 ms) {
    return (u32)ms*74UL+((u32)ms*574UL+999UL)/1000UL;
}

#ifdef DOSFER_DEVTOOLS
u32 timer_elapsed_ms(u32 a,u32 b) {
    u32 d=b-a;
    return (d/TIMER_TICKS_PER_SECOND)*1000UL+
        ((d%TIMER_TICKS_PER_SECOND)*1000UL)/TIMER_TICKS_PER_SECOND;
}

void timer_wait_ms(u16 ms) {
    u32 start=timer_ticks();
    u32 need=timer_ticks_from_ms(ms);
    while(timer_ticks()-start<need) ;
}
#endif
