#ifndef VGA_H
#define VGA_H
#include "dosfer.h"
int vga_enter(void);
void vga_use_320(int enabled);
void vga_leave(void);
void vga_wait_retrace(void);
/* Mode-0Dh setup which never presents a transport symbol. */
int vga_show_qr(const u8 *qr, int qr_size, int module_pixels, int invert,
                const char *line1, const char *line2, int marker);
int vga_show_qr_stream(const u8 *qr,const u8 *codewords,u16 codeword_len,
                int qr_size,int module_pixels,int invert,const char *line1,
                const char *line2,int marker,int delta_only);
int vga_delta_ready(void);
void vga_delta_stats(u16 *groups,u16 *types,int *rotation);
u32 vga_screen_hash(void);
int vga_display_matches(void);
/* Resident Mode-0Dh plane backend. Preparation may render and upload; once a
 * group is resident, vga_plane_show_mask() changes registers only. */
int vga_plane_begin(u16 *page_step);
void vga_plane_end(void);
int vga_plane_store_qr(const u8 *qr,const u8 *codewords,u16 codeword_len,
                       int qr_size,int invert,u8 plane,u8 slot,int delta_only);
int vga_plane_prepare_correction(const u8 *zero_qr,const u8 *zero_codewords,
                                 const u8 *correction_codewords,u16 codeword_len,
                                 int qr_size,int invert,u8 far *correction_raster);
int vga_plane_show_mask(u16 start,u8 mask);
int vga_plane_apply_correction(u8 slot,u8 plane,const u8 far *correction_raster,
                               u32 *restore_hash);
int vga_plane_restore_correction(u8 slot,u8 plane,const u8 far *correction_raster,
                                 u32 restore_hash);
#ifdef DOSFER_PROFILE
/* raster construction, upload, readback, selection+retrace, correction
 * apply, correction restore (all in PIT ticks). */
extern u32 dosferPlaneVgaProfileTicks[6];
#endif
void speaker_beep(void);
void vga_benchmark_qr(const u8 *qr, int qr_size, int module_pixels,
                      int loops, u32 *build_ms, u32 *copy_ms, u32 *text_ms);
void vga_benchmark_delta(const u8 *codewords,u16 codeword_len,int qr_size,int loops,
                      u32 *update_ms,u32 *copy_ms,u32 *text_ms);
void vga_benchmark_planes(u32 *store_ms,u32 *burst_ms,u32 *vblank_ms,
                          u16 *page_step,int *verified);
int vga_benchmark_plane_batch4(const u8 *first_qr,const u8 *codewords,
                                u16 codeword_len,int qr_size,int invert,
                                u32 *render_ms,u32 *upload_ms,
                                u32 *playback_ms,int *verified);
int vga_benchmark_plane_batch4_steady(const u8 *codewords,u16 codeword_len,
                                       u32 *render_ms,u32 *upload_ms,
                                       u32 *playback_ms,int *verified);
int vga_verify_plane_xor3(const u8 *expected_qr,int qr_size,int invert,
                           u8 plane_mask,int *color_plane_enable_ok);
/* With four basis QR bitplanes already resident in slot zero, temporarily
 * XOR the correction raster into plane 3 and verify the all-plane parity
 * display against expected_qr.  The correction is then XORed back. */
int vga_verify_plane_xor4_correction(const u8 *expected_qr,const u8 *correction_qr,
                                     int qr_size,int invert,int *restored_ok,
                                     int *color_plane_enable_ok);
/* Build Q(delta) from a cached zero-symbol raster and codeword delta.  This
 * is the no-generic-QR-render path used by PLANE_CODED parity. */
int vga_verify_affine_correction(const u8 *zero_qr,const u8 *zero_codewords,
                                 const u8 *correction_codewords,const u8 *expected_qr,
                                 u16 codeword_len,int qr_size,int invert,u16 *changed_codewords);
#endif
