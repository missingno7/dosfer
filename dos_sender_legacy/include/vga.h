#ifndef VGA_H
#define VGA_H

#include "dosfer.h"

enum {
    VGA_RGB_RED=0,
    VGA_RGB_GREEN=1,
    VGA_RGB_BLUE=2,
    VGA_RGB_CHANNELS=3
};

/* Fixed V40, 1-pixel/module, planar EGA/VGA Mode 0Dh backend. */
int vga_enter(VideoMode mode,int rgb3);
void vga_leave(void);

/* Monochrome bootstrap/full-redraw path. */
int vga_show_full_qr_at(const u8 *qr, const u8 *codewords,
                        int invert, const char *status, u32 earliest_tick);

/* RGB3 bootstrap/full-redraw path.  Each element is a complete standard QR. */
int vga_show_full_qr3_at(const u8 *const qr[VGA_RGB_CHANNELS],
                         u8 *const codewords[VGA_RGB_CHANNELS],
                         int invert,const char *status,u32 earliest_tick);

/* Monochrome steady-state paths. The direct renderer resets the shadow from
 * the fixed V40-L function template and scatters the complete codeword
 * stream; delta paths remain available as verification/fallback paths. */
int vga_apply_codewords_direct(const u8 *codewords);
/* Shared direct-template availability for the currently active video mode. */
int vga_direct_ready(void);
int vga_apply_v40l_delta(const u8 *data_codewords, const u8 *ecc_blocks,
                         u8 *current_codewords);
int vga_apply_codeword_delta(const u8 *next_codewords,
                             u8 *current_codewords);
int vga_show_prepared_at(int invert, const char *status, u32 earliest_tick);

/* Production RGB3 steady-state path. Each shadow plane is reset from the
 * fixed V40-L function-pattern template. Regular two-column placement runs
 * use run-level and four-lane byte-transpose kernels; function crossings use
 * the exact map. */
int vga_apply_codewords3_direct(
        const u8 *const codewords[VGA_RGB_CHANNELS]);

/* Retained delta paths are the verification oracle for the direct renderer. */
int vga_apply_v40l_delta3(
        const u8 *const data_codewords[VGA_RGB_CHANNELS],
        const u8 *const ecc_blocks[VGA_RGB_CHANNELS],
        u8 *const current_codewords[VGA_RGB_CHANNELS]);
int vga_apply_codeword_delta3(
        const u8 *const next_codewords[VGA_RGB_CHANNELS],
        u8 *const current_codewords[VGA_RGB_CHANNELS]);
int vga_show_prepared3_at(int invert,const char *status,u32 earliest_tick);

u32 vga_last_flip_tick(void);
int vga_delta_ready(void);
int vga_rgb3_active(void);
void speaker_beep(void);

#ifdef DOSFER_DEVTOOLS
u32 vga_measure_refresh_hz100(u16 samples);
void vga_delta_stats(u16 *bits);
u32 vga_screen_hash(void);
int vga_display_matches(void);
void vga_benchmark_qr(const u8 *qr, int loops,
                      u32 *build_ms, u32 *copy_ms, u32 *text_ms);
void vga_benchmark_delta(const u8 *codewords, int loops,
                         u32 *update_ms, u32 *copy_ms, u32 *text_ms);
#endif

#ifdef DOSFER_MAP_STATS
void vga_rgb3_map_stats_reset(void);
void vga_rgb3_map_stats(u32 *map_loads,u32 *red_xors,u32 *green_xors,
                        u32 *blue_xors);
void vga_rgb3_map_layout_stats(u32 *entries,u32 *within_codeword_bytes,
                               u32 *global_unique_bytes);
#endif

#ifdef DOSFER_DIRECT_BENCH
int vga_direct_reset_template(void);
int vga_direct_scatter(const u8 *codewords);
int vga_direct_scatter_asm(const u8 *codewords);
int vga_rgb3_direct_reset_template(void);
int vga_rgb3_direct_scatter(const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_asm(const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_stripe(const u8 *const codewords[VGA_RGB_CHANNELS]);
void vga_rgb3_stripe_stats(u16 *runs,u16 *covered);
int vga_rgb3_direct_scatter_stripe_runs(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_stripe_fallback(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_stripe_setup(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
void vga_rgb3_stripe_geometry_stats(u16 run_count[4],u16 codeword_count[4],
                                    u16 *other_runs,u16 *other_codewords);
void vga_rgb3_stripe_group_stats(u16 *groups,u16 *source_codewords,
                                 u16 *destination_bytes);
void vga_rgb3_stripe_phase_stats(u16 *groups,u16 *source_codewords,
        u16 *destination_bytes,u16 *edge_contributions);
int vga_rgb3_direct_scatter_group_baseline(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_groups(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_grouped(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_phase_baseline(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_phase_groups(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_phase_kernel(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_phase_edges(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
int vga_rgb3_direct_scatter_phased(
        const u8 *const codewords[VGA_RGB_CHANNELS]);
#endif

#endif
