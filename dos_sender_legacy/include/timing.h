#ifndef TIMING_H
#define TIMING_H
#include "dosfer.h"
u32 timer_ticks(void);
u32 timer_ticks_from_ms(u16 ms);
void timer_wait_ms(u16 ms);
u32 timer_elapsed_ms(u32 start,u32 end);
#endif

