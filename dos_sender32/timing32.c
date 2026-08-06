#include <conio.h>
#include <stdint.h>
#include "timing32.h"

#define BIOS_TICKS_PER_DAY 0x1800B0UL
#define TIMER_TICKS_PER_BIOS 4096UL
#define TIMER_TICKS_PER_SECOND 74574UL

static uint32_t last_bios_tick, timer_total;
static uint16_t last_phase;
static uint8_t timer_initialized, pending_bios_tick;

static uint32_t bios_ticks(void) {
    /* In protected mode under DOS4GW, the BIOS tick counter at 0040:006C
     * is accessed through a linear pointer. DOS4GW maps the first 1MB of
     * physical memory linearly, so linear address 0x46C points to the
     * BIOS tick counter. */
    volatile uint32_t *tick = (volatile uint32_t *)0x46C;
    return *tick;
}

uint32_t timer_ticks(void) {
    uint32_t before, after, count_value, phase, bios_delta, delta;
    uint16_t count;
    uint8_t status, lo, hi, mode;

    do {
        before = bios_ticks();
        /* 8254 read-back command: latch status+count for counter 0 */
        outp(0x43, 0xC2);
        status = (uint8_t)inp(0x40);
        lo = (uint8_t)inp(0x40);
        hi = (uint8_t)inp(0x40);
        after = bios_ticks();
    } while (before != after);

    count = (uint16_t)(lo | ((uint16_t)hi << 8));
    count_value = count ? (uint32_t)count : 65536UL;
    mode = (uint8_t)((status >> 1) & 7);

    if (mode == 3 || mode == 7) {
        phase = (65536UL - count_value) >> 1;
        if (!(status & 0x80)) phase += 32768UL;
    } else if (mode == 2 || mode == 6) {
        phase = 65536UL - count_value;
    } else {
        phase = 0;
    }
    phase >>= 4;

    if (!timer_initialized) {
        last_bios_tick = after;
        last_phase = (uint16_t)phase;
        timer_initialized = 1;
        return 0;
    }

    bios_delta = after >= last_bios_tick ? after - last_bios_tick :
        (BIOS_TICKS_PER_DAY - last_bios_tick) + after;

    if (bios_delta && pending_bios_tick) {
        bios_delta--;
        pending_bios_tick = 0;
    }

    if (phase >= (uint32_t)last_phase)
        delta = bios_delta * TIMER_TICKS_PER_BIOS + phase - last_phase;
    else if (bios_delta)
        delta = (bios_delta - 1) * TIMER_TICKS_PER_BIOS +
            TIMER_TICKS_PER_BIOS - last_phase + phase;
    else {
        delta = TIMER_TICKS_PER_BIOS - last_phase + phase;
        pending_bios_tick = 1;
    }

    timer_total += delta;
    last_bios_tick = after;
    last_phase = (uint16_t)phase;
    return timer_total;
}

uint32_t timer_elapsed_ms(uint32_t start, uint32_t end) {
    uint32_t d = end - start;
    return (d / TIMER_TICKS_PER_SECOND) * 1000UL +
        ((d % TIMER_TICKS_PER_SECOND) * 1000UL) / TIMER_TICKS_PER_SECOND;
}

void timer_wait_ms(uint16_t ms) {
    uint32_t start = timer_ticks();
    uint32_t need = (uint32_t)ms * 74UL + ((uint32_t)ms * 574UL + 999UL) / 1000UL;
    while (timer_ticks() - start < need) {
    }
}
