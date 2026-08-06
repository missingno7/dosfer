#ifndef DOSFER32_PIPELINE_H
#define DOSFER32_PIPELINE_H

#include <stdint.h>
#include "platform_vga.h"

/* ========================================================================
 * DOSFER32 Fully Buffered Producer/Playback Pipeline
 *
 *   2 x 32 KiB disk read-ahead buffers
 *       ↓
 *   2 x InputGroup
 *       ↓
 *   one persistent V40 QR worker
 *       ↓
 *   4 x PreparedGroup RAM buffers
 *       ↓
 *   8 x VGA PLANE4 group slots
 *       ↓
 *   absolute-deadline 50 ms display scheduler
 *
 * All buffers are allocated once before streaming.  No malloc/free or
 * file open/close is allowed in the steady-state loop.
 * ======================================================================== */

/* ---- Constants ---- */

#define PL_DISK_SLOTS    2u
#define PL_DISK_SLOT_BYTES 32768u  /* 32 KiB per slot */

#define PL_INPUT_SLOTS   2u
#define PL_INPUT_BASIS   4u        /* C1, C2, C4, C8 */
#define PL_INPUT_BASIS_BYTES 2956u
#define PL_INPUT_GROUP_BYTES (PL_INPUT_BASIS * PL_INPUT_BASIS_BYTES) /* 11,824 */

#define PL_PREP_SLOTS    4u
#define PL_VGA_SLOTS     8u

#define PL_DISPLAY_INTERVAL_MS 50u
#define PL_DISPLAY_FPS  20u

/* ---- State enums ---- */

typedef enum {
    DS_EMPTY = 0,
    DS_FILLING,
    DS_READY,
    DS_CONSUMING
} DiskState;

typedef enum {
    IS_FREE = 0,
    IS_BUILDING,
    IS_READY,
    IS_PROCESSING
} InputState;

typedef enum {
    PS_FREE = 0,
    PS_FILLING,
    PS_READY,
    PS_UPLOADING,
    PS_RESIDENT
} PrepState;

typedef enum {
    VS_FREE = 0,
    VS_UPLOADING,
    VS_READY,
    VS_PLAYING,
    VS_RESTORING
} VgaState;

/* ---- Disk buffer ---- */

typedef struct {
    uint8_t *buf;                /* 32 KiB */
    unsigned len;                /* bytes valid */
    unsigned pos;                /* consume cursor */
    DiskState state;
} DiskBuf;

/* ---- InputGroup ---- */

typedef struct {
    /* Four complete 2956-byte V40-L inputs (C1, C2, C4, C8) */
    uint8_t inputs[PL_INPUT_BASIS][PL_INPUT_BASIS_BYTES];
    /* Per-basis frame headers (48 bytes each) */
    uint8_t headers[PL_INPUT_BASIS][DOS32_FRAME_HEADER];
    /* Header XOR (CF parity header) */
    uint8_t header_xor[DOS32_FRAME_HEADER];
    /* Group metadata */
    uint32_t group_global;
    uint32_t window;
    uint16_t window_index;
    uint16_t window_count;
    uint8_t  width;
    uint8_t  overlap;
    /* Whitening keystream for this group */
    uint8_t  keystream[DOS32_FRAME_PAYLOAD];
    InputState state;
} InputGroup;

/* ---- PreparedGroup (RAM) ---- */

typedef struct {
    /* Four completed basis rasters (C1, C2, C4, C8) */
    uint8_t rasters[4][VGA_RASTER_BYTES];
    /* Sparse CF correction patches */
    uint16_t corr_offset[VGA_PLANE_MAX_CORRECTION_PATCHES];
    uint8_t  corr_xor[VGA_PLANE_MAX_CORRECTION_PATCHES];
    uint16_t corr_count;
    uint8_t  corr_plane;
    /* Group metadata */
    uint32_t group_global;
    uint32_t ordinal;
    uint8_t  width;
    uint8_t  overlap;
    uint32_t window;
    uint16_t window_index;
    uint16_t window_count;
    /* Basis hashes for verify */
    uint32_t basis_hash[4];
    PrepState state;
} PreparedGroup;

