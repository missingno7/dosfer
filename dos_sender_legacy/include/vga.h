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

/* Legacy monochrome bootstrap/full-redraw path. */
int vga_show_full_qr_at(const u8 *qr, const u8 *codewords,
                        int invert, const char *status, u32 earliest_tick);

/* RGB3 bootstrap/full-redraw path.  Each element is a complete standard QR. */
int vga_show_full_qr3_at(const u8 *const qr[VGA_RGB_CHANNELS],
                         u8 *const codewords[VGA_RGB_CHANNELS],
                         int invert,const char *status,u32 earliest_tick);

/* Legacy monochrome steady-state paths. */
int vga_apply_v40l_delta(const u8 *data_codewords, const u8 *ecc_blocks,
                         u8 *current_codewords);
int vga_apply_codeword_delta(const u8 *next_codewords,
                             u8 *current_codewords);
int vga_show_prepared_at(int invert, const char *status, u32 earliest_tick);

/* Fused RGB3 steady-state paths.  The shared placement map is traversed once
 * while all three independent QR bitplanes are updated. */
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

#endif
