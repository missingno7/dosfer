#ifndef TIMING32_H
#define TIMING32_H
#include <stdint.h>
uint32_t timer_ticks(void);
uint32_t timer_elapsed_ms(uint32_t start, uint32_t end);
void timer_wait_ms(uint16_t ms);
#endif
