#ifndef DOSFER32_PLATFORM_VGA_H
#define DOSFER32_PLATFORM_VGA_H

#include <stdint.h>

#define VGA_SLOT_BYTES 8192u
#define VGA_RASTER_BYTES 8000u
#define VGA_PLANE_MAX_CORRECTION_PATCHES 1024u

typedef struct {
    uint8_t *raster;
    uint8_t *readback;
    uint8_t *zero_raster;
    uint16_t slot_step;
    int active;
    int zero_ready;
} Vga32;

int vga32_enter(Vga32 *vga);
void vga32_leave(Vga32 *vga);
int vga32_store_fast(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster);
int vga32_store(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster);
int vga32_show(Vga32 *vga, unsigned slot, unsigned mask);
int vga32_verify(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster);
int vga32_xor(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *delta);
int vga32_apply_correction(Vga32 *vga, unsigned plane, unsigned slot,
    const uint16_t *patch_offset, const uint8_t *patch_xor, uint16_t patch_count, int include_zero,
    uint32_t *restore_hash, int verify);
int vga32_restore_correction(Vga32 *vga, unsigned plane, unsigned slot,
    const uint16_t *patch_offset, const uint8_t *patch_xor, uint16_t patch_count, int include_zero,
    uint32_t restore_hash, int verify);
uint32_t vga32_hash(Vga32 *vga, unsigned plane, unsigned slot);
int vga32_verify_composed(Vga32 *vga, unsigned slot, unsigned mask, const uint8_t *canonical);
void vga32_compose_raster(const uint8_t *const planes[4], unsigned mask, uint8_t *out);
int vga32_read_planes(Vga32 *vga, unsigned slot, uint8_t *plane_out[4]);
void vga32_wait_retrace(void);
void vga32_use_plane_palette(void);

int vga32_show_raw(Vga32 *vga, unsigned slot, unsigned mask, int wait_retrace);
/* Plain DATA tail frames: normal monochrome palette, identical raster in all planes. */
int vga32_show_data_qr(Vga32 *vga, unsigned slot, const uint8_t *raster, int wait_retrace);

#endif
