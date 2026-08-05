#ifndef DOSFER32_PLATFORM_VGA_H
#define DOSFER32_PLATFORM_VGA_H

#include <stdint.h>

#define VGA_SLOT_BYTES 8192u
#define VGA_RASTER_BYTES 8000u

typedef struct {
    uint8_t *raster;
    uint8_t *readback;
    uint16_t slot_step;
    int active;
} Vga32;

int vga32_enter(Vga32 *vga);
void vga32_leave(Vga32 *vga);
int vga32_store_fast(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster);
int vga32_store(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster);
int vga32_show(Vga32 *vga, unsigned slot, unsigned mask);
int vga32_verify(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster);
int vga32_xor(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *delta);
uint32_t vga32_hash(Vga32 *vga, unsigned plane, unsigned slot);
int vga32_verify_composed(Vga32 *vga, unsigned slot, unsigned mask, const uint8_t *canonical);
void vga32_compose_raster(const uint8_t *const planes[4], unsigned mask, uint8_t *out);
int vga32_read_planes(Vga32 *vga, unsigned slot, uint8_t *plane_out[4]);
void vga32_wait_retrace(void);

#endif
