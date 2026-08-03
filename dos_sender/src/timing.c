#include <dos.h>
#include <i86.h>
#include "timing.h"
u32 timer_ticks(void) {
    union REGS r; r.h.ah=0; int86(0x1A,&r,&r);
    return ((u32)r.x.cx<<16)|r.x.dx;
}
u32 timer_elapsed_ms(u32 a,u32 b) {
    u32 d=(b>=a)?b-a:(0x1800B0UL-a)+b;
    return (d*54925UL)/1000UL;
}
void timer_wait_ms(u16 ms) {
    u32 start=timer_ticks(),need=((u32)ms+54UL)/55UL;
    while (((timer_ticks()-start)&0x00FFFFFFUL)<need) ;
}

