#include "platform_vga.h"
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <malloc.h>
#include <string.h>

static volatile uint8_t *const vga_memory = (volatile uint8_t *)0xA0000UL;

static void bios_mode(unsigned mode) {
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0;
    r.h.al = (uint8_t)mode;
    int386(0x10, &r, &r);
}

static uint16_t crtc_get_start(void) {
    uint8_t hi, lo;
    outp(0x3D4, 0x0C); hi = inp(0x3D5);
    outp(0x3D4, 0x0D); lo = inp(0x3D5);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static int bios_page(unsigned page) {
    union REGS r;
    memset(&r, 0, sizeof(r)); r.h.ah = 5; r.h.al = (uint8_t)page;
    int386(0x10, &r, &r);
    return 1;
}

static uint16_t measure_crtc_slot_step(void) {
    uint16_t first, second;
    bios_page(0); first = crtc_get_start();
    bios_page(1); second = crtc_get_start();
    bios_page(0);
    return (uint16_t)(second - first);
}

static void plane_write_setup(uint8_t mask) {
    outp(0x3C4, 2); outp(0x3C5, mask);
    outp(0x3CE, 0); outp(0x3CF, 0);
    outp(0x3CE, 1); outp(0x3CF, 0);
    outp(0x3CE, 3); outp(0x3CF, 0);
    outp(0x3CE, 5); outp(0x3CF, 0);
    outp(0x3CE, 8); outp(0x3CF, 0xFF);
}

static void palette_odd_parity(void) {
    unsigned i, bits;
    outp(0x3C8, 0);
    for (i = 0; i < 16; ++i) {
        bits = i ^ (i >> 1) ^ (i >> 2) ^ (i >> 3);
        bits &= 1;
        outp(0x3C9, bits ? 63 : 0);
        outp(0x3C9, bits ? 63 : 0);
        outp(0x3C9, bits ? 63 : 0);
    }
}

static void palette_normal(void) {
    unsigned i, v;
    outp(0x3C8, 0);
    for (i = 0; i < 16; ++i) {
        v = (i == 15) ? 63 : 0;
        outp(0x3C9, v);
        outp(0x3C9, v);
        outp(0x3C9, v);
    }
}

static void attribute_map_identity(void) {
    unsigned i;
    for (i = 0; i < 16; ++i) {
        inp(0x3DA);
        outp(0x3C0, (uint8_t)i);
        outp(0x3C0, (uint8_t)i);
    }
    inp(0x3DA); outp(0x3C0, 0x20);
}

void vga32_use_plane_palette(void) {
    palette_odd_parity();
    attribute_map_identity();
    plane_write_setup(0x0F);
}

static void attribute_plane_mask(uint8_t mask) {
    inp(0x3DA);
    outp(0x3C0, 0x32);
    outp(0x3C0, mask);
    outp(0x3C0, 0x20);
}

static void crtc_start(uint16_t start) {
    outp(0x3D4, 0x0C); outp(0x3D5, (uint8_t)(start >> 8));
    outp(0x3D4, 0x0D); outp(0x3D5, (uint8_t)start);
}

void vga32_wait_retrace(void) {
    unsigned long guard = 100000UL;
    while ((inp(0x3DA) & 8) && guard--) {}
    guard = 100000UL;
    while (!(inp(0x3DA) & 8) && guard--) {}
}

int vga32_enter(Vga32 *vga) {
    unsigned i;
    if (!vga) return 0;
    memset(vga, 0, sizeof(*vga));
    vga->raster = (uint8_t *)malloc(VGA_RASTER_BYTES);
    vga->readback = (uint8_t *)malloc(VGA_RASTER_BYTES);
    vga->zero_raster = (uint8_t *)malloc(VGA_RASTER_BYTES);
    if (!vga->raster || !vga->readback || !vga->zero_raster) { vga32_leave(vga); return 0; }
    bios_mode(0x0D);
    /* CRTC start addresses are word addresses, and the BIOS page stride is
       mode-dependent.  Measure it instead of treating 8000 raster bytes as
       a CRTC register value. */
    vga->slot_step = measure_crtc_slot_step();
    if (!vga->slot_step || (unsigned long)vga->slot_step * 7UL > 0xFFFFUL) {
        vga32_leave(vga); return 0;
    }
    vga->active = 1;
    plane_write_setup(0x0F);
    attribute_map_identity();
    palette_odd_parity();
    attribute_plane_mask(0);
    /* Clear all eight slots in every physical plane. */
    memset(vga->raster, 0, VGA_RASTER_BYTES);
    for (i = 0; i < 4; ++i)
        for (unsigned slot = 0; slot < 8; ++slot)
            if (!vga32_store(vga, i, slot, vga->raster)) { vga32_leave(vga); return 0; }
    return 1;
}

void vga32_leave(Vga32 *vga) {
    if (!vga) return;
    if (vga->active) {
        attribute_plane_mask(0x0F);
        bios_mode(3);
    }
    free(vga->raster);
    free(vga->readback);
    free(vga->zero_raster);
    memset(vga, 0, sizeof(*vga));
}

int vga32_store_fast(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster) {
    uint16_t offset;
    if (!vga || !vga->active || !raster || plane > 3 || slot > 7) return 0;
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    plane_write_setup((uint8_t)(1u << plane));
    memcpy((void *)(vga_memory + offset), raster, VGA_RASTER_BYTES);
    plane_write_setup(0x0F);
    outp(0x3CE, 4); outp(0x3CF, 0);
    return 1;
}

int vga32_store(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster) {
    if (!vga32_store_fast(vga, plane, slot, raster)) return 0;
    return vga32_verify(vga, plane, slot, raster);
}

int vga32_verify(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster) {
    uint16_t offset;
    int ok;
    if (!vga || !vga->active || !raster || plane > 3 || slot > 7) return 0;
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    outp(0x3CE, 4); outp(0x3CF, (uint8_t)plane);
    memcpy(vga->readback, (const void *)(vga_memory + offset), VGA_RASTER_BYTES);
    ok = memcmp(vga->readback, raster, VGA_RASTER_BYTES) == 0;
    outp(0x3CE, 4); outp(0x3CF, 0);
    return ok;
}

static void vga_xor_bulk(uint16_t offset, const uint8_t *delta) {
    volatile uint32_t *dst = (volatile uint32_t *)(vga_memory + offset);
    const uint32_t *src = (const uint32_t *)delta;
    unsigned i;
    for (i = 0; i < VGA_RASTER_BYTES / 4u; ++i) dst[i] ^= src[i];
    for (i = (VGA_RASTER_BYTES & ~3u); i < VGA_RASTER_BYTES; ++i)
        vga_memory[offset + i] ^= delta[i];
}

int vga32_xor(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *delta) {
    uint16_t offset;
    if (!vga || !vga->active || !delta || plane > 3 || slot > 7) return 0;
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    plane_write_setup((uint8_t)(1u << plane));
    /* A read-modify-write must load the latch from the same physical plane
       that receives the write. Do not inherit Graphics Controller state from
       a previous verification read. */
    outp(0x3CE, 4); outp(0x3CF, (uint8_t)plane);
    vga_xor_bulk(offset, delta);
    plane_write_setup(0x0F);
    outp(0x3CE, 4); outp(0x3CF, 0);
    return 1;
}

int vga32_apply_correction(Vga32 *vga, unsigned plane, unsigned slot,
    const uint16_t *patch_offset, const uint8_t *patch_xor, uint16_t patch_count, int include_zero,
    uint32_t *restore_hash, int verify) {
    uint16_t offset, i, base;
    if (!vga || !vga->active || !vga->zero_raster || !patch_offset || !patch_xor ||
        !restore_hash || patch_count > VGA_PLANE_MAX_CORRECTION_PATCHES ||
        plane > 3 || slot > 7) return 0;
    base = (uint16_t)(slot * VGA_SLOT_BYTES);
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    *restore_hash = verify ? vga32_hash(vga, plane, slot) : 0;
    plane_write_setup((uint8_t)(1u << plane));
    outp(0x3CE, 4); outp(0x3CF, (uint8_t)plane);
    if (include_zero) vga_xor_bulk(offset, vga->zero_raster);
    for (i = 0; i < patch_count; ++i) vga_memory[base + patch_offset[i]] ^= patch_xor[i];
    plane_write_setup(0x0F);
    outp(0x3CE, 4); outp(0x3CF, 0);
    return 1;
}

int vga32_restore_correction(Vga32 *vga, unsigned plane, unsigned slot,
    const uint16_t *patch_offset, const uint8_t *patch_xor, uint16_t patch_count, int include_zero,
    uint32_t restore_hash, int verify) {
    uint16_t offset, i, base;
    uint32_t after;
    if (!vga || !vga->active || !vga->zero_raster || !patch_offset || !patch_xor ||
        patch_count > VGA_PLANE_MAX_CORRECTION_PATCHES || plane > 3 || slot > 7) return 0;
    base = (uint16_t)(slot * VGA_SLOT_BYTES);
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    plane_write_setup((uint8_t)(1u << plane));
    outp(0x3CE, 4); outp(0x3CF, (uint8_t)plane);
    if (include_zero) vga_xor_bulk(offset, vga->zero_raster);
    for (i = 0; i < patch_count; ++i) vga_memory[base + patch_offset[i]] ^= patch_xor[i];
    plane_write_setup(0x0F);
    outp(0x3CE, 4); outp(0x3CF, 0);
    if (!verify) return 1;
    after = vga32_hash(vga, plane, slot);
    return after == restore_hash;
}

uint32_t vga32_hash(Vga32 *vga, unsigned plane, unsigned slot) {
    uint16_t offset; unsigned i; uint32_t h = 2166136261UL;
    if (!vga || !vga->active || plane > 3 || slot > 7) return 0;
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    outp(0x3CE, 4); outp(0x3CF, (uint8_t)plane);
    memcpy(vga->readback, (const void *)(vga_memory + offset), VGA_RASTER_BYTES);
    for (i = 0; i < VGA_RASTER_BYTES; ++i) { h ^= vga->readback[i]; h *= 16777619UL; }
    outp(0x3CE, 4); outp(0x3CF, 0);
    return h;
}

int vga32_verify_composed(Vga32 *vga, unsigned slot, unsigned mask, const uint8_t *canonical) {
    uint16_t offset; unsigned i, plane, first = 4;
    if (!vga || !vga->active || !canonical || slot > 7 || !mask || mask > 15) return 0;
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    for (plane = 0; plane < 4; ++plane) if (mask & (1u << plane)) { first = plane; break; }
    outp(0x3CE, 4); outp(0x3CF, (uint8_t)first);
    memcpy(vga->readback, (const void *)(vga_memory + offset), VGA_RASTER_BYTES);
    for (plane = first + 1u; plane < 4; ++plane) if (mask & (1u << plane)) {
        outp(0x3CE, 4); outp(0x3CF, (uint8_t)plane);
        memcpy(vga->raster, (const void *)(vga_memory + offset), VGA_RASTER_BYTES);
        for (i = 0; i < VGA_RASTER_BYTES; ++i) vga->readback[i] ^= vga->raster[i];
    }
    outp(0x3CE, 4); outp(0x3CF, 0);
    return memcmp(vga->readback, canonical, VGA_RASTER_BYTES) == 0;
}

int vga32_show_raw(Vga32 *vga, unsigned slot, unsigned mask, int wait_retrace) {
    if (!vga || !vga->active || slot > 7 || mask == 0 || mask > 15) return 0;
    if (wait_retrace) vga32_wait_retrace();
    crtc_start((uint16_t)(slot * vga->slot_step));
    attribute_plane_mask((uint8_t)mask);
    return 1;
}

int vga32_show(Vga32 *vga, unsigned slot, unsigned mask) {
    return vga32_show_raw(vga, slot, mask, 1);
}

int vga32_show_data_qr(Vga32 *vga, unsigned slot, const uint8_t *raster, int wait_retrace) {
    unsigned plane;
    if (!vga || !vga->active || !raster || slot > 7) return 0;
    /* Keep the ordinary tail QR visible; do not blank it while its hold interval expires. */
    palette_normal();
    attribute_map_identity();
    plane_write_setup(0x0F);
    for (plane = 0; plane < 4; ++plane)
        if (!vga32_store_fast(vga, plane, slot, raster)) return 0;
    if (!vga32_show_raw(vga, slot, 0x0F, wait_retrace)) return 0;
    attribute_plane_mask(0x0F);
    return 1;
}

void vga32_compose_raster(const uint8_t *const planes[4], unsigned mask, uint8_t *out) {
    unsigned i, plane;
    if (!planes || !out || !mask || mask > 15) return;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) {
        uint8_t value = 0;
        for (plane = 0; plane < 4; ++plane)
            if (mask & (1u << plane)) value ^= planes[plane][i];
        out[i] = value;
    }
}

int vga32_read_planes(Vga32 *vga, unsigned slot, uint8_t *plane_out[4]) {
    uint16_t offset;
    unsigned plane;
    if (!vga || !vga->active || slot > 7 || !plane_out) return 0;
    offset = (uint16_t)(slot * VGA_SLOT_BYTES);
    for (plane = 0; plane < 4; ++plane) {
        if (!plane_out[plane]) return 0;
        outp(0x3CE, 4); outp(0x3CF, (uint8_t)plane);
        memcpy(plane_out[plane], (const void *)(vga_memory + offset), VGA_RASTER_BYTES);
    }
    outp(0x3CE, 4); outp(0x3CF, 0);
    return 1;
}
