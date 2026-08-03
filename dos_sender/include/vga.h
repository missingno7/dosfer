#ifndef VGA_H
#define VGA_H
#include "dosfer.h"
int vga_enter(void);
void vga_use_320(int enabled);
void vga_leave(void);
void vga_wait_retrace(void);
int vga_show_qr(const u8 *qr, int qr_size, int module_pixels, int invert,
                const char *line1, const char *line2, int marker);
int vga_show_qr_stream(const u8 *qr,const u8 *codewords,u16 codeword_len,
                int qr_size,int module_pixels,int invert,const char *line1,
                const char *line2,int marker,int delta_only);
int vga_delta_ready(void);
u32 vga_screen_hash(void);
int vga_display_matches(void);
void speaker_beep(void);
void vga_benchmark_qr(const u8 *qr, int qr_size, int module_pixels,
                      int loops, u32 *build_ms, u32 *copy_ms, u32 *text_ms);
void vga_benchmark_delta(const u8 *codewords,u16 codeword_len,int qr_size,int loops,
                      u32 *update_ms,u32 *copy_ms,u32 *text_ms);
#endif
