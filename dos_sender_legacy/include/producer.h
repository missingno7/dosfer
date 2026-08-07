#ifndef PRODUCER_H
#define PRODUCER_H

#include "dosfer.h"

#define DOSFER_DISK_BUFFER 16384

typedef struct {
    FILE *manifest;
    FILE *source;
    ManifestEntry entry;
    int state;
    int finished;
    u32 record_id;
    u32 global_index;
    u32 file_offset;
    u32 file_crc;
    u32 file_count;
    u32 dir_count;
    u32 total_bytes;
    u8 disk[2][DOSFER_DISK_BUFFER];
    u16 disk_len[2];
    u16 disk_pos[2];
    int disk_slot;
    int io_error;
} Producer;

typedef struct {
    u32 files;
    u32 dirs;
    u32 bytes;
} SelectionStats;

int manifest_add_selection(FILE *manifest,const char *path,SelectionStats *stats);

void producer_init(Producer *producer,FILE *manifest);
void producer_close(Producer *producer);
int producer_fill_window(Producer *producer,Window *window,const Config *cfg,
                         u32 session,u32 window_id);
void producer_free_window(Window *window);

#endif
