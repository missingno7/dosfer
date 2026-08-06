#ifndef VGA_H
#define VGA_H

#include "dosfer.h"

/* Fixed V40, 1-pixel/module, VGA Mode 0Dh backend. */
int vga_enter(VideoMode mode);
void vga_leave(void);
void vga_wait_retrace(void);

int vga_show_qr(const u8 *qr, int invert, const char *status);
int vga_show_qr_stream(const u8 *qr, const u8 *codewords, u16 codeword_len,
                       int invert, const char *status, int delta_only);

int vga_delta_ready(void);
void vga_delta_stats(u16 *bits);
u32 vga_screen_hash(void);
int vga_display_matches(void);

void speaker_beep(void);
void vga_benchmark_qr(const u8 *qr, int loops,
                      u32 *build_ms, u32 *copy_ms, u32 *text_ms);
void vga_benchmark_delta(const u8 *codewords, u16 codeword_len, int loops,
                         u32 *update_ms, u32 *copy_ms, u32 *text_ms);

#endif