/* ---- VGA slot descriptor ---- */

typedef struct {
    uint8_t  state;              /* VgaState */
    uint8_t  slot;               /* physical VGA slot index 0..7 */
    uint8_t  width;
    uint8_t  correction_applied;
    uint8_t  correction_plane;
    uint16_t correction_patch_count;
    uint16_t correction_patch_offset[VGA_PLANE_MAX_CORRECTION_PATCHES];
    uint8_t  correction_patch_xor[VGA_PLANE_MAX_CORRECTION_PATCHES];
    uint32_t group_global;
    uint32_t ordinal;
    uint32_t resident_hash;
    uint32_t correction_restore_hash;
} VgaSlot;

/* ---- Pipeline ---- */

typedef struct {
    /* Disk layer */
    DiskBuf disk[PL_DISK_SLOTS];
    /* Input layer */
    InputGroup input[PL_INPUT_SLOTS];
    /* Prepared layer */
    PreparedGroup prep[PL_PREP_SLOTS];
    /* VGA layer */
    VgaSlot vga[PL_VGA_SLOTS];
    /* Indices */
    uint8_t disk_head;           /* next slot to fill */
    uint8_t input_head;          /* next slot to build */
    uint8_t prep_head;           /* next slot to fill */
    uint8_t vga_head;            /* next slot to play */
    /* Counts */
    unsigned disk_ready;
    unsigned input_ready;
    unsigned prep_ready;
    unsigned vga_ready;
    /* Scheduler */
    uint32_t start_ticks;
    uint32_t next_deadline;      /* in ticks */
    unsigned display_interval;   /* current interval index */
    /* Underrun tracking */
    unsigned underrun_count;
    unsigned duplicate_count;
} Pipeline;

/* ---- Stage functions (each performs bounded work) ---- */

/* Refill one EMPTY disk buffer from file. Returns 1 if work done. */
int disk_refill_step(Pipeline *pl, FILE *file);

/* Consume disk data and build one InputGroup. Returns 1 if work done. */
int input_builder_step(Pipeline *pl, RecordStream *s);

/* Process one bounded RS-block chunk. Returns 1 if work done. */
int v40_worker_step(Pipeline *pl, WorkMemory *w, Vga32 *vga, int verify);

/* Upload one READY PreparedGroup to a FREE VGA slot. Returns 1 if work done. */
int prepared_upload_step(Pipeline *pl, WorkMemory *w, Vga32 *vga);

/* Display scheduler: show next symbol at 50ms deadline. Returns 1 if displayed. */
int playback_step(Pipeline *pl, Vga32 *vga);

/* ---- Pipeline lifecycle ---- */

int pipeline_init(Pipeline *pl, WorkMemory *w, Vga32 *vga);
void pipeline_shutdown(Pipeline *pl);

/* ---- Metrics ---- */

typedef struct {
    uint32_t disk_refill_ticks;
    uint32_t input_build_ticks;
    uint32_t qr_worker_ticks;
    uint32_t upload_ticks;
    uint32_t playback_ticks;
    uint32_t wall_ticks;
    uint32_t disk_refill_calls;
    uint32_t input_build_calls;
    uint32_t qr_worker_calls;
    uint32_t upload_calls;
    uint32_t playback_calls;
    uint32_t underrun_count;
    uint32_t duplicate_count;
    uint32_t min_prep_ready;
    uint32_t min_vga_ready;
    /* Display interval tracking */
    uint32_t min_interval_ticks;
    uint32_t max_interval_ticks;
    uint32_t intervals_outside_tolerance;
    uint32_t burst_catchup_transitions;
    /* Sustained rates */
    uint32_t basis_qr_generated;
    uint32_t symbols_displayed;
} PipelineMetrics;

#endif /* DOSFER32_PIPELINE_H */
