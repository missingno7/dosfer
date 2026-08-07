#ifndef VGA_H
#define VGA_H

#include "dosfer.h"

/* Fixed V40, 1-pixel/module, VGA Mode 0Dh backend. */
int vga_enter(VideoMode mode);
void vga_leave(void);

/* Canonical bootstrap/full-redraw path.  `qr` is the packed 177x177 matrix;
 * `codewords` is already the current 3706-byte stream owned by the sender. */
int vga_show_full_qr_at(const u8 *qr, const u8 *codewords,
                        int invert, const char *status, u32 earliest_tick);

/* Steady-state paths.  They update the persistent RAM shadow raster and the
 * caller-owned current codeword stream without materializing a second full
 * codeword buffer. */
int vga_apply_v40l_delta(const u8 *data_codewords, const u8 *ecc_blocks,
                         u8 *current_codewords);
int vga_apply_codeword_delta(const u8 *next_codewords,
                             u8 *current_codewords);
int vga_show_prepared_at(int invert, const char *status, u32 earliest_tick);

u32 vga_last_flip_tick(void);
int vga_delta_ready(void);
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
