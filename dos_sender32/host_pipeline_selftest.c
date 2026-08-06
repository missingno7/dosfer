/* End-to-end host oracle for dos_sender32's producer/playback state machine.
 * It includes main.c directly and replaces only the DOS/VGA/timer boundary.
 * /VERIFY therefore still checks every generated PLANE parity raster. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define stricmp strcasecmp
#define strnicmp strncasecmp
#define main dosfer32_embedded_main
#include "main.c"
#undef main

static uint8_t host_planes[4][8][VGA_RASTER_BYTES];
static uint32_t host_ticks;
static unsigned host_visible_symbols;
static unsigned host_current_slot;
static unsigned host_current_mask;
static uint8_t host_expected_visible[VGA_RASTER_BYTES];
static int host_visible_integrity = 1;

uint32_t timer_ticks(void) { host_ticks += 16u; return host_ticks; }
uint32_t timer_elapsed_ms(uint32_t start, uint32_t end) {
    return (uint32_t)(((uint64_t)(end - start) * 1000u) / 74574u);
}
void timer_wait_ms(uint16_t ms) { host_ticks += (uint32_t)ms * 75u; }
void delay(unsigned ms) { host_ticks += (uint32_t)ms * 75u; }
int kbhit(void) { return 1; }
int getch(void) { return 13; }
#ifdef _WIN32
int _mkdir(const char *path) { return mkdir(path); }
#else
int _mkdir(const char *path) { return mkdir(path, 0777); }
#endif
unsigned long _memavl(void) { return 64UL * 1024UL * 1024UL; }

static uint32_t host_hash(const uint8_t *data) {
    unsigned i; uint32_t h = 2166136261UL;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) { h ^= data[i]; h *= 16777619UL; }
    return h;
}

static void host_compose(unsigned slot, unsigned mask, uint8_t *out) {
    unsigned i, p;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) {
        uint8_t value = 0;
        for (p = 0; p < 4u; ++p) if (mask & (1u << p))
            value ^= host_planes[p][slot][i];
        out[i] = value;
    }
}

static void host_check_visible(const char *operation) {
    uint8_t actual[VGA_RASTER_BYTES];
    if (!host_current_mask) return;
    host_compose(host_current_slot, host_current_mask, actual);
    if (memcmp(actual, host_expected_visible, VGA_RASTER_BYTES) != 0) {
        if (host_visible_integrity)
            fprintf(stderr, "visible mutation during %s slot=%u mask=%02X\n",
                    operation, host_current_slot, host_current_mask);
        host_visible_integrity = 0;
    }
}

int vga32_enter(Vga32 *vga) {
    unsigned p, s;
    memset(vga, 0, sizeof(*vga));
    vga->raster = (uint8_t *)malloc(VGA_RASTER_BYTES);
    vga->readback = (uint8_t *)malloc(VGA_RASTER_BYTES);
    vga->zero_raster = (uint8_t *)malloc(VGA_RASTER_BYTES);
    if (!vga->raster || !vga->readback || !vga->zero_raster) return 0;
    vga->slot_step = VGA_SLOT_BYTES / 2u; vga->active = 1;
    memset(vga->zero_raster, 0xFF, VGA_RASTER_BYTES);
    for (p = 0; p < 4u; ++p) for (s = 0; s < 8u; ++s)
        memset(host_planes[p][s], 0xFF, VGA_RASTER_BYTES);
    return 1;
}
void vga32_leave(Vga32 *vga) {
    if (!vga) return;
    host_check_visible("leave");
    free(vga->raster); free(vga->readback); free(vga->zero_raster);
    memset(vga, 0, sizeof(*vga));
}
int vga32_store_fast(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster) {
    if (!vga || !vga->active || plane > 3u || slot > 7u || !raster) return 0;
    memcpy(host_planes[plane][slot], raster, VGA_RASTER_BYTES);
    host_check_visible("store_fast"); return 1;
}
int vga32_store_qr(Vga32 *vga, unsigned plane, unsigned slot,
                    const uint8_t *raster, unsigned qr_x, unsigned qr_y,
                    unsigned qr_size) {
    unsigned first, last, bytes, y;
    if (!vga || !vga->active || plane > 3u || slot > 7u || !raster ||
        qr_x + qr_size > 320u || qr_y + qr_size > 200u) return 0;
    first = qr_x >> 3; last = (qr_x + qr_size + 7u) >> 3; bytes = last - first;
    for (y = 0; y < qr_size; ++y)
        memcpy(host_planes[plane][slot] + (qr_y + y) * 40u + first,
               raster + (qr_y + y) * 40u + first, bytes);
    host_check_visible("store_qr");
    return 1;
}

int vga32_verify(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster) {
    (void)vga;
    return plane < 4u && slot < 8u && raster &&
           memcmp(host_planes[plane][slot], raster, VGA_RASTER_BYTES) == 0;
}
int vga32_store(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *raster) {
    return vga32_store_fast(vga, plane, slot, raster) && vga32_verify(vga, plane, slot, raster);
}
int vga32_xor(Vga32 *vga, unsigned plane, unsigned slot, const uint8_t *delta) {
    unsigned i; (void)vga;
    if (plane > 3u || slot > 7u || !delta) return 0;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) host_planes[plane][slot][i] ^= delta[i];
    host_check_visible("xor");
    return 1;
}
uint32_t vga32_hash(Vga32 *vga, unsigned plane, unsigned slot) {
    (void)vga; return plane < 4u && slot < 8u ? host_hash(host_planes[plane][slot]) : 0;
}
int vga32_apply_correction(Vga32 *vga, unsigned plane, unsigned slot,
    const uint16_t *patch_offset, const uint8_t *patch_xor, uint16_t patch_count,
    int include_zero, uint32_t *restore_hash, int verify) {
    unsigned i;
    if (!vga || plane > 3u || slot > 7u || !patch_offset || !patch_xor || !restore_hash) return 0;
    *restore_hash = verify ? vga32_hash(vga, plane, slot) : 0;
    if (include_zero) for (i = 0; i < VGA_RASTER_BYTES; ++i)
        host_planes[plane][slot][i] ^= vga->zero_raster[i];
    for (i = 0; i < patch_count; ++i)
        host_planes[plane][slot][patch_offset[i]] ^= patch_xor[i];
    host_check_visible("apply_correction");
    return 1;
}
int vga32_restore_correction(Vga32 *vga, unsigned plane, unsigned slot,
    const uint16_t *patch_offset, const uint8_t *patch_xor, uint16_t patch_count,
    int include_zero, uint32_t restore_hash, int verify) {
    unsigned i;
    if (!vga || plane > 3u || slot > 7u || !patch_offset || !patch_xor) return 0;
    if (include_zero) for (i = 0; i < VGA_RASTER_BYTES; ++i)
        host_planes[plane][slot][i] ^= vga->zero_raster[i];
    for (i = 0; i < patch_count; ++i)
        host_planes[plane][slot][patch_offset[i]] ^= patch_xor[i];
    host_check_visible("restore_correction");
    return !verify || vga32_hash(vga, plane, slot) == restore_hash;
}
void vga32_compose_raster(const uint8_t *const planes[4], unsigned mask, uint8_t *out) {
    unsigned i, p;
    for (i = 0; i < VGA_RASTER_BYTES; ++i) {
        uint8_t v = 0;
        for (p = 0; p < 4u; ++p) if (mask & (1u << p)) v ^= planes[p][i];
        out[i] = v;
    }
}
int vga32_verify_composed(Vga32 *vga, unsigned slot, unsigned mask, const uint8_t *canonical) {
    const uint8_t *p[4]; unsigned i; uint8_t composed[VGA_RASTER_BYTES]; (void)vga;
    for (i = 0; i < 4u; ++i) p[i] = host_planes[i][slot];
    vga32_compose_raster(p, mask, composed);
    return memcmp(composed, canonical, VGA_RASTER_BYTES) == 0;
}
int vga32_read_planes(Vga32 *vga, unsigned slot, uint8_t *plane_out[4]) {
    unsigned p; (void)vga;
    if (slot > 7u) return 0;
    for (p = 0; p < 4u; ++p) memcpy(plane_out[p], host_planes[p][slot], VGA_RASTER_BYTES);
    return 1;
}
int vga32_show_raw(Vga32 *vga, unsigned slot, unsigned mask, int wait_retrace) {
    (void)vga; (void)wait_retrace;
    if (slot > 7u || !mask || mask > 15u) return 0;
    host_current_slot = slot; host_current_mask = mask;
    host_compose(slot, mask, host_expected_visible);
    ++host_visible_symbols; return 1;
}
int vga32_show(Vga32 *vga, unsigned slot, unsigned mask) { return vga32_show_raw(vga, slot, mask, 1); }
int vga32_show_data_qr(Vga32 *vga, unsigned slot, const uint8_t *raster, int wait_retrace) {
    unsigned p;
    for (p = 0; p < 4u; ++p) if (!vga32_store_fast(vga, p, slot, raster)) return 0;
    return vga32_show_raw(vga, slot, 15u, wait_retrace);
}
void vga32_wait_retrace(void) { host_ticks += 1065u; }
void vga32_use_plane_palette(void) {}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "PLANE4";
    unsigned file_size = argc > 2 ? (unsigned)strtoul(argv[2], 0, 0) : 85000u;
    unsigned window = argc > 3 ? (unsigned)strtoul(argv[3], 0, 0) : 32u;
    FILE *f; unsigned i; int rc;
    char window_arg[32];
    char *args[8];
    f = fopen("HOSTDATA.BIN", "wb");
    if (!f) return 2;
    for (i = 0; i < file_size; ++i) fputc((int)((i * 73u + (i >> 3)) & 255u), f);
    fclose(f);
    args[0] = "DOSFER32"; args[1] = "HOSTDATA.BIN";
    args[2] = !strcmp(mode, "PLANE3") ? "/RE:PLANE3" : "/RE:PLANE4";
    sprintf(window_arg, "/WINDOW:%u", window);
    args[3] = window_arg; args[4] = "/HOLD:50";
    args[5] = "/NOFOCUS"; args[6] = "/NORETRACE"; args[7] = "/VERIFY";
    rc = dosfer32_embedded_main(8, args);
    unlink("HOSTDATA.BIN");
    unlink("DOSFER32.PRO");
    unlink("DOSFER32.TRC");
    if (rc != 0) { printf("host pipeline %s failed rc=%d\n", mode, rc); return 1; }
    if (host_visible_symbols == 0u) { puts("no visible symbols"); return 1; }
    if (!host_visible_integrity) {
        puts("visible VGA raster changed between display selections"); return 1;
    }
    printf("host pipeline %s size=%u window=%u passed with %u visible symbols\n",
           mode, file_size, window, host_visible_symbols);
    return 0;
}
